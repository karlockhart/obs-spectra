#pragma once

#include <utility/SpectraFeatures.hpp>

#include <QDialog>

#include <map>

class OBSBasic;
class QCheckBox;
class QComboBox;
class QLabel;

/* Spectra > Features: switches the Full edition's parts on and off, from
 * everything down to what Ultralight has. Changes apply after a restart,
 * which the dialog offers. */
class SpectraFeaturesDialog : public QDialog {
	Q_OBJECT

public:
	explicit SpectraFeaturesDialog(QWidget *parent);

	/* Shows the dialog and, when the user agrees, restarts OBS-Spectra */
	static void Open(OBSBasic *main);

private:
	QComboBox *preset = nullptr;
	QLabel *restartNote = nullptr;
	std::map<SpectraFeatures::Feature, QCheckBox *> boxes;
	bool applyingPreset = false;
	bool restartWanted = false;

	void ApplyPreset(SpectraFeatures::Preset preset);
	void UpdatePreset();
	void UpdateRestartNote();
	bool Changed() const;
	void Save();
};
