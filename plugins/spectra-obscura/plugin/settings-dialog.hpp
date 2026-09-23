#pragma once

#include <spectra-vision/chat.hpp>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

namespace obscura {

class Controller;

/* Obscura's settings (port of obscura/gui/settings.py) */
class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	SettingsDialog(Controller *controller, QWidget *parent = nullptr);

	void accept() override;

private:
	Controller *c;
	QLineEdit *process, *title, *outputDir, *originalsDir, *prefix, *fill, *seeds, *imgbbKey, *distRepo;
	QComboBox *cropReview, *tz, *imgbbExpiry;
	QCheckBox *watchEnabled, *moveWatched, *openedSrc, *keepOriginals, *autoApply, *useInstalled, *imgbbCopy,
		*imgbbOpen, *autoDefs, *prereleaseDefs;
	QListWidget *watchList;
	QDoubleSpinBox *region[2][4];
	QSpinBox *minScreens;
	QDoubleSpinBox *confidence;
	QLabel *stats;
	QString imgbbKeyInitial;

	QWidget *CaptureTab();
	QWidget *FilesTab();
	QWidget *DetectionTab();
	QWidget *LearningTab();
	QWidget *UploadTab();
	QWidget *UpdatesTab();
	QWidget *PathEdit(QLineEdit *&edit, const QString &value);
	QWidget *RegionEdit(int which, const spectra::Region &r);
	spectra::Region RegionValue(int which) const;
	void UpdateStats();
};

} // namespace obscura
