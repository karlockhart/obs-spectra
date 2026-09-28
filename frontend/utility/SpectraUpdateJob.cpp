#include "SpectraUpdateJob.hpp"
#include "SpectraZip.hpp"

#include "../spectra-updater/updater-common.hpp"

#include <curl/curl.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>

#include "moc_SpectraUpdateJob.cpp"

using namespace SpectraReleases;

static constexpr const char *USER_AGENT = "OBS-Spectra-Updater";
/* A stalled download gives up after this many seconds below 1 byte/s */
static constexpr long STALL_SECONDS = 60;
/* Room needed next to the install, as a multiple of the zip's size: the zip
 * plus the extracted files */
static constexpr int SPACE_FACTOR = 4;
/* How often progress reaches the dialog */
static constexpr auto REPORT_INTERVAL = std::chrono::milliseconds(100);

static std::wstring W(const QString &path)
{
	return QDir::toNativeSeparators(path).toStdWString();
}

SpectraUpdateJob::SpectraUpdateJob(const QString &installDir_, const Download &download_, QObject *parent)
	: QObject(parent),
	  installDir(QDir::cleanPath(installDir_)),
	  download(download_)
{
}

SpectraUpdateJob::~SpectraUpdateJob()
{
	cancelled = true;
	if (worker.joinable()) {
		worker.join();
	}
}

QString SpectraUpdateJob::RunningInstallDir()
{
	QFileInfo exe(QCoreApplication::applicationFilePath());
	QDir dir = exe.absoluteDir(); /* bin\64bit */
	if (exe.fileName().compare(QString::fromWCharArray(SpectraUpdater::APP_EXE), Qt::CaseInsensitive) != 0 ||
	    dir.dirName().compare("64bit", Qt::CaseInsensitive) != 0 || !dir.cdUp() ||
	    dir.dirName().compare("bin", Qt::CaseInsensitive) != 0 || !dir.cdUp()) {
		return QString();
	}
	return QDir::cleanPath(dir.absolutePath());
}

bool SpectraUpdateJob::IsPortableInstall(const QString &installDir)
{
	for (const wchar_t *marker : SpectraUpdater::PORTABLE_MARKERS) {
		if (QFileInfo::exists(QDir(installDir).filePath(QString::fromWCharArray(marker)))) {
			return true;
		}
	}
	return false;
}

static bool UpdaterRunning()
{
	HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, SpectraUpdater::MUTEX_NAME);
	if (mutex) {
		CloseHandle(mutex);
		return true;
	}
	return false;
}

void SpectraUpdateJob::CleanUpLeftovers(const QString &installDir)
{
	if (installDir.isEmpty()) {
		return;
	}
	std::wstring install = W(QDir::cleanPath(installDir));
	std::wstring work = SpectraUpdater::WorkDir(install);
	if (!SpectraUpdater::Exists(work) || UpdaterRunning()) {
		return;
	}
	if (!SpectraUpdater::Exists(SpectraUpdater::Join(work, SpectraUpdater::WORK_MARKER)) ||
	    SpectraUpdater::HoldsSettings(SpectraUpdater::BackupDir(install))) {
		return;
	}
	/* a download or a staged version: another instance may be updating right now
	 * (the next update's Prepare() clears it if not) */
	WIN32_FIND_DATAW fd;
	HANDLE zip = FindFirstFileW(SpectraUpdater::Join(work, L"*.zip").c_str(), &fd);
	if (zip != INVALID_HANDLE_VALUE) {
		FindClose(zip);
		return;
	}
	if (SpectraUpdater::Exists(SpectraUpdater::StagingDir(install))) {
		return;
	}
	SpectraUpdater::RemoveTree(work);
}

QString SpectraUpdateJob::QuoteArguments(const QStringList &args)
{
	/* the rules CommandLineToArgvW and the C runtime parse by */
	QStringList quoted;
	for (const QString &arg : args) {
		if (!arg.isEmpty() && !arg.contains(QRegularExpression("[\\s\"]"))) {
			quoted.append(arg);
			continue;
		}
		QString out = "\"";
		int backslashes = 0;
		for (QChar c : arg) {
			if (c == '\\') {
				backslashes++;
				continue;
			}
			if (c == '"') {
				out += QString(backslashes * 2 + 1, '\\');
			} else {
				out += QString(backslashes, '\\');
			}
			backslashes = 0;
			out += c;
		}
		out += QString(backslashes * 2, '\\');
		out += '"';
		quoted.append(out);
	}
	return quoted.join(' ');
}

QString SpectraUpdateJob::WorkDir() const
{
	return installDir + QString::fromWCharArray(SpectraUpdater::WORK_SUFFIX);
}

QString SpectraUpdateJob::StagingDir() const
{
	return QDir(WorkDir()).filePath(QString::fromWCharArray(SpectraUpdater::STAGING_NAME));
}

QString SpectraUpdateJob::LogPath() const
{
	return installDir + QString::fromWCharArray(SpectraUpdater::LOG_SUFFIX);
}

/* Makes an empty work folder next to the install. Empty result on success. */
QString SpectraUpdateJob::Prepare()
{
	if (installDir.isEmpty() || !QFileInfo::exists(QDir(installDir).filePath("bin/64bit/obs-spectra.exe"))) {
		return tr("This copy of OBS-Spectra isn't in a folder it can update.");
	}
	if (UpdaterRunning()) {
		return tr("An update is already being installed.");
	}

	std::wstring install = W(installDir);
	std::wstring work = SpectraUpdater::WorkDir(install);
	if (SpectraUpdater::Exists(work)) {
		if (!SpectraUpdater::Exists(SpectraUpdater::Join(work, SpectraUpdater::WORK_MARKER))) {
			return tr("%1 is in the way: it isn't OBS-Spectra's. Move it elsewhere and try again.")
				.arg(QDir::toNativeSeparators(WorkDir()));
		}
		if (SpectraUpdater::HoldsSettings(SpectraUpdater::BackupDir(install))) {
			return tr("An earlier update didn't finish and left a previous version with settings in %1. "
				  "Check it (%2 says what happened), move back what you need and delete it.")
				.arg(QDir::toNativeSeparators(
					     QString::fromStdWString(SpectraUpdater::BackupDir(install))),
				     QDir::toNativeSeparators(LogPath()));
		}
		if (!SpectraUpdater::RemoveTree(work)) {
			return tr("Could not delete %1, left by an earlier update.")
				.arg(QDir::toNativeSeparators(WorkDir()));
		}
	}

	if (!QDir().mkpath(WorkDir())) {
		return tr("OBS-Spectra can't create %1: it can only update itself where it may write next to its "
			  "folder. Download the new version from the release page instead.")
			.arg(QDir::toNativeSeparators(WorkDir()));
	}
	ownsWorkDir = true;
	QFile marker(QDir(WorkDir()).filePath(QString::fromWCharArray(SpectraUpdater::WORK_MARKER)));
	if (!marker.open(QIODevice::WriteOnly) || marker.write(SpectraUpdater::WORK_MARKER_TEXT) < 0) {
		return tr("OBS-Spectra can't write to %1. Download the new version from the release page instead.")
			.arg(QDir::toNativeSeparators(WorkDir()));
	}
	marker.close();

	QStorageInfo storage(WorkDir());
	qint64 needed = download.zip.size * SPACE_FACTOR;
	if (storage.isValid() && download.zip.size > 0 && storage.bytesAvailable() < needed) {
		return tr("Not enough free disk space next to OBS-Spectra: the update needs %1 MB.")
			.arg(needed / (1024 * 1024));
	}
	return QString();
}

void SpectraUpdateJob::Start()
{
	emit Progress(Stage::Preparing, 0, 0);
	if (download.checksum.name.isEmpty()) {
		Complete(tr("This release has no checksum file, so its download can't be verified."));
		return;
	}
	QString error = Prepare();
	if (!error.isEmpty()) {
		Complete(error);
		return;
	}
	worker = std::thread([this]() {
		QString result = Work();
		QMetaObject::invokeMethod(this, [this, result]() { Complete(result); }, Qt::QueuedConnection);
	});
}

void SpectraUpdateJob::Report(Stage stage, qint64 doneBytes, qint64 total)
{
	QMetaObject::invokeMethod(
		this, [this, stage, doneBytes, total]() { emit Progress(stage, doneBytes, total); },
		Qt::QueuedConnection);
}

namespace {

struct Transfer {
	std::atomic<bool> *cancelled = nullptr;
	FILE *file = nullptr; /* with hash, or else into text */
	QCryptographicHash *hash = nullptr;
	std::string text;
	bool writeFailed = false;
	std::function<void(qint64, qint64)> progress;
	std::chrono::steady_clock::time_point last;
};

size_t WriteData(char *ptr, size_t size, size_t count, void *userdata)
{
	Transfer *t = static_cast<Transfer *>(userdata);
	size_t len = size * count;
	if (t->file) {
		if (fwrite(ptr, 1, len, t->file) != len) {
			t->writeFailed = true;
			return 0;
		}
		t->hash->addData(QByteArrayView(ptr, (qsizetype)len));
	} else {
		/* a checksum file is tiny: anything big is not one */
		if (t->text.size() + len > (1 << 20)) {
			return 0;
		}
		t->text.append(ptr, len);
	}
	return len;
}

int TransferProgress(void *userdata, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
{
	Transfer *t = static_cast<Transfer *>(userdata);
	if (t->cancelled->load()) {
		return 1;
	}
	auto clock = std::chrono::steady_clock::now();
	if (t->progress && clock - t->last > REPORT_INTERVAL) {
		t->last = clock;
		t->progress((qint64)now, (qint64)total);
	}
	return 0;
}

/* Downloads url into the transfer. Empty on success, "cancelled", or the error. */
QString Fetch(const QString &url, Transfer &t)
{
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl) {
		return QStringLiteral("libcurl failed to start");
	}
	char error[CURL_ERROR_SIZE] = {};
	QByteArray u = url.toUtf8();
	CURL *c = curl.get();
	curl_easy_setopt(c, CURLOPT_URL, u.constData());
	/* GitHub's download links redirect to its storage; https only (file for tests) */
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,file");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteData);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, TransferProgress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, STALL_SECONDS);

	CURLcode rc = curl_easy_perform(c);
	if (t.cancelled->load()) {
		return QStringLiteral("cancelled");
	}
	if (rc == CURLE_OK) {
		return QString();
	}
	return QString::fromUtf8(error[0] ? error : curl_easy_strerror(rc)).trimmed();
}

} // namespace

/* On the worker thread */
QString SpectraUpdateJob::Work()
{
	const QString cancelledText = QStringLiteral("cancelled");

	Transfer sum;
	sum.cancelled = &cancelled;
	QString error = Fetch(download.checksum.url, sum);
	if (error == cancelledText || cancelled) {
		return cancelledText;
	}
	if (!error.isEmpty()) {
		return tr("Could not download %1: %2").arg(download.checksum.name, error);
	}
	QString expected = ParseSha256(QByteArray::fromStdString(sum.text));
	if (expected.isEmpty()) {
		return tr("%1 doesn't hold a SHA-256 checksum.").arg(download.checksum.name);
	}

	QString zipPath = QDir(WorkDir()).filePath(download.zip.name);
	FILE *file = _wfopen(W(zipPath).c_str(), L"wb");
	if (!file) {
		return tr("Could not write %1.").arg(QDir::toNativeSeparators(zipPath));
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	Transfer zip;
	zip.cancelled = &cancelled;
	zip.file = file;
	zip.hash = &hash;
	zip.progress = [this](qint64 got, qint64 total) {
		Report(Stage::Downloading, got, total > 0 ? total : download.zip.size);
	};
	Report(Stage::Downloading, 0, download.zip.size);
	error = Fetch(download.zip.url, zip);
	bool closeFailed = fclose(file) != 0;
	if (error == cancelledText || cancelled) {
		return cancelledText;
	}
	if (zip.writeFailed || closeFailed) {
		return tr("Could not write %1 (disk full?)").arg(QDir::toNativeSeparators(zipPath));
	}
	if (!error.isEmpty()) {
		return tr("The download failed: %1").arg(error);
	}

	Report(Stage::Verifying, 0, 0);
	qint64 size = QFileInfo(zipPath).size();
	if (download.zip.size > 0 && size != download.zip.size) {
		return tr("The download is incomplete (%1 of %2 bytes). Try again.").arg(size).arg(download.zip.size);
	}
	if (QString::fromLatin1(hash.result().toHex()) != expected) {
		return tr("The download doesn't match its checksum (%1). Try again.").arg(download.checksum.name);
	}

	Report(Stage::Extracting, 0, 0);
	auto last = std::chrono::steady_clock::now();
	std::string zipError =
		SpectraZip::Extract(W(zipPath), W(StagingDir()), [this, &last](uint64_t got, uint64_t total) {
			auto now = std::chrono::steady_clock::now();
			if (now - last > REPORT_INTERVAL || got == total) {
				last = now;
				Report(Stage::Extracting, (qint64)got, (qint64)total);
			}
			return !cancelled.load();
		});
	QFile::remove(zipPath);
	if (zipError == "cancelled" || cancelled) {
		return cancelledText;
	}
	if (!zipError.empty()) {
		return tr("Could not extract the update: %1").arg(QString::fromStdString(zipError));
	}
	if (!QFileInfo::exists(QDir(StagingDir()).filePath("bin/64bit/obs-spectra.exe"))) {
		return tr("The download isn't an OBS-Spectra package (no bin\\64bit\\obs-spectra.exe).");
	}
	return QString();
}

void SpectraUpdateJob::Complete(const QString &error)
{
	if (worker.joinable()) {
		worker.join();
	}
	if (done) {
		return;
	}
	done = true;
	if (error.isEmpty()) {
		emit Progress(Stage::Ready, 1, 1);
		emit Finished(true, QString());
	} else {
		Discard();
		emit Finished(false, error);
	}
}

void SpectraUpdateJob::Cancel()
{
	cancelled = true;
}

void SpectraUpdateJob::Discard()
{
	if (worker.joinable()) {
		cancelled = true;
		worker.join();
	}
	std::wstring install = W(installDir);
	std::wstring work = SpectraUpdater::WorkDir(install);
	if (ownsWorkDir && SpectraUpdater::Exists(SpectraUpdater::Join(work, SpectraUpdater::WORK_MARKER)) &&
	    !SpectraUpdater::HoldsSettings(SpectraUpdater::BackupDir(install)) && !UpdaterRunning()) {
		ownsWorkDir = false;
		SpectraUpdater::RemoveTree(work);
	}
}

bool SpectraUpdateJob::LaunchHelper(const QString &helperExe, const QString &version, const QString &relaunchArgs,
				    QString *error)
{
	QString copy = QDir(WorkDir()).filePath(QString::fromWCharArray(SpectraUpdater::HELPER_EXE));
	/* run from outside the install, or it would hold the folder it renames */
	QFile::remove(copy);
	if (!QFile::copy(helperExe, copy)) {
		*error = tr("Could not copy %1 to %2.")
				 .arg(QDir::toNativeSeparators(helperExe), QDir::toNativeSeparators(copy));
		return false;
	}

	QStringList args = {"--install",  QDir::toNativeSeparators(installDir),
			    "--staging",  QDir::toNativeSeparators(StagingDir()),
			    "--wait-pid", QString::number(GetCurrentProcessId()),
			    "--version",  version};
	if (!relaunchArgs.isEmpty()) {
		args << "--relaunch-args" << relaunchArgs;
	}
	args << helperExtraArgs;

	std::wstring exe = W(copy);
	std::wstring cmd =
		(QuoteArguments({QDir::toNativeSeparators(copy)}) + " " + QuoteArguments(args)).toStdWString();
	/* its own folder: the install's parent, never inside the install */
	std::wstring cwd = W(QFileInfo(installDir).absolutePath());

	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	/* out of any job Spectra runs in, so closing Spectra doesn't end it */
	BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr,
				 cwd.c_str(), &si, &pi);
	if (!ok) {
		ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si,
				    &pi);
	}
	if (!ok) {
		*error = tr("Could not start the updater (error %1).").arg(GetLastError());
		return false;
	}
	AllowSetForegroundWindow(pi.dwProcessId);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}
