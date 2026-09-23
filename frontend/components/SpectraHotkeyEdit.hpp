#pragma once

#include <obs.h>
#include <util/config-file.h>

#include <QList>
#include <QWidget>

#include <string>

class OBSHotkeyEdit;
class QFormLayout;

/* Edits the main key of a frontend hotkey (e.g. "Spectra.ClipLast") outside
 * Settings > Hotkeys. Other bindings of the hotkey are kept. */
class SpectraHotkeyEdit : public QWidget {
	Q_OBJECT

public:
	explicit SpectraHotkeyEdit(const char *hotkeyName, QWidget *parent = nullptr);

	/* False if the hotkey isn't registered (e.g. its plugin isn't loaded) */
	bool Available() const { return available; }

	/* Applies and stores the key if it was changed */
	void Save(config_t *config);

	/* Adds a row per Spectra shortcut (clip, screenshot, Obscura capture and
	 * region) to `form`, skipping hotkeys that aren't registered */
	static QList<SpectraHotkeyEdit *> AddShortcutRows(QFormLayout *form);

private:
	std::string name;
	bool available = false;
	OBSHotkeyEdit *edit;
};
