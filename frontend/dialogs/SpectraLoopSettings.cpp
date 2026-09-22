#include "SpectraLoopSettings.hpp"

#include <utility/LoopRecorder.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#define LOOP_SECTION "SpectraLoop"

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

	processes = new QLineEdit(recorder->ProcessPatterns().join(", "));
	processes->setToolTip(QTStr("Spectra.Loop.Settings.ProcessesTip"));

	double usedGB = (double)recorder->UsedBytes() / (1024.0 * 1024.0 * 1024.0);
	usage = new QLabel(QTStr("Spectra.Loop.Settings.Usage").arg(usedGB, 0, 'f', 1));

	auto *form = new QFormLayout();
	form->addRow(QTStr("Spectra.Loop.Settings.LoopPath"), PathRow(loopPath));
	form->addRow(QTStr("Spectra.Loop.Settings.ClipsPath"), PathRow(clipsPath));
	form->addRow(QTStr("Spectra.Loop.Settings.Quota"), quotaGB);
	form->addRow(QString(), usage);
	form->addRow(QTStr("Spectra.Loop.Settings.Segment"), segmentSec);
	form->addRow(QTStr("Spectra.Loop.Settings.ClipLength"), clipSec);
	form->addRow(QString(), autoStart);
	form->addRow(QTStr("Spectra.Loop.Settings.Processes"), processes);
	form->addRow(QString(), autoStop);
	form->addRow(QString(), autoCapture);
	form->addRow(QString(), fitToCanvas);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);

	if (recorder->Active()) {
		QLabel *note = new QLabel(QTStr("Spectra.Loop.Settings.ActiveNote"));
		note->setWordWrap(true);
		layout->insertWidget(1, note);
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

void SpectraLoopSettings::accept()
{
	config_t *config = main->Config();
	config_set_string(config, LOOP_SECTION, "Path", QT_TO_UTF8(loopPath->text().trimmed()));
	config_set_string(config, LOOP_SECTION, "ClipsPath", QT_TO_UTF8(clipsPath->text().trimmed()));
	config_set_uint(config, LOOP_SECTION, "QuotaGB", (uint64_t)quotaGB->value());
	config_set_int(config, LOOP_SECTION, "SegmentSec", segmentSec->value());
	config_set_int(config, LOOP_SECTION, "ClipSec", clipSec->value());
	config_set_bool(config, LOOP_SECTION, "AutoStart", autoStart->isChecked());
	config_set_bool(config, LOOP_SECTION, "AutoStop", autoStop->isChecked());
	config_set_bool(config, LOOP_SECTION, "AutoCapture", autoCapture->isChecked());
	config_set_bool(config, LOOP_SECTION, "FitToCanvas", fitToCanvas->isChecked());
	config_set_string(config, LOOP_SECTION, "Processes", QT_TO_UTF8(processes->text().trimmed()));
	config_save_safe(config, "tmp", nullptr);

	QDialog::accept();
}
