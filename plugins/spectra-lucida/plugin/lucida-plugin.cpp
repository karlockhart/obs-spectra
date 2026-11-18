/*
 * spectra-lucida: Lucida's background chat logger inside Spectra. Samples
 * the game capture every few seconds, OCRs the chat box and keeps one
 * time-ordered, deduplicated log of every line.
 */

#include "lucida-controller.hpp"
#include "lucida-dock.hpp"
#include "lucida-host.hpp"
#include "lucida-speech.hpp"
#include "lucida-prisma.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <spectra-grab/spectra-features.hpp>
#include <util/config-file.h>

#include <QAction>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spectra-lucida", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Lucida chat logger for Spectra";
}

QString lucida::Text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString lucida::ProfileString(const char *section, const char *name)
{
	config_t *c = obs_frontend_get_profile_config();
	const char *value = c ? config_get_string(c, section, name) : nullptr;
	return QString::fromUtf8(value ? value : "");
}

static QPointer<lucida::Controller> controller;
static QPointer<lucida::Dock> dock;

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		if (!spectra::FeatureEnabled("Lucida")) {
			/* Spectra > Features: no controller, dock, OCR or log this session */
			blog(LOG_INFO, "[Lucida] Switched off in Spectra > Features");
			break;
		}
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		controller = new lucida::Controller(main);
		dock = new lucida::Dock(controller);
		obs_frontend_add_dock_by_id("spectra-lucida-chat-log", obs_module_text("Lucida.Dock.Title"), dock);
		auto *viewer = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(obs_module_text("Lucida.Menu.Viewer")));
		QObject::connect(viewer, &QAction::triggered, [] {
			if (dock) {
				dock->OpenViewer();
			}
		});
		auto *settings = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(obs_module_text("Lucida.Menu.Settings")));
		QObject::connect(settings, &QAction::triggered, [] {
			if (dock) {
				dock->OpenSettings();
			}
		});
		auto *regions = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(obs_module_text("Lucida.Menu.Regions")));
		QObject::connect(regions, &QAction::triggered, [] {
			if (dock) {
				dock->OpenRegions();
			}
		});
		QObject::connect(controller, &lucida::Controller::failed, [](const QString &message) {
			blog(LOG_WARNING, "[Lucida] %s", message.toUtf8().constData());
		});
		controller->Start();
		/* clips and archived loops to Prisma, when Spectra is connected */
		lucida::PrismaExports::Get().Start(controller);
		/* Developer aid: open the viewer straight away */
		if (qEnvironmentVariableIsSet("SPECTRA_LUCIDA_VIEWER")) {
			dock->OpenViewer();
		}
		if (qEnvironmentVariableIsSet("SPECTRA_LUCIDA_REGIONS")) {
			QTimer::singleShot(0, dock, [] {
				if (dock) {
					dock->OpenRegions();
				}
			});
		}
		blog(LOG_INFO, "[Lucida] Started (log: %s)", controller->CurrentSettings().dbPath.toUtf8().constData());
		break;
	}
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		if (controller) {
			controller->ApplySettings(lucida::Settings::Load());
		}
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		lucida::PrismaExports::Get().Stop();
		if (controller) {
			controller->Stop();
			controller->StopLan();
			controller->Speech()->Stop();
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
		controller->Speech()->Stop();
	}
}
