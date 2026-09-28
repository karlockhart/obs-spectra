#include "SpectraUpdateDialog.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCoreApplication>
#include <QDesktopServices>
#include <algorithm>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_SpectraUpdateDialog.cpp"

using namespace SpectraReleases;

static QPointer<SpectraUpdateDialog> current;

SpectraUpdateDialog *SpectraUpdateDialog::Current()
{
	return current;
}

static QString Megabytes(qint64 bytes)
{
	return QString::number(bytes / (1024.0 * 1024.0), 'f', 1);
}

SpectraUpdateDialog::SpectraUpdateDialog(QWidget *parent, const Release &release_, const Download &download,
					 const QString &installDir, const QString &helperExe_)
	: QDialog(parent),
	  release(release_),
	  helperExe(helperExe_)
{
	setWindowTitle(QTStr("Spectra.Update.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(460);
	current = this;

	QVBoxLayout *layout = new QVBoxLayout(this);
	QLabel *heading = new QLabel(QTStr("Spectra.Update.Installing").arg(release.tag), this);
	QFont bold = heading->font();
	bold.setBold(true);
	heading->setFont(bold);
	layout->addWidget(heading);

	status = new QLabel(this);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(status);

	progress = new QProgressBar(this);
	progress->setTextVisible(false);
	layout->addWidget(progress);

	detail = new QLabel(this);
	detail->setWordWrap(true);
	layout->addWidget(detail);

	buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &SpectraUpdateDialog::reject);
	layout->addWidget(buttons);

	job = new SpectraUpdateJob(installDir, download, this);
	connect(job, &SpectraUpdateJob::Progress, this, &SpectraUpdateDialog::OnProgress);
	connect(job, &SpectraUpdateJob::Finished, this, &SpectraUpdateDialog::OnFinished);

	blog(LOG_INFO, "[Spectra] Updating %s to %s from %s", QT_TO_UTF8(QDir::toNativeSeparators(installDir)),
	     QT_TO_UTF8(release.tag), QT_TO_UTF8(download.zip.url));
	QMetaObject::invokeMethod(job, &SpectraUpdateJob::Start, Qt::QueuedConnection);
}

void SpectraUpdateDialog::OnProgress(SpectraUpdateJob::Stage stage, qint64 done, qint64 total)
{
	switch (stage) {
	case SpectraUpdateJob::Stage::Preparing:
		status->setText(QTStr("Spectra.Update.Preparing"));
		progress->setRange(0, 0);
		detail->clear();
		break;
	case SpectraUpdateJob::Stage::Downloading:
		status->setText(QTStr("Spectra.Update.Downloading"));
		if (total > 0) {
			progress->setRange(0, 1000);
			progress->setValue((int)(done * 1000 / total));
			detail->setText(QTStr("Spectra.Update.Megabytes").arg(Megabytes(done), Megabytes(total)));
		} else {
			progress->setRange(0, 0);
			detail->setText(QTStr("Spectra.Update.MegabytesSoFar").arg(Megabytes(done)));
		}
		break;
	case SpectraUpdateJob::Stage::Verifying:
		status->setText(QTStr("Spectra.Update.Verifying"));
		progress->setRange(0, 0);
		detail->clear();
		break;
	case SpectraUpdateJob::Stage::Extracting:
		status->setText(QTStr("Spectra.Update.Extracting"));
		if (total > 0) {
			progress->setRange(0, 1000);
			progress->setValue((int)(done * 1000 / total));
		} else {
			progress->setRange(0, 0);
		}
		detail->clear();
		break;
	case SpectraUpdateJob::Stage::Ready:
		progress->setRange(0, 1);
		progress->setValue(1);
		break;
	}
}

void SpectraUpdateDialog::OnFinished(bool ok, const QString &error)
{
	if (!ok && error == "cancelled") {
		blog(LOG_INFO, "[Spectra] Update to %s cancelled", QT_TO_UTF8(release.tag));
		state = State::Failed;
		QDialog::reject();
		return;
	}

	buttons->clear();
	if (!ok) {
		blog(LOG_WARNING, "[Spectra] Update to %s failed: %s", QT_TO_UTF8(release.tag), QT_TO_UTF8(error));
		state = State::Failed;
		status->setText(QTStr("Spectra.Update.Error").arg(error) + "\n\n" + QTStr("Spectra.Update.NotChanged"));
		progress->hide();
		detail->clear();
		QPushButton *page = buttons->addButton(QTStr("Spectra.Update.Download"), QDialogButtonBox::ActionRole);
		connect(page, &QPushButton::clicked, this,
			[this]() { QDesktopServices::openUrl(QUrl(release.htmlUrl)); });
		buttons->addButton(QDialogButtonBox::Close);
		return;
	}

	blog(LOG_INFO, "[Spectra] Update to %s downloaded, verified and extracted to %s", QT_TO_UTF8(release.tag),
	     QT_TO_UTF8(QDir::toNativeSeparators(job->StagingDir())));
	state = State::Ready;
	QString text = QTStr("Spectra.Update.Ready");
	OBSBasic *main = OBSBasic::Get();
	if (main && main->ActiveExceptLoop()) {
		text += "\n\n" + QTStr("Spectra.Update.OutputsWillStop");
	}
	status->setText(text);
	detail->clear();

	QPushButton *install =
		buttons->addButton(QTStr("Spectra.Update.RestartAndInstall"), QDialogButtonBox::AcceptRole);
	install->setDefault(true);
	connect(install, &QPushButton::clicked, this, &SpectraUpdateDialog::Install);
	buttons->addButton(QDialogButtonBox::Cancel);
}

void SpectraUpdateDialog::Install()
{
	/* start the new version the way this one was started, but don't repeat
	 * one-off actions like --startrecording */
	QStringList args = QCoreApplication::arguments().mid(1);
	args.erase(std::remove_if(args.begin(), args.end(),
				  [](const QString &arg) { return arg.startsWith("--start"); }),
		   args.end());

	QString error;
	if (!job->LaunchHelper(helperExe, release.tag, SpectraUpdateJob::QuoteArguments(args), &error)) {
		blog(LOG_WARNING, "[Spectra] Update to %s: %s", QT_TO_UTF8(release.tag), QT_TO_UTF8(error));
		OnFinished(false, error);
		job->Discard();
		return;
	}

	blog(LOG_INFO, "[Spectra] Closing for the update to %s; spectra-updater.exe logs to %s",
	     QT_TO_UTF8(release.tag), QT_TO_UTF8(QDir::toNativeSeparators(job->LogPath())));
	state = State::Launched;
	QDialog::accept();
	if (OBSBasic *main = OBSBasic::Get()) {
		main->CloseForUpdate();
	}
}

void SpectraUpdateDialog::reject()
{
	switch (state) {
	case State::Working:
		/* the job reports "cancelled", which closes the dialog */
		status->setText(QTStr("Spectra.Update.Cancelling"));
		job->Cancel();
		return;
	case State::Ready:
		job->Discard();
		break;
	case State::Failed:
	case State::Launched:
		break;
	}
	QDialog::reject();
}
