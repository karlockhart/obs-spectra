#pragma once

#include <util/config-file.h>

#include <cstdint>

/*
 * Loop recordings can carry one audio track per speaker next to the full
 * mix, so speech can be transcribed and labelled per speaker:
 *
 *   track 1  the full mix (what clips keep)
 *   track 2  me: microphones and the Starling voice
 *   track 3  TeamSpeak
 *   track 4  the game, and every other sound
 *
 * Every audio source stays in track 1 and is also in exactly one of tracks
 * 2-4, so transcribing those three misses nothing. They are only recorded
 * by the loop, never by normal recordings unless they were set up to.
 */
namespace SpectraSpeakerTracks {

enum class Speaker { Mix = 0, Me = 1, TeamSpeak = 2, Game = 3 };
constexpr int kCount = 4;
/* Mixer bits the loop records with speaker tracks on */
constexpr uint32_t kLoopMixers = (1u << kCount) - 1;

bool Enabled(config_t *profile);

/* Routes every audio source to track 1 plus its speaker's track, if any.
 * Cheap; called whenever sources may have appeared. */
void Apply();

/* Label for transcripts ("me", "teamspeak", "game"), empty for the mix */
const char *Label(Speaker speaker);

} // namespace SpectraSpeakerTracks
