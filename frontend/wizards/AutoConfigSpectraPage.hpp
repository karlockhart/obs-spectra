#pragma once

#include <QList>
#include <QWizardPage>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class SpectraAppPicker;
class SpectraHotkeyEdit;

/* Spectra setup: one base folder holding the loop recording, clips,
 * screenshots and Lucida/Obscura data, plus the game to watch for. */
class AutoConfigSpectraPage : public QWizardPage {
	Q_OBJECT

	QLineEdit *baseFolder;
	QLabel *folderPreview;
	SpectraAppPicker *processes;
	QSpinBox *quotaGB;
	QSpinBox *clipSec;
	QList<SpectraHotkeyEdit *> shortcuts;
	QCheckBox *autoStart;
	QCheckBox *autoCapture;
	QComboBox *resolution;
	QComboBox *quality;

	void UpdatePreview();
	/* The folders can't change while the loop records into them */
	static bool FoldersLocked();

public:
	AutoConfigSpectraPage(QWidget *parent = nullptr);

	virtual int nextId() const override;
	virtual bool validatePage() override;

	/* Creates the folders and writes the settings to the profile */
	void Save();
};
