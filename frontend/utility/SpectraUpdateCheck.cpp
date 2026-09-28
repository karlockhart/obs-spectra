#include "SpectraUpdateCheck.hpp"

#include <OBSApp.hpp>
#include <utility/RemoteTextThread.hpp>
#ifdef _WIN32
#include <dialogs/SpectraUpdateDialog.hpp>
#include <utility/SpectraUpdateJob.hpp>
#endif

#include <qt-wrappers.hpp>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

#include <thread>

#include "moc_SpectraUpdateCheck.cpp"

using namespace SpectraReleases;

/* Every release, newest first; /releases/latest would skip the release candidates */
#define SPECTRA_RELEASES_API "https://api.github.com/repos/karlockhart/obs-spectra/releases?per_page=30"
#define UPSTREAM_RELEASES_API "https://api.github.com/repos/obsproject/obs-studio/releases/latest"

/* Longest part of the release notes shown in the update dialog */
static constexpr int MAX_NOTES_CHARS = 1500;

SpectraUpdateCheck::SpectraUpdateCheck(QWidget *parent, bool manual_)
	: QObject(parent),
	  parentWidget(parent),
	  manual(manual_),
	  channel(CurrentChannel())
{
}

Channel SpectraUpdateCheck::CurrentChannel()
{
	const char *setting = config_get_string(App()->GetAppConfig(), "Spectra", "UpdateChannel");
	return ChannelFromSetting(QString::fromUtf8(setting ? setting : ""),
				  QString::fromStdString(App()->GetVersionString(false)));
}

namespace {
struct FetchResult {
	bool ok = false;
	long status = 0;
	std::string body;
	std::string error;
};

FetchResult FetchJson(const char *url)
{
	FetchResult result;
	std::vector<std::string> headers = {"Accept: application/vnd.github+json"};
	result.ok = GetRemoteFile(url, result.body, result.error, &result.status, nullptr, "", nullptr,
				  std::move(headers), nullptr, 15);
	return result;
}
} // namespace

void SpectraUpdateCheck::Start()
{
	QPointer<SpectraUpdateCheck> self(this);
	std::thread([self]() {
		FetchResult spectra = FetchJson(SPECTRA_RELEASES_API);
		FetchResult upstream = FetchJson(UPSTREAM_RELEASES_API);

		QMetaObject::invokeMethod(
			qApp,
			[self, spectra, upstream]() {
				if (!self) {
					return;
				}
				self->ApplySpectra(spectra.ok, spectra.status, spectra.body, spectra.error);
				self->ApplyUpstream(upstream.ok, upstream.status, upstream.body, upstream.error);
				self->Finished();
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraUpdateCheck::ApplySpectra(bool ok, long status, const std::string &body, const std::string &error)
{
	if (!ok) {
		blog(LOG_WARNING, "[Spectra] Update check failed for OBS-Spectra (HTTP %ld): %s", status,
		     error.c_str());
		failed = true;
		return;
	}

	QString parseError;
	QList<Release> releases = ParseReleases(QByteArray::fromStdString(body), &parseError);
	if (!parseError.isEmpty()) {
		blog(LOG_WARNING, "[Spectra] Update check: unexpected answer from GitHub: %s", QT_TO_UTF8(parseError));
		failed = true;
		return;
	}
	release = SelectRelease(releases, channel);
}

void SpectraUpdateCheck::ApplyUpstream(bool ok, long status, const std::string &body, const std::string &error)
{
	if (!ok) {
		blog(LOG_WARNING, "[Spectra] Update check failed for OBS Studio (HTTP %ld): %s", status, error.c_str());
		return;
	}
	QJsonObject json = QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
	upstreamTag = json["tag_name"].toString();
}

void SpectraUpdateCheck::Finished()
{
	config_t *config = App()->GetAppConfig();
	QString currentString = QString::fromStdString(App()->GetVersionString(false));
	Version current = ParseVersion(currentString);
	Version upstream = ParseVersion(upstreamTag);
	QString releaseTag = release ? release->tag : QString();

	QString upstreamLine;
	if (upstream.valid() && Version{current.major, current.minor, current.patch} <
					Version{upstream.major, upstream.minor, upstream.patch}) {
		upstreamLine = QTStr("Spectra.Update.UpstreamNewer").arg(current.Base(), upstream.Base());
		blog(LOG_INFO, "[Spectra] Upstream OBS %s is newer than base %s", QT_TO_UTF8(upstream.Base()),
		     QT_TO_UTF8(current.Base()));
	}

	bool newer = release && current < release->version;
	QString skipped = QString::fromUtf8(config_get_string(config, "Spectra", "SkipVersion"));

	blog(LOG_INFO, "[Spectra] Update check (%s channel): current %s, newest %s, upstream OBS %s",
	     channel == Channel::ReleaseCandidates ? "release candidate" : "stable", QT_TO_UTF8(currentString),
	     releaseTag.isEmpty() ? "(none)" : QT_TO_UTF8(releaseTag),
	     upstreamTag.isEmpty() ? "(unknown)" : QT_TO_UTF8(upstreamTag));

	if (!parentWidget || (!manual && (!newer || skipped == releaseTag))) {
		deleteLater();
		return;
	}
#ifdef _WIN32
	if (SpectraUpdateDialog *updating = SpectraUpdateDialog::Current()) {
		/* an update is already downloading or waiting to be installed */
		updating->raise();
		updating->activateWindow();
		deleteLater();
		return;
	}
#endif

	QMessageBox box(parentWidget);
	box.setWindowTitle(QTStr("Spectra.Update.Title"));

	if (newer) {
		QString notes = release->notes.trimmed();
		if (notes.size() > MAX_NOTES_CHARS) {
			notes = notes.left(MAX_NOTES_CHARS) + "\n...";
		}
		box.setIcon(QMessageBox::Information);
		box.setText(
			QTStr(release->version.prerelease() ? "Spectra.Update.AvailableRC" : "Spectra.Update.Available")
				.arg(releaseTag, currentString));
		box.setDetailedText(notes);

		QPushButton *install = nullptr;
		QString informative = upstreamLine;
#ifdef _WIN32
		/* Installing needs a release build laid out like a release zip, and a
		 * download that can be verified */
		QString installDir = SpectraUpdateJob::RunningInstallDir();
		std::optional<Download> download;
		QString helper = QDir(QCoreApplication::applicationDirPath()).filePath("spectra-updater.exe");
		if (!IsReleaseTag(currentString)) {
			blog(LOG_INFO, "[Spectra] Update can't be installed from here: %s is a developer build",
			     QT_TO_UTF8(currentString));
		} else if (installDir.isEmpty() || !QFileInfo::exists(helper)) {
			blog(LOG_INFO, "[Spectra] Update can't be installed from here: not a release package layout");
		} else if (!(download = SelectDownload(*release, SpectraUpdateJob::IsPortableInstall(installDir))) ||
			   download->checksum.name.isEmpty()) {
			blog(LOG_INFO, "[Spectra] Update can't be installed from here: %s has no verifiable zip",
			     QT_TO_UTF8(releaseTag));
			download.reset();
		}
		if (download) {
			install = box.addButton(QTStr("Spectra.Update.Install"), QMessageBox::AcceptRole);
		} else {
			informative = (informative.isEmpty() ? QString() : informative + "\n\n") +
				      QTStr("Spectra.Update.ManualOnly");
		}
#endif
		box.setInformativeText(informative);

		QPushButton *page = box.addButton(QTStr("Spectra.Update.Download"),
						  install ? QMessageBox::ActionRole : QMessageBox::AcceptRole);
		QPushButton *skip = box.addButton(QTStr("Spectra.Update.Skip"), QMessageBox::DestructiveRole);
		box.addButton(QTStr("Spectra.Update.Later"), QMessageBox::RejectRole);
		box.setDefaultButton(install ? install : page);
		box.exec();

		if (box.clickedButton() == page) {
			QDesktopServices::openUrl(QUrl(release->htmlUrl));
		} else if (box.clickedButton() == skip) {
			config_set_string(config, "Spectra", "SkipVersion", QT_TO_UTF8(releaseTag));
			config_save_safe(config, "tmp", nullptr);
		}
#ifdef _WIN32
		else if (install && box.clickedButton() == install && parentWidget) {
			SpectraUpdateDialog *dialog =
				new SpectraUpdateDialog(parentWidget, *release, *download, installDir, helper);
			dialog->setAttribute(Qt::WA_DeleteOnClose);
			dialog->open();
		}
#endif
	} else {
		box.setIcon(failed ? QMessageBox::Warning : QMessageBox::Information);
		box.setText(failed ? QTStr("Spectra.Update.Failed")
				   : QTStr("Spectra.Update.UpToDate").arg(currentString, current.Base()));
		box.setInformativeText(upstreamLine);
		box.addButton(QMessageBox::Ok);
		box.exec();
	}

	deleteLater();
}
