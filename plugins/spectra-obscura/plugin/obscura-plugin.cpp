/*
 * spectra-obscura: Obscura inside Spectra. A hotkey grabs the game from
 * OBS's capture, chat lines are detected and the ones to censor are
 * suggested (learning from every review), and the censored screenshot is
 * saved and optionally uploaded to imgbb. A second hotkey snips a screen
 * region straight to imgbb, censoring any chat inside it first.
 */

#include "obscura-controller.hpp"
#include "obscura-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spectra-obscura", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Obscura chat censoring for Spectra";
}

static QPointer<obscura::Controller> controller;
static obs_hotkey_id captureHotkey = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id regionHotkey = OBS_INVALID_HOTKEY_ID;

/* --- hotkeys: saved in the module's config folder so they are the same for
 * every scene collection. Defaults: Ctrl+F12 capture, Ctrl+Shift+F12 region. */

static char *HotkeysPath()
{
	return obs_module_config_path("hotkeys.json");
}

static obs_data_array_t *DefaultBinding(bool shift)
{
	obs_data_array_t *array = obs_data_array_create();
	obs_data_t *key = obs_data_create();
	obs_data_set_string(key, "key", "OBS_KEY_F12");
	obs_data_set_bool(key, "control", true);
	obs_data_set_bool(key, "shift", shift);
	obs_data_array_push_back(array, key);
	obs_data_release(key);
	return array;
}

static void LoadHotkeys()
{
	char *path = HotkeysPath();
	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);
	auto load = [data](obs_hotkey_id id, const char *name, bool shift) {
		obs_data_array_t *array = data ? obs_data_get_array(data, name) : nullptr;
		if (!array) {
			array = DefaultBinding(shift);
		}
		obs_hotkey_load(id, array);
		obs_data_array_release(array);
	};
	load(captureHotkey, "capture", false);
	load(regionHotkey, "region", true);
	obs_data_release(data);
}

static void SaveHotkeys()
{
	if (captureHotkey == OBS_INVALID_HOTKEY_ID) {
		return;
	}
	obs_data_t *data = obs_data_create();
	obs_data_array_t *capture = obs_hotkey_save(captureHotkey);
	obs_data_array_t *region = obs_hotkey_save(regionHotkey);
	obs_data_set_array(data, "capture", capture);
	obs_data_set_array(data, "region", region);
	obs_data_array_release(capture);
	obs_data_array_release(region);
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	char *path = HotkeysPath();
	obs_data_save_json_safe(data, path, "tmp", "bak");
	bfree(path);
	obs_data_release(data);
}

static void OnHotkey(void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !controller) {
		return;
	}
	const bool region = param != nullptr;
	/* hotkeys fire on OBS's hotkey thread */
	QMetaObject::invokeMethod(
		controller,
		[region] {
			if (!controller) {
				return;
			}
			if (region) {
				controller->CaptureRegion();
			} else {
				controller->Capture();
			}
		},
		Qt::QueuedConnection);
}

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		controller = new obscura::Controller(main);
		captureHotkey = obs_hotkey_register_frontend(
			"SpectraObscura.Capture", obs_module_text("Obscura.Hotkey.Capture"), OnHotkey, nullptr);
		regionHotkey = obs_hotkey_register_frontend(
			"SpectraObscura.Region", obs_module_text("Obscura.Hotkey.Region"), OnHotkey, (void *)1);
		LoadHotkeys();

		auto *dock = new obscura::Dock(controller);
		obs_frontend_add_dock_by_id("spectra-obscura", obs_module_text("Obscura.Dock.Title"), dock);
		auto *action = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(obs_module_text("Obscura.Menu.Review")));
		QObject::connect(action, &QAction::triggered, [] {
			if (controller) {
				controller->ShowReview();
			}
		});
		controller->Start();
		blog(LOG_INFO, "[Obscura] Started (output: %s)", controller->Cfg().outputDir.toUtf8().constData());
		/* Developer aid: analyse a screenshot at startup */
		const QString open = qEnvironmentVariable("SPECTRA_OBSCURA_OPEN");
		if (!open.isEmpty()) {
			controller->SubmitPath(open, QStringLiteral("open"));
		}
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		SaveHotkeys();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		SaveHotkeys();
		if (controller) {
			controller->Stop();
		}
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	if (controller) {
		controller->Stop();
	}
	if (captureHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(captureHotkey);
		obs_hotkey_unregister(regionHotkey);
	}
}
