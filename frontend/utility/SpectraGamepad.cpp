#include "SpectraGamepad.hpp"
#include "SpectraDefaults.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#endif

#include "moc_SpectraGamepad.cpp"

#define AUDIO_SECTION "SpectraAudio"

static constexpr int POLL_MS = 16;
/* Reading a disconnected controller is slow, so empty slots are only
 * checked every couple of seconds. */
static constexpr int RECONNECT_POLLS = 2000 / POLL_MS;
static constexpr int TRIGGER_THRESHOLD = 64;

/* Bits above the XInput button bits stand for the analog triggers */
static constexpr unsigned int LEFT_TRIGGER_BIT = 1u << 16;
static constexpr unsigned int RIGHT_TRIGGER_BIT = 1u << 17;

struct GamepadButton {
	const char *id;
	const char *label;
	unsigned int mask;
};

static const GamepadButton gamepadButtons[] = {
	{"A", "A", 0x1000},
	{"B", "B", 0x2000},
	{"X", "X", 0x4000},
	{"Y", "Y", 0x8000},
	{"LB", "Left Bumper (LB)", 0x0100},
	{"RB", "Right Bumper (RB)", 0x0200},
	{"LT", "Left Trigger (LT)", LEFT_TRIGGER_BIT},
	{"RT", "Right Trigger (RT)", RIGHT_TRIGGER_BIT},
	{"LS", "Left Stick Click (LS)", 0x0040},
	{"RS", "Right Stick Click (RS)", 0x0080},
	{"Back", "Back / View", 0x0020},
	{"Start", "Start / Menu", 0x0010},
	{"DPadUp", "D-Pad Up", 0x0001},
	{"DPadDown", "D-Pad Down", 0x0002},
	{"DPadLeft", "D-Pad Left", 0x0004},
	{"DPadRight", "D-Pad Right", 0x0008},
};

#ifdef _WIN32
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);

static unsigned int HeldMask(const XINPUT_STATE &state)
{
	unsigned int held = state.Gamepad.wButtons;
	if (state.Gamepad.bLeftTrigger > TRIGGER_THRESHOLD) {
		held |= LEFT_TRIGGER_BIT;
	}
	if (state.Gamepad.bRightTrigger > TRIGGER_THRESHOLD) {
		held |= RIGHT_TRIGGER_BIT;
	}
	return held;
}

static XInputGetStateFn XInputGetStateFunc()
{
	static XInputGetStateFn func = []() -> XInputGetStateFn {
		for (const wchar_t *dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
			if (HMODULE module = LoadLibraryW(dll)) {
				if (auto fn = reinterpret_cast<XInputGetStateFn>(
					    GetProcAddress(module, "XInputGetState"))) {
					return fn;
				}
			}
		}
		return nullptr;
	}();
	return func;
}
#endif

SpectraGamepadPTT::SpectraGamepadPTT(OBSBasic *main_) : QObject(main_), main(main_)
{
	timer.setInterval(POLL_MS);
	connect(&timer, &QTimer::timeout, this, &SpectraGamepadPTT::Poll);
	SettingsChanged();
}

QStringList SpectraGamepadPTT::ButtonIds()
{
	QStringList ids;
	for (const GamepadButton &button : gamepadButtons) {
		ids << button.id;
	}
	return ids;
}

QString SpectraGamepadPTT::ButtonLabel(const QString &id)
{
	for (const GamepadButton &button : gamepadButtons) {
		if (id == button.id) {
			return QTStr("Spectra.Audio.Gamepad").arg(button.label);
		}
	}
	return id;
}

bool SpectraGamepadPTT::Supported()
{
#ifdef _WIN32
	return XInputGetStateFunc() != nullptr;
#else
	return false;
#endif
}

QString SpectraGamepadPTT::HeldButton()
{
#ifdef _WIN32
	XInputGetStateFn getState = XInputGetStateFunc();
	if (!getState) {
		return QString();
	}
	for (DWORD i = 0; i < 4; i++) {
		XINPUT_STATE state = {};
		if (getState(i, &state) != ERROR_SUCCESS) {
			continue;
		}
		unsigned int held = HeldMask(state);
		for (const GamepadButton &button : gamepadButtons) {
			if (held & button.mask) {
				return button.id;
			}
		}
	}
#endif
	return QString();
}

QStringList SpectraGamepadPTT::Buttons() const
{
	return buttons;
}

void SpectraGamepadPTT::SetButtons(const QStringList &newButtons)
{
	config_set_string(main->Config(), AUDIO_SECTION, "PTTGamepad", QT_TO_UTF8(newButtons.join(",")));
	SettingsChanged();
}

void SpectraGamepadPTT::SettingsChanged()
{
	const char *value = config_get_string(main->Config(), AUDIO_SECTION, "PTTGamepad");
	buttons.clear();
	buttonMask = 0;
	for (const QString &id : QString::fromUtf8(value ? value : "").split(',', Qt::SkipEmptyParts)) {
		for (const GamepadButton &button : gamepadButtons) {
			if (id.trimmed() == button.id) {
				buttons << button.id;
				buttonMask |= button.mask;
			}
		}
	}

	if (buttonMask && Supported()) {
		connected = 0xF;
		reconnectPolls = 0;
		timer.start();
	} else {
		timer.stop();
		SetPressed(false);
	}
}

void SpectraGamepadPTT::Poll()
{
#ifdef _WIN32
	XInputGetStateFn getState = XInputGetStateFunc();
	if (!getState) {
		return;
	}

	bool recheck = ++reconnectPolls >= RECONNECT_POLLS;
	if (recheck) {
		reconnectPolls = 0;
	}

	unsigned int held = 0;
	for (DWORD i = 0; i < 4; i++) {
		if (!(connected & (1u << i)) && !recheck) {
			continue;
		}

		XINPUT_STATE state = {};
		if (getState(i, &state) != ERROR_SUCCESS) {
			connected &= ~(1u << i);
			continue;
		}
		connected |= 1u << i;

		held |= HeldMask(state);
	}

	SetPressed((held & buttonMask) != 0);
#endif
}

void SpectraGamepadPTT::SetPressed(bool nowPressed)
{
	if (pressed == nowPressed) {
		return;
	}
	pressed = nowPressed;

	OBSSourceAutoRelease mic = obs_get_output_source(3);
	if (!mic) {
		return;
	}
	obs_hotkey_id id = SpectraDefaults::PushToTalkHotkey(mic);
	if (id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_trigger_routed_callback(id, pressed);
	}
}
