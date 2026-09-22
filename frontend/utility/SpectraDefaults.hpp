#pragma once

#include <obs.h>

#include <QString>

class QComboBox;

/* Spectra's default audio setup, applied on first run and by
 * "Reset Sources to Defaults". */
namespace SpectraDefaults {

/* Enables push-to-talk on `source`, bound to N, M and P. */
void EnableDefaultPushToTalk(obs_source_t *source);

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

} // namespace SpectraDefaults
