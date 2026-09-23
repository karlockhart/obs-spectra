#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

class OBSBasic;

/*
 * Push-to-talk on gamepad buttons. libobs hotkeys only know keyboard and
 * mouse, so XInput controllers are polled here and the microphone's
 * push-to-talk hotkey is triggered directly while a bound button is held.
 * Buttons are stored in the profile config (SpectraAudio/PTTGamepad).
 */
class SpectraGamepadPTT : public QObject {
	Q_OBJECT

public:
	explicit SpectraGamepadPTT(OBSBasic *main);

	/* Button ids as stored in the config, e.g. "LB" */
	static QStringList ButtonIds();
	static QString ButtonLabel(const QString &id);
	/* Whether gamepads can be read on this platform */
	static bool Supported();
	/* A button currently held on any controller, or empty */
	static QString HeldButton();

	QStringList Buttons() const;
	void SetButtons(const QStringList &buttons);

	/* Re-reads the bound buttons from the config */
	void SettingsChanged();

private:
	OBSBasic *main;
	QTimer timer;
	QStringList buttons;
	unsigned int buttonMask = 0;
	bool pressed = false;
	unsigned int connected = 0;
	int reconnectPolls = 0;

	void Poll();
	void SetPressed(bool pressed);
};
