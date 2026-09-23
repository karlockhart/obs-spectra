#include "SpectraHotkeyEdit.hpp"

#include <settings/OBSHotkeyEdit.hpp>
#include <utility/SpectraDefaults.hpp>

#include <qt-wrappers.hpp>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>

#include "moc_SpectraHotkeyEdit.cpp"

SpectraHotkeyEdit::SpectraHotkeyEdit(const char *hotkeyName, QWidget *parent) : QWidget(parent), name(hotkeyName)
{
	obs_hotkey_id id = SpectraDefaults::FrontendHotkey(hotkeyName);
	available = id != OBS_INVALID_HOTKEY_ID;

	std::vector<obs_key_combination_t> keys = SpectraDefaults::GetHotkeyKeys(id);
	obs_key_combination_t current = keys.empty() ? obs_key_combination_t{0, OBS_KEY_NONE} : keys.front();

	edit = new OBSHotkeyEdit(this, current, nullptr);
	edit->setPlaceholderText(QTStr("Spectra.Hotkey.PressKey"));
	edit->setEnabled(available);

	QPushButton *clear = new QPushButton(QTStr("Clear"));
	clear->setAutoDefault(false);
	clear->setEnabled(available);
	connect(clear, &QPushButton::clicked, edit, &OBSHotkeyEdit::ClearKey);

	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(edit, 1);
	layout->addWidget(clear);
}

void SpectraHotkeyEdit::Save(config_t *config)
{
	if (!available || !edit->changed || edit->key == edit->original) {
		return;
	}

	std::vector<obs_key_combination_t> keys =
		SpectraDefaults::GetHotkeyKeys(SpectraDefaults::FrontendHotkey(name.c_str()));
	if (!keys.empty()) {
		keys.erase(keys.begin());
	}
	if (!obs_key_combination_is_empty(edit->key)) {
		keys.insert(keys.begin(), edit->key);
	}
	SpectraDefaults::SetFrontendHotkeyKeys(config, name.c_str(), keys);

	blog(LOG_INFO, "[Spectra] Hotkey '%s' changed", name.c_str());
}

QList<SpectraHotkeyEdit *> SpectraHotkeyEdit::AddShortcutRows(QFormLayout *form)
{
	static const struct {
		const char *hotkey;
		const char *label;
	} shortcuts[] = {
		{"Spectra.ClipLast", "Spectra.Hotkey.Clip"},
		{"OBSBasic.Screenshot", "Spectra.Hotkey.Screenshot"},
		{"SpectraObscura.Capture", "Spectra.Hotkey.ObscuraCapture"},
		{"SpectraObscura.Region", "Spectra.Hotkey.ObscuraRegion"},
	};

	QList<SpectraHotkeyEdit *> edits;
	for (const auto &shortcut : shortcuts) {
		auto *edit = new SpectraHotkeyEdit(shortcut.hotkey);
		if (!edit->Available()) {
			delete edit;
			continue;
		}
		form->addRow(QTStr(shortcut.label), edit);
		edits << edit;
	}
	return edits;
}
