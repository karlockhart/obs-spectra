#pragma once

#include <obs.h>

#include <QString>

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

} // namespace SpectraDefaults
