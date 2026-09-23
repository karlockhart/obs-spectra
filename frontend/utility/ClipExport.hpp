#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ClipExport {

/* Duration of a media file in seconds, or a negative value on failure. */
double ProbeDuration(const std::string &path);

/* Progress callback: fraction in [0, 1]. Return false to cancel. */
using ProgressCallback = std::function<bool(float)>;

/*
 * Losslessly joins `inputs` (in playback order, all with the same stream
 * layout and codecs) and writes the span [startSec, endSec) of the joined
 * timeline to `output`. The container is chosen from the output extension.
 *
 * No re-encoding happens, so the start snaps back to the nearest keyframe
 * at or before `startSec`. A negative `endSec` means "to the end".
 */
bool Export(const std::vector<std::string> &inputs, double startSec, double endSec, const std::string &output,
	    std::string &error, const ProgressCallback &progress = nullptr);

} // namespace ClipExport
