#pragma once

#include <QString>

#include <optional>

namespace lucida {

/* Where a moment is in Spectra's loop recording */
struct VideoSpot {
	QString path;  /* the segment file */
	double offset; /* seconds into it */
};

/* Loop segments are named after their local start time
 * ("2026-09-23 14-05-00.mkv"); invalid for other names */
double SegmentStart(const QString &fileName);

/* The loop segment that was recording at wall-clock time `wallTs` (seconds
 * since the epoch). A segment runs until the next one starts; the newest one
 * until its file was last written, or until now while `recording`. */
std::optional<VideoSpot> LocateVideo(const QString &loopDir, double wallTs, bool recording);

/* "1:02:03" / "2:03" */
QString FormatOffset(double seconds);

} // namespace lucida
