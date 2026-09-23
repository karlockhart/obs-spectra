#include "SpectraUpdateCheck.hpp"

#include <climits>

#include <OBSApp.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>

#include <thread>
#include <tuple>

#include "moc_SpectraUpdateCheck.cpp"

#define SPECTRA_RELEASES_API "https://api.github.com/repos/karlockhart/obs-spectra/releases/latest"
#define UPSTREAM_RELEASES_API "https://api.github.com/repos/obsproject/obs-studio/releases/latest"

/* Longest part of the release notes shown in the update dialog */
static constexpr int MAX_NOTES_CHARS = 1500;

QString SpectraUpdateCheck::Version::Base() const
{
	return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

bool SpectraUpdateCheck::Version::operator<(const Version &other) const
{
	/* a final release (rc 0) sorts after its release candidates */
	const int rank = rc ? rc : INT_MAX;
	const int otherRank = other.rc ? other.rc : INT_MAX;
	return std::tie(major, minor, patch, spectra, rank) <
	       std::tie(other.major, other.minor, other.patch, other.spectra, otherRank);
}

SpectraUpdateCheck::Version SpectraUpdateCheck::Parse(const QString &version)
{
	Version v;
	QRegularExpressionMatch base = QRegularExpression("(\\d+)\\.(\\d+)\\.(\\d+)").match(version);
	if (base.hasMatch()) {
		v.major = base.captured(1).toInt();
		v.minor = base.captured(2).toInt();
		v.patch = base.captured(3).toInt();
	}
	QRegularExpressionMatch spectra = QRegularExpression("-spectra\\.(\\d+)").match(version);
	if (spectra.hasMatch()) {
		v.spectra = spectra.captured(1).toInt();
	}
	QRegularExpressionMatch rc = QRegularExpression("-rc\\.?(\\d+)").match(version);
	if (rc.hasMatch()) {
		v.rc = rc.captured(1).toInt();
	}
	return v;
}

SpectraUpdateCheck::SpectraUpdateCheck(QWidget *parent, bool manual_)
	: QObject(parent),
	  parentWidget(parent),
	  manual(manual_)
{
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
				self->Apply(spectra.ok, spectra.status, spectra.body, spectra.error, true);
				self->Apply(upstream.ok, upstream.status, upstream.body, upstream.error, false);
				self->Finished();
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraUpdateCheck::Apply(bool ok, long status, const std::string &body, const std::string &error,
			       bool spectraRelease)
{
	if (!ok) {
		/* No release published yet is not an error worth reporting */
		if (!(spectraRelease && status == 404)) {
			blog(LOG_WARNING, "[Spectra] Update check failed for %s (HTTP %ld): %s",
			     spectraRelease ? "OBS-Spectra" : "OBS Studio", status, error.c_str());
			if (spectraRelease) {
				failed = true;
			}
		}
		return;
	}

	QJsonObject json = QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
	if (spectraRelease) {
		releaseTag = json["tag_name"].toString();
		releaseUrl = json["html_url"].toString();
		releaseNotes = json["body"].toString();
	} else {
		upstreamTag = json["tag_name"].toString();
	}
}

void SpectraUpdateCheck::Finished()
{
	config_t *config = App()->GetAppConfig();
	QString currentString = QString::fromStdString(App()->GetVersionString(false));
	Version current = Parse(currentString);
	Version release = Parse(releaseTag);
	Version upstream = Parse(upstreamTag);

	QString upstreamLine;
	if (upstream.valid() && Version{current.major, current.minor, current.patch, 0} <
					Version{upstream.major, upstream.minor, upstream.patch, 0}) {
		upstreamLine = QTStr("Spectra.Update.UpstreamNewer").arg(current.Base(), upstream.Base());
		blog(LOG_INFO, "[Spectra] Upstream OBS %s is newer than base %s", QT_TO_UTF8(upstream.Base()),
		     QT_TO_UTF8(current.Base()));
	}

	bool newer = release.valid() && current < release;
	QString skipped = QString::fromUtf8(config_get_string(config, "Spectra", "SkipVersion"));

	blog(LOG_INFO, "[Spectra] Update check: current %s, latest release %s, upstream OBS %s",
	     QT_TO_UTF8(currentString), releaseTag.isEmpty() ? "(none)" : QT_TO_UTF8(releaseTag),
	     upstreamTag.isEmpty() ? "(unknown)" : QT_TO_UTF8(upstreamTag));

	if (!parentWidget || (!manual && (!newer || skipped == releaseTag))) {
		deleteLater();
		return;
	}

	QMessageBox box(parentWidget);
	box.setWindowTitle(QTStr("Spectra.Update.Title"));

	if (newer) {
		QString notes = releaseNotes.trimmed();
		if (notes.size() > MAX_NOTES_CHARS) {
			notes = notes.left(MAX_NOTES_CHARS) + "\n...";
		}
		box.setIcon(QMessageBox::Information);
		box.setText(QTStr("Spectra.Update.Available").arg(releaseTag, currentString));
		box.setInformativeText(upstreamLine);
		box.setDetailedText(notes);

		QPushButton *download = box.addButton(QTStr("Spectra.Update.Download"), QMessageBox::AcceptRole);
		QPushButton *skip = box.addButton(QTStr("Spectra.Update.Skip"), QMessageBox::DestructiveRole);
		box.addButton(QTStr("Spectra.Update.Later"), QMessageBox::RejectRole);
		box.setDefaultButton(download);
		box.exec();

		if (box.clickedButton() == download) {
			QDesktopServices::openUrl(QUrl(releaseUrl));
		} else if (box.clickedButton() == skip) {
			config_set_string(config, "Spectra", "SkipVersion", QT_TO_UTF8(releaseTag));
			config_save_safe(config, "tmp", nullptr);
		}
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
