#include "SpectraLoopSettings.hpp"

#include <components/SpectraAppPicker.hpp>
#include <components/SpectraHotkeyEdit.hpp>
#include <utility/LoopRecorder.hpp>
#include <utility/SpectraDefaults.hpp>
#include <utility/SpectraOverlay.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#define LOOP_SECTION "SpectraLoop"
#define OVERLAY_SECTION "SpectraOverlay"

SpectraLoopSettings::SpectraLoopSettings(OBSBasic *main_, LoopRecorder *recorder_)
	: QDialog(main_),
	  main(main_),
	  recorder(recorder_)
{
	setWindowTitle(QTStr("Spectra.Loop.Settings.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(520);

	config_t *config = main->Config();

	loopPath = new QLineEdit(QString::fromUtf8(config_get_string(config, LOOP_SECTION, "Path")));
	loopPath->setPlaceholderText(recorder->LoopDirectory());
	clipsPath = new QLineEdit(QString::fromUtf8(config_get_string(config, LOOP_SECTION, "ClipsPath")));
	clipsPath->setPlaceholderText(recorder->ClipsDirectory());

	quotaGB = new QSpinBox();
	quotaGB->setRange(1, 100000);
	quotaGB->setSuffix(" GB");
	quotaGB->setValue((int)config_get_uint(config, LOOP_SECTION, "QuotaGB"));

	segmentSec = new QSpinBox();
	segmentSec->setRange(10, 3600);
	segmentSec->setSuffix(" s");
	segmentSec->setValue(recorder->SegmentSeconds());
	segmentSec->setToolTip(QTStr("Spectra.Loop.Settings.SegmentTip"));

	clipSec = new QSpinBox();
	clipSec->setRange(5, 24 * 3600);
	clipSec->setSuffix(" s");
	clipSec->setValue(recorder->DefaultClipSeconds());
	clipSec->setToolTip(QTStr("Spectra.Loop.Settings.ClipLengthTip"));

	autoStart = new QCheckBox(QTStr("Spectra.Loop.Settings.AutoStart"));
	autoStart->setChecked(recorder->AutoStartEnabled());
	autoStop = new QCheckBox(QTStr("Spectra.Loop.Settings.AutoStop"));
	autoStop->setChecked(recorder->AutoStopEnabled());
	autoCapture = new QCheckBox(QTStr("Spectra.Loop.Settings.AutoCapture"));
	autoCapture->setChecked(recorder->AutoCaptureEnabled());
	autoCapture->setToolTip(QTStr("Spectra.Loop.Settings.AutoCaptureTip"));
	fitToCanvas = new QCheckBox(QTStr("Spectra.Loop.Settings.FitToCanvas"));
	fitToCanvas->setChecked(recorder->FitToCanvasEnabled());
	fitToCanvas->setToolTip(QTStr("Spectra.Loop.Settings.FitToCanvasTip"));
	starlingVoice = new QCheckBox(QTStr("Spectra.Loop.Settings.StarlingVoice"));
	starlingVoice->setChecked(recorder->StarlingVoiceEnabled());
	starlingVoice->setToolTip(QTStr("Spectra.Loop.Settings.StarlingVoiceTip"));
	speakerTracks = new QCheckBox(QTStr("Spectra.Loop.Settings.SpeakerTracks"));
	speakerTracks->setChecked(config_get_bool(config, LOOP_SECTION, "SpeakerTracks"));
	speakerTracks->setToolTip(QTStr("Spectra.Loop.Settings.SpeakerTracksTip"));

	resolution = new QComboBox();
	SpectraDefaults::FillResolutionCombo(resolution, (int)config_get_int(config, LOOP_SECTION, "CanvasCX"),
					     (int)config_get_int(config, LOOP_SECTION, "CanvasCY"));
	resolution->setToolTip(QTStr("Spectra.Video.ResolutionTip"));
	quality = new QComboBox();
	SpectraDefaults::FillQualityCombo(quality,
					  QString::fromUtf8(config_get_string(config, LOOP_SECTION, "Quality")));
	quality->setToolTip(QTStr("Spectra.Video.QualityTip"));

	processes = new SpectraAppPicker();
	processes->SetPatterns(recorder->ProcessPatterns().join(", "));
	processes->SetAnyFullscreen(recorder->AnyFullscreenEnabled());

	double usedGB = (double)recorder->UsedBytes() / (1024.0 * 1024.0 * 1024.0);
	usage = new QLabel(QTStr("Spectra.Loop.Settings.Usage").arg(usedGB, 0, 'f', 1));

	loopPathRow = PathRow(loopPath);
	clipsPathRow = PathRow(clipsPath);

	auto *form = new QFormLayout();
	form->addRow(QTStr("Spectra.Loop.Settings.LoopPath"), loopPathRow);
	form->addRow(QTStr("Spectra.Loop.Settings.ClipsPath"), clipsPathRow);
	form->addRow(QTStr("Spectra.Loop.Settings.Quota"), quotaGB);
	form->addRow(QString(), usage);
	form->addRow(QTStr("Spectra.Loop.Settings.Segment"), segmentSec);
	form->addRow(QTStr("Spectra.Loop.Settings.ClipLength"), clipSec);
	form->addRow(QTStr("Spectra.Video.Resolution"), resolution);
	form->addRow(QTStr("Spectra.Video.Quality"), quality);
	form->addRow(QString(), autoStart);
	form->addRow(QTStr("Spectra.Loop.Settings.Processes"), processes);
	form->addRow(QString(), autoStop);
	form->addRow(QString(), autoCapture);
	form->addRow(QString(), fitToCanvas);
	form->addRow(QString(), starlingVoice);
	form->addRow(QString(), speakerTracks);

	QGroupBox *overlayGroup = new QGroupBox(QTStr("Spectra.Overlay.Settings"));
	auto *overlayForm = new QFormLayout(overlayGroup);
	overlayEnabled = new QCheckBox(QTStr("Spectra.Overlay.Settings.Enabled"));
	overlayEnabled->setToolTip(QTStr("Spectra.Overlay.Settings.EnabledTip"));
	overlayEnabled->setChecked(config_get_bool(config, OVERLAY_SECTION, "Enabled"));
	overlayCorner = new QComboBox();
	for (const char *key :
	     {"Spectra.Overlay.Corner.TopRight", "Spectra.Overlay.Corner.TopLeft", "Spectra.Overlay.Corner.BottomRight",
	      "Spectra.Overlay.Corner.BottomLeft", "Spectra.Overlay.Corner.TopCenter"}) {
		overlayCorner->addItem(QTStr(key));
	}
	overlayCorner->setCurrentIndex((int)config_get_int(config, OVERLAY_SECTION, "Corner"));
	overlayDuration = new QSpinBox();
	overlayDuration->setRange(1, 30);
	overlayDuration->setSuffix(QStringLiteral(" s"));
	overlayDuration->setValue((int)config_get_int(config, OVERLAY_SECTION, "DurationSec"));
	QPushButton *overlayTest = new QPushButton(QTStr("Spectra.Overlay.Settings.Test"));
	connect(overlayTest, &QPushButton::clicked, this, [this]() {
		SpectraOverlay *overlay = main->GetSpectraOverlay();
		if (!overlay) {
			return;
		}
		/* preview the choices; Cancel/OK reloads the saved ones */
		overlay->SetStyle(true, (SpectraOverlay::Corner)overlayCorner->currentIndex(),
				  overlayDuration->value());
		overlay->Notify(SpectraOverlay::Kind::Loop, QTStr("Spectra.Overlay.LoopStarted"),
				QTStr("Spectra.Overlay.LoopStartedText").arg(QTStr("Spectra.Loop.Minutes").arg(2)));
		overlay->Notify(SpectraOverlay::Kind::Clip, QTStr("Spectra.Overlay.ClipSaved"),
				QStringLiteral("FiveM 2026-09-23 21-04-12.mp4"));
		overlay->Notify(SpectraOverlay::Kind::Obscura, QTStr("Spectra.Overlay.Test.Obscura"),
				QTStr("Spectra.Overlay.Test.ObscuraText"));
	});
	overlayForm->addRow(overlayEnabled);
	overlayForm->addRow(QTStr("Spectra.Overlay.Settings.Corner"), overlayCorner);
	overlayForm->addRow(QTStr("Spectra.Overlay.Settings.Duration"), overlayDuration);
	auto *eventGrid = new QGridLayout();
	eventGrid->setContentsMargins(0, 0, 0, 0);
	for (const SpectraOverlay::Category &category : SpectraOverlay::Categories()) {
		auto *check = new QCheckBox(QTStr(category.labelKey));
		check->setChecked(config_get_bool(config, OVERLAY_SECTION, category.configKey));
		check->setProperty("configKey", QString::fromLatin1(category.configKey));
		const int i = (int)overlayEvents.size();
		eventGrid->addWidget(check, i / 2, i % 2);
		overlayEvents << check;
	}
	overlayForm->addRow(QTStr("Spectra.Overlay.Settings.Events"), eventGrid);
	overlayForm->addRow(QString(), overlayTest);

	QGroupBox *shortcutGroup = new QGroupBox(QTStr("Spectra.Hotkey.Shortcuts"));
	auto *shortcutForm = new QFormLayout(shortcutGroup);
	QLabel *shortcutNote = new QLabel(QTStr("Spectra.Hotkey.ShortcutsNote"));
	shortcutNote->setWordWrap(true);
	shortcutForm->addRow(shortcutNote);
	shortcuts = SpectraHotkeyEdit::AddShortcutRows(shortcutForm);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(overlayGroup);
	layout->addWidget(shortcutGroup);
	layout->addWidget(buttons);

	if (main->Active()) {
		resolution->setEnabled(false);
		quality->setEnabled(false);
	}

	if (recorder->Active()) {
		QLabel *note = new QLabel(QTStr("Spectra.Loop.Settings.ActiveNote"));
		note->setWordWrap(true);
		layout->insertWidget(1, note);
		LockFolders();
	}
}

void SpectraLoopSettings::LockFolders()
{
	config_t *config = main->Config();
	loopPath->setText(QString::fromUtf8(config_get_string(config, LOOP_SECTION, "Path")));
	clipsPath->setText(QString::fromUtf8(config_get_string(config, LOOP_SECTION, "ClipsPath")));
	for (QWidget *row : {loopPathRow, clipsPathRow}) {
		row->setEnabled(false);
		row->setToolTip(QTStr("Spectra.Loop.FoldersLocked"));
	}
}

QWidget *SpectraLoopSettings::PathRow(QLineEdit *edit)
{
	QWidget *row = new QWidget();
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(edit);

	QPushButton *browse = new QPushButton(QTStr("Browse"));
	connect(browse, &QPushButton::clicked, this, [this, edit]() {
		QString start = edit->text().isEmpty() ? edit->placeholderText() : edit->text();
		QString dir = QFileDialog::getExistingDirectory(this, QTStr("Browse"), start);
		if (!dir.isEmpty()) {
			edit->setText(dir);
		}
	});
	layout->addWidget(browse);
	return row;
}

void SpectraLoopSettings::reject()
{
	if (SpectraOverlay *overlay = main->GetSpectraOverlay()) {
		overlay->LoadSettings();
	}
	QDialog::reject();
}

void SpectraLoopSettings::accept()
{
	config_t *config = main->Config();

	/* The loop can start by itself while the dialog is open */
	const bool foldersLocked = recorder->Active();
	if (foldersLocked && loopPathRow->isEnabled()) {
		const bool changed =
			loopPath->text().trimmed() !=
				QString::fromUtf8(config_get_string(config, LOOP_SECTION, "Path")).trimmed() ||
			clipsPath->text().trimmed() !=
				QString::fromUtf8(config_get_string(config, LOOP_SECTION, "ClipsPath")).trimmed();
		LockFolders();
		if (changed) {
			OBSMessageBox::warning(this, QTStr("Spectra.Loop.Settings.Title"),
					       QTStr("Spectra.Loop.FoldersLockedNotSaved"));
			return;
		}
	}

	int cx = 0, cy = 0;
	if (!SpectraDefaults::ParseResolution(resolution->currentText(), cx, cy)) {
		OBSMessageBox::warning(this, QTStr("Spectra.Loop.Settings.Title"),
				       QTStr("Spectra.Video.InvalidResolution"));
		return;
	}
	bool videoChanged = cx != (int)config_get_int(config, LOOP_SECTION, "CanvasCX") ||
			    cy != (int)config_get_int(config, LOOP_SECTION, "CanvasCY") ||
			    quality->currentData().toString() !=
				    QString::fromUtf8(config_get_string(config, LOOP_SECTION, "Quality"));
	config_set_int(config, LOOP_SECTION, "CanvasCX", cx);
	config_set_int(config, LOOP_SECTION, "CanvasCY", cy);
	config_set_string(config, LOOP_SECTION, "Quality", QT_TO_UTF8(quality->currentData().toString()));
	if (!foldersLocked) {
		config_set_string(config, LOOP_SECTION, "Path", QT_TO_UTF8(loopPath->text().trimmed()));
		config_set_string(config, LOOP_SECTION, "ClipsPath", QT_TO_UTF8(clipsPath->text().trimmed()));
	}
	config_set_uint(config, LOOP_SECTION, "QuotaGB", (uint64_t)quotaGB->value());
	config_set_int(config, LOOP_SECTION, "SegmentSec", segmentSec->value());
	config_set_int(config, LOOP_SECTION, "ClipSec", clipSec->value());
	config_set_bool(config, LOOP_SECTION, "AutoStart", autoStart->isChecked());
	config_set_bool(config, LOOP_SECTION, "AutoStop", autoStop->isChecked());
	config_set_bool(config, LOOP_SECTION, "AutoCapture", autoCapture->isChecked());
	config_set_bool(config, LOOP_SECTION, "FitToCanvas", fitToCanvas->isChecked());
	config_set_bool(config, LOOP_SECTION, "StarlingVoice", starlingVoice->isChecked());
	config_set_bool(config, LOOP_SECTION, "SpeakerTracks", speakerTracks->isChecked());
	config_set_string(config, LOOP_SECTION, "Processes", QT_TO_UTF8(processes->Patterns()));
	config_set_bool(config, LOOP_SECTION, "AnyFullscreen", processes->AnyFullscreen());
	config_set_bool(config, OVERLAY_SECTION, "Enabled", overlayEnabled->isChecked());
	config_set_int(config, OVERLAY_SECTION, "Corner", overlayCorner->currentIndex());
	config_set_int(config, OVERLAY_SECTION, "DurationSec", overlayDuration->value());
	for (QCheckBox *check : overlayEvents) {
		config_set_bool(config, OVERLAY_SECTION, QT_TO_UTF8(check->property("configKey").toString()),
				check->isChecked());
	}
	for (SpectraHotkeyEdit *shortcut : shortcuts) {
		shortcut->Save(config);
	}
	config_save_safe(config, "tmp", nullptr);

	if (videoChanged) {
		main->ApplySpectraVideo();
	}
	if (SpectraOverlay *overlay = main->GetSpectraOverlay()) {
		overlay->LoadSettings();
	}

	QDialog::accept();
}
