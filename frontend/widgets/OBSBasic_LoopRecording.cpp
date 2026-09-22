#include "OBSBasic.hpp"

#include <dialogs/SpectraLoopSettings.hpp>

#include <qt-wrappers.hpp>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QMenuBar>
#include <QUrl>

static const int clipPresets[] = {30, 60, 120, 300, 600, 1800};

static QString FormatClipLength(int seconds)
{
	if (seconds < 60) {
		return QTStr("Spectra.Loop.Seconds").arg(seconds);
	}
	return QTStr("Spectra.Loop.Minutes").arg(seconds / 60);
}

void OBSBasic::InitSpectra()
{
	loopRecorder = new LoopRecorder(this);

	spectraMenu = new QMenu(QTStr("Spectra.Menu"), this);
	menuBar()->insertMenu(ui->menuTools->menuAction(), spectraMenu);

	loopToggleAction = spectraMenu->addAction(QTStr("Spectra.Loop.Start"));
	connect(loopToggleAction, &QAction::triggered, this, [this]() {
		if (loopRecorder->Active()) {
			loopRecorder->Stop();
		} else {
			loopRecorder->Start();
		}
	});

	QMenu *clipMenu = spectraMenu->addMenu(QTStr("Spectra.Loop.ClipLast"));
	for (int seconds : clipPresets) {
		clipMenu->addAction(FormatClipLength(seconds), this,
				    [this, seconds]() { loopRecorder->ClipLast(seconds); });
	}

	spectraMenu->addSeparator();
	spectraMenu->addAction(QTStr("Spectra.Loop.OpenClips"), this, [this]() {
		QString dir = loopRecorder->ClipsDirectory();
		QDir().mkpath(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	spectraMenu->addAction(QTStr("Spectra.Loop.OpenLoopFolder"), this, [this]() {
		QString dir = loopRecorder->LoopDirectory();
		QDir().mkpath(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	spectraMenu->addSeparator();
	spectraMenu->addAction(QTStr("Spectra.Loop.Settings"), this, &OBSBasic::OpenLoopSettings);

	connect(loopRecorder, &LoopRecorder::activeChanged, this, &OBSBasic::UpdateLoopRecordingUI);
	/* The output handler is recreated when output settings change */
	connect(spectraMenu, &QMenu::aboutToShow, this, [this]() { UpdateLoopRecordingUI(loopRecorder->Active()); });
	connect(loopRecorder, &LoopRecorder::clipStarted, this, [this](int seconds) {
		ShowStatusBarMessage(QTStr("Spectra.Loop.Clipping").arg(FormatClipLength(seconds)));
	});
	connect(loopRecorder, &LoopRecorder::clipSaved, this, [this](const QString &path) {
		QString msg = QTStr("Spectra.Loop.ClipSaved").arg(QDir::toNativeSeparators(path));
		ShowStatusBarMessage(msg);
		if (!isActiveWindow()) {
			SysTrayNotify(msg, QSystemTrayIcon::Information);
		}
		QApplication::beep();
	});
	connect(loopRecorder, &LoopRecorder::clipFailed, this, [this](const QString &error) {
		QString msg = QTStr("Spectra.Loop.ClipFailed").arg(error);
		ShowStatusBarMessage(msg);
		SysTrayNotify(msg, QSystemTrayIcon::Warning);
	});

	UpdateLoopRecordingUI(false);
}

void OBSBasic::UpdateLoopRecordingUI(bool active)
{
	if (loopToggleAction) {
		loopToggleAction->setText(QTStr(active ? "Spectra.Loop.Stop" : "Spectra.Loop.Start"));
		loopToggleAction->setEnabled(outputHandler && outputHandler->LoopRecordingAvailable());
	}
}

void OBSBasic::OpenLoopSettings()
{
	SpectraLoopSettings dialog(this, loopRecorder);
	if (dialog.exec() == QDialog::Accepted) {
		loopRecorder->SettingsChanged();
	}
}

bool OBSBasic::StartLoopRecording(const QString &directory, int segmentSeconds)
{
	if (!outputHandler || !outputHandler->LoopRecordingAvailable()) {
		ShowStatusBarMessage(QTStr("Spectra.Loop.Error.Unavailable"));
		return false;
	}
	if (outputHandler->LoopRecordingActive()) {
		return true;
	}
	if (disableOutputsRef) {
		return false;
	}
	if (LowDiskSpace()) {
		DiskSpaceMessage();
		return false;
	}

	SaveProject();

	if (!outputHandler->StartLoopRecording(QT_TO_UTF8(directory), segmentSeconds)) {
		QString error = QString::fromStdString(outputHandler->lastError);
		QString msg = QTStr("Spectra.Loop.Error.Start").arg(error);
		ShowStatusBarMessage(msg);
		SysTrayNotify(msg, QSystemTrayIcon::Warning);
		return false;
	}
	return true;
}

void OBSBasic::StopLoopRecording()
{
	if (outputHandler && outputHandler->LoopRecordingActive()) {
		outputHandler->StopLoopRecording();
	}
}

bool OBSBasic::LoopRecordingActive() const
{
	return outputHandler && outputHandler->LoopRecordingActive();
}

bool OBSBasic::SplitLoopRecording()
{
	if (!LoopRecordingActive()) {
		return false;
	}

	calldata_t cd = {0};
	proc_handler_t *ph = obs_output_get_proc_handler(outputHandler->loopOutput);
	bool called = proc_handler_call(ph, "split_file", &cd);
	bool enabled = called && calldata_bool(&cd, "split_file_enabled");
	calldata_free(&cd);
	return enabled;
}

void OBSBasic::LoopRecordingStart()
{
	OnActivate();
	if (loopRecorder) {
		loopRecorder->OnStarted();
	}
}

void OBSBasic::LoopRecordingStop(int code, QString lastError)
{
	if (loopRecorder) {
		loopRecorder->OnStopped(code, lastError);
	}

	if (code == OBS_OUTPUT_NO_SPACE) {
		SysTrayNotify(QTStr("Output.RecordNoSpace.Msg"), QSystemTrayIcon::Warning);
	} else if (code != OBS_OUTPUT_SUCCESS) {
		SysTrayNotify(QTStr("Spectra.Loop.Error.Start").arg(lastError), QSystemTrayIcon::Warning);
	}

	OnDeactivate();
}

void OBSBasic::LoopRecordingFileChanged(QString nextFile)
{
	if (loopRecorder) {
		loopRecorder->OnFileChanged(nextFile);
	}
}
