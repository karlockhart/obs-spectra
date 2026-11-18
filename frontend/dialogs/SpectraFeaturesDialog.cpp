#include "SpectraFeaturesDialog.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

#include "moc_SpectraFeaturesDialog.cpp"

extern bool restart;

using namespace SpectraFeatures;

SpectraFeaturesDialog::SpectraFeaturesDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QTStr("Spectra.Features.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(520);

	auto *layout = new QVBoxLayout(this);

	auto *intro = new QLabel(QTStr("Spectra.Features.Intro"), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	auto *form = new QFormLayout();
	preset = new QComboBox(this);
	for (Preset p : {Preset::Full, Preset::Lite, Preset::Ultralight, Preset::Custom}) {
		if (PresetAvailable(p)) {
			preset->addItem(PresetName(p), (int)p);
		}
	}
	form->addRow(QTStr("Spectra.Features.Preset"), preset);
	layout->addLayout(form);

	for (Feature feature : All) {
		if (!Available(feature)) {
			continue;
		}
		auto *box = new QCheckBox(Name(feature), this);
		box->setChecked(Configured(feature));
		auto *tip = new QLabel(Description(feature), this);
		tip->setWordWrap(true);
		tip->setIndent(24);
		tip->setProperty("class", "text-muted");
		layout->addWidget(box);
		layout->addWidget(tip);
		boxes[feature] = box;
		connect(box, &QCheckBox::toggled, this, [this]() {
			if (!applyingPreset) {
				UpdatePreset();
			}
			UpdateRestartNote();
		});
	}

	restartNote = new QLabel(QTStr("Spectra.Features.RestartNote"), this);
	restartNote->setWordWrap(true);
	restartNote->setProperty("class", "text-warning");
	layout->addWidget(restartNote);
	layout->addStretch();

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &SpectraFeaturesDialog::Save);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	connect(preset, &QComboBox::activated, this, [this](int index) {
		auto chosen = (Preset)preset->itemData(index).toInt();
		if (chosen != Preset::Custom) {
			ApplyPreset(chosen);
		}
	});

	UpdatePreset();
	UpdateRestartNote();
}

void SpectraFeaturesDialog::ApplyPreset(Preset p)
{
	applyingPreset = true;
	for (auto &[feature, box] : boxes) {
		box->setChecked(PresetIncludes(p, feature));
	}
	applyingPreset = false;
	UpdatePreset();
	UpdateRestartNote();
}

void SpectraFeaturesDialog::UpdatePreset()
{
	/* PresetFor takes a plain function, so the boxes are read through a static */
	static const std::map<Feature, QCheckBox *> *current = nullptr;
	current = &boxes;
	Preset p = PresetFor([](Feature feature) {
		auto it = current->find(feature);
		return it == current->end() || it->second->isChecked();
	});
	int index = preset->findData((int)p);
	if (index >= 0) {
		preset->setCurrentIndex(index);
	}
}

bool SpectraFeaturesDialog::Changed() const
{
	for (auto &[feature, box] : boxes) {
		if (box->isChecked() != Enabled(feature)) {
			return true;
		}
	}
	return false;
}

void SpectraFeaturesDialog::UpdateRestartNote()
{
	restartNote->setVisible(Changed());
}

void SpectraFeaturesDialog::Save()
{
	for (auto &[feature, box] : boxes) {
		if (box->isChecked() != Configured(feature)) {
			SetConfigured(feature, box->isChecked());
			blog(LOG_INFO, "[Spectra] Feature %s turned %s (applies on restart)", Key(feature),
			     box->isChecked() ? "on" : "off");
		}
	}
	restartWanted = false;
	if (Changed()) {
		restartWanted = OBSMessageBox::question(this, QTStr("Restart"), QTStr("Spectra.Features.RestartNow")) ==
				QMessageBox::Yes;
	}
	accept();
}

void SpectraFeaturesDialog::Open(OBSBasic *main)
{
	bool restartNow = false;
	{
		SpectraFeaturesDialog dialog(main);
		restartNow = dialog.exec() == QDialog::Accepted && dialog.restartWanted;
	}
	/* only once the dialog is gone: closing the main window deletes its children */
	if (restartNow) {
		restart = true;
		main->close();
	}
}
