#pragma once

#include <QWizardPage>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

/* Spectra setup: one base folder holding the loop recording, clips,
 * screenshots and Lucida/Obscura data, plus the game to watch for. */
class AutoConfigSpectraPage : public QWizardPage {
	Q_OBJECT

	QLineEdit *baseFolder;
	QLabel *folderPreview;
	QLineEdit *processes;
	QSpinBox *quotaGB;
	QCheckBox *autoStart;
	QCheckBox *autoCapture;
	QComboBox *resolution;
	QComboBox *quality;

	void UpdatePreview();

public:
	AutoConfigSpectraPage(QWidget *parent = nullptr);

	virtual int nextId() const override;
	virtual bool validatePage() override;

	/* Creates the folders and writes the settings to the profile */
	void Save();
};
