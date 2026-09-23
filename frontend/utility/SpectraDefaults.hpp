#pragma once

#include <obs.h>
#include <util/config-file.h>

#include <QString>

#include <vector>

class QComboBox;

/* Spectra's default audio setup, applied on first run and by
 * "Reset Sources to Defaults". */
namespace SpectraDefaults {

/* Enables push-to-talk on `source`, bound to the default keys. */
void EnableDefaultPushToTalk(obs_source_t *source);

/* N, M, O and P */
std::vector<obs_key_combination_t> DefaultPushToTalkKeys();

/* The push-to-talk hotkey of an audio source */
obs_hotkey_id PushToTalkHotkey(obs_source_t *source);
std::vector<obs_key_combination_t> GetPushToTalkKeys(obs_source_t *source);
void SetPushToTalkKeys(obs_source_t *source, const std::vector<obs_key_combination_t> &keys);

/* Key combinations bound to a hotkey */
std::vector<obs_key_combination_t> GetHotkeyKeys(obs_hotkey_id id);
/* A hotkey registered with obs_hotkey_register_frontend (Spectra, OBS or a
 * plugin such as Obscura), or OBS_INVALID_HOTKEY_ID if it is not registered */
obs_hotkey_id FrontendHotkey(const char *name);
/* Rebinds a frontend hotkey and stores it in the profile like Settings > Hotkeys */
void SetFrontendHotkeyKeys(config_t *config, const char *name, const std::vector<obs_key_combination_t> &keys);

/* Executable of an installed TeamSpeak client, or empty if none is found. */
QString FindTeamSpeakExecutable();

/* Adds an audio-only TeamSpeak application audio capture to `scene` if
 * TeamSpeak is installed and the scene doesn't capture it already. */
void EnsureTeamSpeakAudio(obs_scene_t *scene);

/* Spectra's recording video target (canvas and output resolution) */
constexpr int DefaultCanvasCX = 1920;
constexpr int DefaultCanvasCY = 1080;

/* Quality presets map to Simple output mode recording quality */
extern const char *const DefaultQuality;
const char *QualityToRecQuality(const QString &quality);

/* Fills a resolution combo box (editable, "WIDTHxHEIGHT") and selects cx/cy */
void FillResolutionCombo(QComboBox *combo, int cx, int cy);
/* Parses "WIDTHxHEIGHT"; returns false if invalid */
bool ParseResolution(const QString &text, int &cx, int &cy);

void FillQualityCombo(QComboBox *combo, const QString &quality);

/* Best available hardware encoder as a Simple output mode encoder name
 * (NVENC, then AMD, then Quick Sync, then Apple), or x264 if none. */
const char *PreferredSimpleEncoder();

/* Whether a Simple output mode encoder name is a software (x264) encoder */
bool IsSoftwareSimpleEncoder(const char *encoder);

} // namespace SpectraDefaults
