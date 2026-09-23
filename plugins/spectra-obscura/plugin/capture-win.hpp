#pragma once

#include <spectra-vision/image.hpp>

#include <QString>

#include <optional>

namespace obscura {

/* Fallback for the capture hotkey when no OBS capture is hooked onto the
 * game: the target window's client area via PrintWindow
 * (PW_RENDERFULLCONTENT). Picks the foreground match, else the largest. */
std::optional<spectra::Image> CaptureTargetWindow(const QString &processRegex, const QString &titleContains,
						  QString *error);

/* The whole virtual desktop in physical pixels, for the region snip */
std::optional<spectra::Image> CaptureDesktop(QString *error);

} // namespace obscura
