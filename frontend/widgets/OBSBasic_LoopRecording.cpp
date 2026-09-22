#include "OBSBasic.hpp"

#include <dialogs/SpectraLoopSettings.hpp>
#include <utility/SpectraDefaults.hpp>

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

	captureStatusAction = spectraMenu->addAction(loopRecorder->Capture()->StatusText());
	captureStatusAction->setEnabled(false);
	spectraMenu->addSeparator();

	loopArmAction = spectraMenu->addAction(QTStr("Spectra.Loop.ArmedMenu"));
	loopArmAction->setCheckable(true);
	connect(loopArmAction, &QAction::triggered, this, [this](bool checked) { loopRecorder->SetArmed(checked); });

	loopToggleAction = spectraMenu->addAction(QTStr("Spectra.Loop.Start"));
	connect(loopToggleAction, &QAction::triggered, this, &OBSBasic::LoopRecordingActionTriggered);

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
	spectraMenu->addAction(QTStr("Spectra.ResetSources"), this, &OBSBasic::ResetSourcesToDefaults);

	connect(loopRecorder, &LoopRecorder::activeChanged, this, &OBSBasic::UpdateLoopRecordingUI);
	connect(loopRecorder, &LoopRecorder::armedChanged, this,
		[this]() { UpdateLoopRecordingUI(loopRecorder->Active()); });
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

	connect(loopRecorder->Capture(), &LoopCapture::stateChanged, this, [this](LoopCapture::State state) {
		QString status = loopRecorder->Capture()->StatusText();
		captureStatusAction->setText(status);
		if (state == LoopCapture::State::NotCapturing || state == LoopCapture::State::WindowCapture) {
			ShowStatusBarMessage(status);
			SysTrayNotify(status, state == LoopCapture::State::NotCapturing ? QSystemTrayIcon::Warning
											: QSystemTrayIcon::Information);
		}
	});

	UpdateLoopRecordingUI(false);
}

bool OBSBasic::ApplySpectraVideo()
{
	if (Active()) {
		return false;
	}

	config_t *config = Config();
	int cx = (int)config_get_int(config, "SpectraLoop", "CanvasCX");
	int cy = (int)config_get_int(config, "SpectraLoop", "CanvasCY");
	QString quality = QString::fromUtf8(config_get_string(config, "SpectraLoop", "Quality"));

	config_set_uint(config, "Video", "BaseCX", cx);
	config_set_uint(config, "Video", "BaseCY", cy);
	config_set_uint(config, "Video", "OutputCX", cx);
	config_set_uint(config, "Video", "OutputCY", cy);
	config_set_string(config, "SimpleOutput", "RecQuality", SpectraDefaults::QualityToRecQuality(quality));
	config_save_safe(config, "tmp", nullptr);

	blog(LOG_INFO, "[Spectra] Video set to %dx%d, %s quality", cx, cy, QT_TO_UTF8(quality));

	ResetVideo();
	ResetOutputs();
	return true;
}

OBSScene OBSBasic::GetProgramScene()
{
	OBSSource source = IsPreviewProgramMode() ? GetProgramSource() : GetCurrentSceneSource();
	return obs_scene_from_source(source);
}

void OBSBasic::UpdateLoopRecordingUI(bool active)
{
	bool available = outputHandler && outputHandler->LoopRecordingAvailable();
	if (loopToggleAction) {
		loopToggleAction->setText(QTStr(active ? "Spectra.Loop.Stop" : "Spectra.Loop.Start"));
		loopToggleAction->setEnabled(available);
	}
	bool armed = loopRecorder && loopRecorder->Armed();
	if (loopArmAction) {
		loopArmAction->setChecked(armed);
	}
	emit LoopRecordingStateChanged(active ? 2 : (armed ? 1 : 0));
	emit LoopRecordingEnabled(available);
}

void OBSBasic::LoopArmActionTriggered()
{
	if (!loopRecorder) {
		return;
	}
	/* The button disarms while armed or recording, and arms otherwise */
	loopRecorder->SetArmed(!(loopRecorder->Armed() || loopRecorder->Active()));
}

void OBSBasic::LoopRecordingActionTriggered()
{
	if (!loopRecorder) {
		return;
	}
	if (loopRecorder->Active()) {
		loopRecorder->Stop();
	} else {
		loopRecorder->Start();
	}
}

void OBSBasic::LoopClipActionTriggered()
{
	if (loopRecorder) {
		loopRecorder->ClipLast(loopRecorder->DefaultClipSeconds());
	}
}

void OBSBasic::OpenLoopSettings()
{
	SpectraLoopSettings dialog(this, loopRecorder);
	if (dialog.exec() == QDialog::Accepted) {
		loopRecorder->SettingsChanged();
	}
}

static bool HasInputDevices()
{
	OBSProperties props = obs_get_source_properties(App()->InputAudioSource());
	obs_property_t *devices = props ? obs_properties_get(props, "device_id") : nullptr;
	return devices && obs_property_list_item_count(devices) != 0;
}

void OBSBasic::ResetSourcesToDefaults()
{
	QMessageBox::StandardButton answer =
		OBSMessageBox::question(this, QTStr("Spectra.ResetSources.Title"), QTStr("Spectra.ResetSources.Text"));
	if (answer != QMessageBox::Yes) {
		return;
	}

	blog(LOG_INFO, "[Spectra] Resetting sources to defaults");

#ifdef _WIN32
	/* Game audio is captured per application, not from all desktop audio */
	ResetAudioDevice(App()->OutputAudioSource(), "disabled", nullptr, 1);
	ResetAudioDevice(App()->OutputAudioSource(), "disabled", nullptr, 2);
#endif

	if (HasInputDevices()) {
		ResetAudioDevice(App()->InputAudioSource(), "default", Str("Basic.AuxDevice1"), 3);
		OBSSourceAutoRelease mic = obs_get_output_source(3);
		if (mic) {
			SpectraDefaults::EnableDefaultPushToTalk(mic);
		}
	}

	SpectraDefaults::EnsureTeamSpeakAudio(GetProgramScene());

	if (loopRecorder) {
		loopRecorder->ResetCapture();
	}

	SaveProject();
	ShowStatusBarMessage(QTStr("Spectra.ResetSources.Done"));
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
