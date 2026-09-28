#pragma once

#include "export.hpp"

#include <QString>

#include <vector>

namespace spectra::speech {

/* Number of audio tracks in a media file, or -1 if it can't be opened */
SPECTRA_SPEECH_API int AudioTrackCount(const QString &path, QString *error = nullptr);

/* Decodes audio track `track` (0 = the first audio stream) of `path` from
 * `start` seconds for `duration` seconds (< 0: to the end) into 16 kHz mono
 * float samples. Sample 0 is at `start`; gaps in the track are filled with
 * silence so sample times stay true. */
SPECTRA_SPEECH_API bool ReadAudio(const QString &path, int track, double start, double duration,
				  std::vector<float> &samples, QString *error = nullptr);

} // namespace spectra::speech
