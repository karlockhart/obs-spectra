#include "SpectraDefaults.hpp"

#include <OBSApp.hpp>
#include <obs.hpp>
#include <qt-wrappers.hpp>

#include <QComboBox>
#include <QFileInfo>
#include <QRegularExpression>

extern bool EncoderAvailable(const char *encoder);

namespace SpectraDefaults {

const char *PreferredSimpleEncoder()
{
	if (EncoderAvailable("obs_nvenc_h264_tex") || EncoderAvailable("ffmpeg_nvenc")) {
		return "nvenc";
	}
	if (EncoderAvailable("h264_texture_amf")) {
		return "amd";
	}
	if (EncoderAvailable("obs_qsv11")) {
		return "qsv";
	}
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.avc")) {
		return "apple_h264";
	}
	return "x264";
}

bool IsSoftwareSimpleEncoder(const char *encoder)
{
	return !encoder || !*encoder || strcmp(encoder, "x264") == 0 || strcmp(encoder, "x264_lowcpu") == 0;
}

const char *const DefaultQuality = "Good";

const char *QualityToRecQuality(const QString &quality)
{
	if (quality == "High") {
		return "HQ"; /* Indistinguishable quality, large file size */
	}
	if (quality == "Stream") {
		return "Stream"; /* Same as stream */
	}
	return "Small"; /* Good: high quality, medium file size */
}

void FillResolutionCombo(QComboBox *combo, int cx, int cy)
{
	static const char *resolutions[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};

	combo->setEditable(true);
	for (const char *res : resolutions) {
		combo->addItem(res);
	}
	combo->setCurrentText(QStringLiteral("%1x%2").arg(cx).arg(cy));
}

bool ParseResolution(const QString &text, int &cx, int &cy)
{
	QRegularExpressionMatch match = QRegularExpression("^\\s*(\\d{3,5})\\s*[xX]\\s*(\\d{3,5})\\s*$").match(text);
	if (!match.hasMatch()) {
		return false;
	}
	cx = match.captured(1).toInt();
	cy = match.captured(2).toInt();
	return cx >= 320 && cy >= 240 && cx <= 7680 && cy <= 4320;
}

void FillQualityCombo(QComboBox *combo, const QString &quality)
{
	combo->addItem(QTStr("Spectra.Video.Quality.Good"), "Good");
	combo->addItem(QTStr("Spectra.Video.Quality.High"), "High");
	combo->addItem(QTStr("Spectra.Video.Quality.Stream"), "Stream");
	int index = combo->findData(quality);
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

std::vector<obs_key_combination_t> DefaultPushToTalkKeys()
{
	return {{0, OBS_KEY_N}, {0, OBS_KEY_M}, {0, OBS_KEY_O}, {0, OBS_KEY_P}};
}

obs_hotkey_id PushToTalkHotkey(obs_source_t *source)
{
	struct Context {
		obs_source_t *source;
		obs_hotkey_id id;
	} ctx = {source, OBS_INVALID_HOTKEY_ID};

	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
			Context *c = static_cast<Context *>(data);
			if (obs_hotkey_get_registerer_type(key) != OBS_HOTKEY_REGISTERER_SOURCE ||
			    strcmp(obs_hotkey_get_name(key), "libobs.push-to-talk") != 0) {
				return true;
			}
			obs_weak_source_t *weak = static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key));
			if (obs_weak_source_references_source(weak, c->source)) {
				c->id = id;
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.id;
}

std::vector<obs_key_combination_t> GetHotkeyKeys(obs_hotkey_id id)
{
	struct Context {
		obs_hotkey_id id;
		std::vector<obs_key_combination_t> keys;
	} ctx = {id, {}};

	if (ctx.id == OBS_INVALID_HOTKEY_ID) {
		return {};
	}

	obs_enum_hotkey_bindings(
		[](void *data, size_t, obs_hotkey_binding_t *binding) {
			Context *c = static_cast<Context *>(data);
			if (obs_hotkey_binding_get_hotkey_id(binding) == c->id) {
				c->keys.push_back(obs_hotkey_binding_get_key_combination(binding));
			}
			return true;
		},
		&ctx);
	return ctx.keys;
}

std::vector<obs_key_combination_t> GetPushToTalkKeys(obs_source_t *source)
{
	return GetHotkeyKeys(PushToTalkHotkey(source));
}

obs_hotkey_id FrontendHotkey(const char *name)
{
	struct Context {
		const char *name;
		obs_hotkey_id id;
	} ctx = {name, OBS_INVALID_HOTKEY_ID};

	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
			Context *c = static_cast<Context *>(data);
			if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_FRONTEND &&
			    strcmp(obs_hotkey_get_name(key), c->name) == 0) {
				c->id = id;
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.id;
}

void SetFrontendHotkeyKeys(config_t *config, const char *name, const std::vector<obs_key_combination_t> &keys)
{
	obs_hotkey_id id = FrontendHotkey(name);
	if (id == OBS_INVALID_HOTKEY_ID) {
		blog(LOG_WARNING, "[Spectra] Could not find the '%s' hotkey", name);
		return;
	}
	obs_hotkey_load_bindings(id, const_cast<obs_key_combination_t *>(keys.data()), keys.size());

	/* Stored the same way as Settings > Hotkeys stores it */
	OBSDataArrayAutoRelease array = obs_hotkey_save(id);
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_array(data, "bindings", array);
	config_set_string(config, "Hotkeys", name, obs_data_get_json(data));
}

void SetPushToTalkKeys(obs_source_t *source, const std::vector<obs_key_combination_t> &keys)
{
	obs_hotkey_id id = PushToTalkHotkey(source);
	if (id == OBS_INVALID_HOTKEY_ID) {
		blog(LOG_WARNING, "[Spectra] Could not find the push-to-talk hotkey of '%s'",
		     obs_source_get_name(source));
		return;
	}
	obs_hotkey_load_bindings(id, const_cast<obs_key_combination_t *>(keys.data()), keys.size());
}

void EnableDefaultPushToTalk(obs_source_t *source)
{
	obs_source_enable_push_to_talk(source, true);
	SetPushToTalkKeys(source, DefaultPushToTalkKeys());
	blog(LOG_INFO, "[Spectra] Push-to-talk enabled for '%s' (N, M, O, P)", obs_source_get_name(source));
}

QString FindTeamSpeakExecutable()
{
#ifdef _WIN32
	struct Candidate {
		QString path;
		const char *exe;
	};

	const QString programFiles = qEnvironmentVariable("ProgramFiles");
	const QString programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)");
	const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");

	const Candidate candidates[] = {
		{localAppData + "/Programs/TeamSpeak/TeamSpeak.exe", "TeamSpeak.exe"},
		{programFiles + "/TeamSpeak/TeamSpeak.exe", "TeamSpeak.exe"},
		{programFiles + "/TeamSpeak 3 Client/ts3client_win64.exe", "ts3client_win64.exe"},
		{programFilesX86 + "/TeamSpeak 3 Client/ts3client_win32.exe", "ts3client_win32.exe"},
		{localAppData + "/TeamSpeak 3 Client/ts3client_win64.exe", "ts3client_win64.exe"},
	};

	for (const Candidate &candidate : candidates) {
		if (QFileInfo::exists(candidate.path)) {
			return candidate.exe;
		}
	}
#endif
	return QString();
}

void EnsureTeamSpeakAudio(obs_scene_t *scene)
{
	QString exe = FindTeamSpeakExecutable();
	if (exe.isEmpty() || !scene) {
		return;
	}

	/* "title:class:exe", matched by executable */
	QString window = "::" + exe;

	struct Context {
		QString window;
		bool found;
	} ctx = {window, false};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			Context *c = static_cast<Context *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (strcmp(obs_source_get_unversioned_id(source), "wasapi_process_output_capture") != 0) {
				return true;
			}
			OBSDataAutoRelease settings = obs_source_get_settings(source);
			QString target = QString::fromUtf8(obs_data_get_string(settings, "window"));
			if (target.section(':', 2).compare(c->window.section(':', 2), Qt::CaseInsensitive) == 0) {
				c->found = true;
				return false;
			}
			return true;
		},
		&ctx);

	if (ctx.found) {
		return;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "window", QT_TO_UTF8(window));
	obs_data_set_int(settings, "priority", 2 /* WINDOW_PRIORITY_EXE */);

	OBSSourceAutoRelease existing = obs_get_source_by_name("TeamSpeak");
	OBSSourceAutoRelease source = existing
					      ? OBSSourceAutoRelease(obs_source_get_ref(existing))
					      : OBSSourceAutoRelease(obs_source_create("wasapi_process_output_capture",
										       "TeamSpeak", settings, nullptr));
	if (!source || strcmp(obs_source_get_unversioned_id(source), "wasapi_process_output_capture") != 0) {
		return;
	}

	obs_scene_add(scene, source);
	blog(LOG_INFO, "[Spectra] Added TeamSpeak audio capture (%s)", QT_TO_UTF8(exe));
}

} // namespace SpectraDefaults
