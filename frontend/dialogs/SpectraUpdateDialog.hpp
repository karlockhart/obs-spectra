#pragma once

#include <utility/SpectraReleases.hpp>
#include <utility/SpectraUpdateJob.hpp>

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QProgressBar;
class QPushButton;

/*
 * "Update Now" from the update notification: downloads and verifies the
 * release, then closes Spectra and lets spectra-updater.exe put it in place
 * and start Spectra again.
 */
class SpectraUpdateDialog : public QDialog {
	Q_OBJECT

public:
	SpectraUpdateDialog(QWidget *parent, const SpectraReleases::Release &release,
			    const SpectraReleases::Download &download, const QString &installDir,
			    const QString &helperExe);

	void reject() override;

	/* The update in progress, if any: only one runs at a time */
	static SpectraUpdateDialog *Current();

private:
	enum class State { Working, Ready, Failed, Launched };

	SpectraReleases::Release release;
	QString helperExe;
	SpectraUpdateJob *job;
	State state = State::Working;

	QLabel *status;
	QProgressBar *progress;
	QLabel *detail;
	QDialogButtonBox *buttons;

	void OnProgress(SpectraUpdateJob::Stage stage, qint64 done, qint64 total);
	void OnFinished(bool ok, const QString &error);
	void Install();
};
