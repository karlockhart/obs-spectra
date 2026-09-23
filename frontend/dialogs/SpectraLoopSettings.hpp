#pragma once

#include <QDialog>
#include <QList>

class LoopRecorder;
class OBSBasic;
class SpectraAppPicker;
class SpectraHotkeyEdit;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class SpectraLoopSettings : public QDialog {
	Q_OBJECT

public:
	SpectraLoopSettings(OBSBasic *main, LoopRecorder *recorder);

	void accept() override;

private:
	OBSBasic *main;
	LoopRecorder *recorder;

	QLineEdit *loopPath;
	QLineEdit *clipsPath;
	QSpinBox *quotaGB;
	QSpinBox *segmentSec;
	QSpinBox *clipSec;
	QCheckBox *autoStart;
	QCheckBox *autoStop;
	QCheckBox *autoCapture;
	QCheckBox *fitToCanvas;
	QComboBox *resolution;
	QComboBox *quality;
	SpectraAppPicker *processes;
	QLabel *usage;
	QList<SpectraHotkeyEdit *> shortcuts;

	QWidget *PathRow(QLineEdit *edit);
};
