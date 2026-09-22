/*
 * spectra-lucida: Lucida's background chat logger inside Spectra. Samples
 * the game capture every few seconds, OCRs the chat box and keeps one
 * time-ordered, deduplicated log of every line.
 */

#include "lucida-controller.hpp"
#include "lucida-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spectra-lucida", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Lucida chat logger for Spectra";
}

static QPointer<lucida::Controller> controller;

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		controller = new lucida::Controller(main);
		auto *dock = new lucida::Dock(controller);
		obs_frontend_add_dock_by_id("spectra-lucida-chat-log", obs_module_text("Lucida.Dock.Title"), dock);
		QObject::connect(controller, &lucida::Controller::failed, [](const QString &message) {
			blog(LOG_WARNING, "[Lucida] %s", message.toUtf8().constData());
		});
		controller->Start();
		blog(LOG_INFO, "[Lucida] Started (log: %s)", controller->CurrentSettings().dbPath.toUtf8().constData());
		break;
	}
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		if (controller) {
			controller->ApplySettings(lucida::Settings::Load());
		}
		break;
	case OBS_FRONTEND_EVENT_EXIT:
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
}
