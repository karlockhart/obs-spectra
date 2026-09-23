#pragma once

#include <obs.hpp>

#include <QDialog>
#include <QTimer>

class OBSBasic;
class OBSHotkeyEdit;
class SpectraAppPicker;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QListWidget;
class QPushButton;
class QSpinBox;

/*
 * Spectra's audio setup in one place: the microphone with push-to-talk on
 * keys, mouse buttons and gamepad buttons, the applications whose audio is
 * recorded (TeamSpeak by default), and whether all desktop audio is
 * recorded.
 */
class SpectraAudioSetup : public QDialog {
	Q_OBJECT

public:
	explicit SpectraAudioSetup(OBSBasic *main);

	void accept() override;

private:
	OBSBasic *main;

	QComboBox *micDevice;
	QGroupBox *pttGroup;
	QListWidget *pttKeys;
	OBSHotkeyEdit *keyEdit;
	QPushButton *gamepadButton;
	QPushButton *removeKey;
	QSpinBox *pttDelay;
	QTimer gamepadListen;
	int gamepadListenPolls = 0;

	SpectraAppPicker *apps;
	QCheckBox *desktopAudio;

	void AddKey(obs_key_combination_t key);
	void AddGamepadButton(const QString &id);
	void SetDefaultKeys();
	void StartGamepadListen();
	void StopGamepadListen();
	void UpdateMicState();

	void ApplyApplicationAudio(const QStringList &exes);
};
