/*
 * lucida-viewer: Lucida's log browser on its own, installed next to
 * obs-spectra.exe. Reads the chat log (and finds the loop recording) of the
 * current Spectra profile, so the log can be browsed without starting
 * Spectra.
 *
 *   lucida-viewer [--portable] [chatlog.db]
 */

#include "cloud.hpp"
#include "lucida-host.hpp"
#include "lucida-viewer.hpp"

#include <util/config-file.h>
#include <util/platform.h>
#include <util/text-lookup.h>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

#include <memory>

namespace {

/* As the frontend's SPECTRA_CONFIG_DIR: Spectra never shares OBS Studio's */
constexpr const char *kConfigDir = "OBS-Spectra";

struct ConfigDeleter {
	void operator()(config_t *c) const { config_close(c); }
};
using Config = std::unique_ptr<config_t, ConfigDeleter>;

struct LookupDeleter {
	void operator()(lookup_t *l) const { text_lookup_destroy(l); }
};

Config profile;
std::unique_ptr<lookup_t, LookupDeleter> texts;

Config OpenIni(const QString &path)
{
	config_t *c = nullptr;
	if (!QFileInfo::exists(path) || config_open(&c, QDir::toNativeSeparators(path).toUtf8().constData(),
						    CONFIG_OPEN_EXISTING) != CONFIG_SUCCESS) {
		return nullptr;
	}
	return Config(c);
}

QString IniString(config_t *c, const char *section, const char *name)
{
	const char *value = c ? config_get_string(c, section, name) : nullptr;
	return QString::fromUtf8(value ? value : "");
}

/* Where obs-spectra.exe (in bin/64bit) looks for "portable_mode" */
QString InstallRoot()
{
	return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../.."));
}

bool PortableMode(const QStringList &args)
{
	if (args.contains(QStringLiteral("--portable")) || args.contains(QStringLiteral("-p"))) {
		return true;
	}
	const QDir root(InstallRoot());
	for (const char *name : {"portable_mode", "obs_portable_mode", "portable_mode.txt", "obs_portable_mode.txt"}) {
		if (QFileInfo::exists(root.filePath(QString::fromLatin1(name)))) {
			return true;
		}
	}
	return false;
}

/* The folder holding obs-studio/global.ini (GetAppConfigPath in the frontend) */
QString ConfigRoot(bool portable)
{
	if (portable) {
		return QDir(InstallRoot()).filePath(QStringLiteral("config"));
	}
	char path[512];
	if (os_get_config_path(path, sizeof(path), kConfigDir) <= 0) {
		return QString();
	}
	return QDir::cleanPath(QString::fromUtf8(path));
}

/* Opens the current profile's basic.ini, following the frontend's rules for
 * relocated configuration and profiles; also returns the UI language */
Config OpenProfile(bool portable, QString *language)
{
	const QString root = ConfigRoot(portable);
	if (root.isEmpty()) {
		return nullptr;
	}
	Config global = OpenIni(QDir(root).filePath(QStringLiteral("obs-studio/global.ini")));
	auto location = [&](const char *name) {
		const QString moved = portable ? QString() : IniString(global.get(), "Locations", name);
		return !moved.isEmpty() && QFileInfo::exists(moved) ? moved : root;
	};
	Config user = OpenIni(QDir(location("Configuration")).filePath(QStringLiteral("obs-studio/user.ini")));
	*language = IniString(user.get(), "General", "Language");
	if (language->isEmpty()) {
		*language = IniString(global.get(), "General", "Language");
	}
	QString dir = IniString(user.get(), "Basic", "ProfileDir");
	if (dir.isEmpty()) {
		dir = QStringLiteral("Untitled");
	}
	return OpenIni(
		QDir(location("Profiles")).filePath(QStringLiteral("obs-studio/basic/profiles/%1/basic.ini").arg(dir)));
}

void LoadTexts(const QString &language)
{
	const QDir locale(QDir(InstallRoot()).filePath(QStringLiteral("data/obs-plugins/spectra-lucida/locale")));
	texts.reset(text_lookup_create(locale.filePath(QStringLiteral("en-US.ini")).toUtf8().constData()));
	if (texts && !language.isEmpty() && language != QStringLiteral("en-US")) {
		const QString file = locale.filePath(language + QStringLiteral(".ini"));
		if (QFileInfo::exists(file)) {
			text_lookup_add(texts.get(), file.toUtf8().constData());
		}
	}
}

QString Profile(const char *section, const char *name)
{
	return IniString(profile.get(), section, name);
}

/* As lucida::Settings: [Lucida] DbPath, else chatlog.db in Spectra's Lucida folder */
QString ChatLogPath()
{
	const QString db = Profile("Lucida", "DbPath");
	if (!db.isEmpty()) {
		return QDir::cleanPath(db);
	}
	QString folder = Profile("Spectra", "LucidaPath");
	if (folder.isEmpty()) {
		folder = QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
				 .filePath(QStringLiteral("Spectra/Lucida"));
	}
	return QDir(QDir::cleanPath(folder)).filePath(QStringLiteral("chatlog.db"));
}

/* As lucida::LoopDirectory, with the recording path OBS would use */
QString LoopDirectory()
{
	const QString loop = Profile("SpectraLoop", "Path");
	if (!loop.isEmpty()) {
		return QDir::cleanPath(loop);
	}
	QString output;
	if (Profile("Output", "Mode") == QStringLiteral("Advanced")) {
		output = Profile("AdvOut", Profile("AdvOut", "RecType") == QStringLiteral("FFmpeg") ? "FFFilePath"
												    : "RecFilePath");
	} else {
		output = Profile("SimpleOutput", "FilePath");
	}
	if (output.isEmpty()) {
		output = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	}
	return QDir(QDir::cleanPath(output)).filePath(QStringLiteral("Spectra Loop"));
}

} // namespace

QString lucida::Text(const char *key)
{
	const char *value = nullptr;
	if (texts && text_lookup_getstr(texts.get(), key, &value)) {
		return QString::fromUtf8(value);
	}
	return QString::fromUtf8(key);
}

QString lucida::ProfileString(const char *section, const char *name)
{
	return Profile(section, name);
}

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("Lucida Viewer"));
	QApplication::setOrganizationName(QStringLiteral("OBS-Spectra"));

	QStringList args = QApplication::arguments();
	args.removeFirst();
	QString language;
	profile = OpenProfile(PortableMode(args), &language);
	LoadTexts(language);

	QString db;
	for (const QString &arg : args) {
		if (!arg.startsWith(QLatin1Char('-'))) {
			db = QFileInfo(arg).absoluteFilePath();
		}
	}
	if (db.isEmpty()) {
		db = ChatLogPath();
	}
	/* Opening creates a log, so never open one that is not there */
	if (!QFileInfo::exists(db)) {
		db = QFileDialog::getOpenFileName(
			nullptr, lucida::Text("Lucida.Viewer.OpenLog"), QFileInfo(db).absolutePath(),
			QStringLiteral("%1 (*.db)").arg(lucida::Text("Lucida.Viewer.ChatLog")));
		if (db.isEmpty()) {
			return 0;
		}
	}

	auto store = std::make_shared<lucida::Store>(db);
	QString error;
	if (!store->Open(&error) || !store->IsOpen()) {
		QMessageBox::critical(
			nullptr, lucida::Text("Lucida.Viewer.Title"),
			lucida::Text("Lucida.Viewer.OpenFailed").arg(QDir::toNativeSeparators(db), error));
		return 1;
	}

	const QString loopDir = LoopDirectory();
	/* the Cloud toggle reads Prisma with the same credentials Spectra backs up with */
	const QString credentials =
		lucida::CloudCredentialsPath(Profile("Lucida", "CloudCredentials"),
					     {QFileInfo(db).absolutePath(), Profile("Spectra", "LucidaPath")});
	std::shared_ptr<lucida::PrismaClient> cloud;
	if (!credentials.isEmpty()) {
		if (std::optional<lucida::PrismaCredentials> creds = lucida::PrismaCredentials::FromFile(credentials)) {
			cloud = std::make_shared<lucida::PrismaClient>(*creds);
		}
	}
	lucida::ViewerSource source{[store]() { return store.get(); },
				    [loopDir](const lucida::LogLine &line) -> std::optional<lucida::VideoSpot> {
					    if (line.video && QFileInfo::exists(line.video->path)) {
						    return line.video;
					    }
					    if (line.firstSeen <= 0) {
						    return std::nullopt;
					    }
					    /* Spectra may be recording, but the newest segment
					     * then ends where its file was last written */
					    return lucida::LocateVideo(loopDir, line.firstSeen, false);
				    },
				    nullptr, [cloud]() { return cloud; }};
	auto *viewer = new lucida::Viewer(std::move(source));
	viewer->setWindowTitle(
		QStringLiteral("%1 - %2").arg(lucida::Text("Lucida.Viewer.Title"), QDir::toNativeSeparators(db)));
	viewer->show();
	return app.exec();
}
