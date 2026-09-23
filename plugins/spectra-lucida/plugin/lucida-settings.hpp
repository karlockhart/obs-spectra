#pragma once

#include "lucida-controller.hpp"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

namespace lucida {

/* Lucida's settings: when it runs, how it samples, what it keeps, and the
 * tag rules. Applies through the controller, which restarts sampling. */
class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	SettingsDialog(Controller *controller, QWidget *parent = nullptr);

	void accept() override;

private:
	Controller *controller;

	/* General */
	QCheckBox *enabled, *followLoop;
	QLineEdit *targetProcess, *dbPath;
	QSpinBox *retentionDays;

	/* Sampling */
	QDoubleSpinBox *interval, *minInterval, *maxInterval, *idleInterval, *gateThreshold;
	QCheckBox *adaptive, *readHud;
	QSpinBox *ocrThreads;
	QDoubleSpinBox *chat[4], *hud[4];

	/* Screenshots */
	QCheckBox *keepFrames, *keepCrops;
	QLineEdit *framesDir, *cropsDir;
	QSpinBox *frameQuality, *frameRetention, *cropQuality, *cropRetention;

	/* Tags */
	QTableWidget *rules;
	QCheckBox *tolerateTypos;
	QLineEdit *tryLine;
	QLabel *tryResult;

	QWidget *GeneralPage(const Settings &s);
	QWidget *SamplingPage(const Settings &s);
	QWidget *ScreenshotsPage(const Settings &s);
	QWidget *TagsPage(const Settings &s);
	QWidget *PathRow(QLineEdit *edit, bool file);
	void AddRule(const TagRule &rule);
	QList<TagRule> Rules() const;
	void UpdateTry();
	Settings Collect() const;
};

} // namespace lucida
