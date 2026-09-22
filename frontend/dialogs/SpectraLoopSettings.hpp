#pragma once

#include <QDialog>

class LoopRecorder;
class OBSBasic;
class QCheckBox;
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
	QLineEdit *processes;
	QLabel *usage;

	QWidget *PathRow(QLineEdit *edit);
};
