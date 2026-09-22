#pragma once

#include "chat.hpp"
#include "export.hpp"
#include "image.hpp"

#include <QString>

#include <optional>
#include <utility>
#include <vector>

namespace spectra {

/* --- Lucida's change gate (lucida/gate.py) ------------------------------- */

/* White top-hat (5x5 rect) of the grayscale image, thresholded at > 40:
 * thin bright strokes (glyphs) without large bright areas (sky) */
SPECTRA_VISION_API std::vector<uint8_t> TextMask(const Image &img);

/* Per-row count of set pixels */
SPECTRA_VISION_API std::vector<float> Signature(const std::vector<uint8_t> &mask, int width, int height);

/* sum|a-b| / max(sum a, sum b, 1); 1.0 if empty or of different length */
SPECTRA_VISION_API double SignatureDistance(const std::vector<float> &a, const std::vector<float> &b);

/* (changed, distance). No previous signature always counts as changed. */
SPECTRA_VISION_API std::pair<bool, double> SignatureChanged(const std::vector<float> *previous,
							    const std::vector<float> &current, double threshold);

/* --- Redaction (obscura/redact.py) --------------------------------------- */

/* Union of vertically touching rectangles, so stacked lines become one block */
SPECTRA_VISION_API std::vector<Rect> MergeRects(std::vector<Rect> rects);

/* Opaque fill of the (clamped, merged) rects. fill is "auto" (median colour of
 * each block's darkest 40% of pixels) or "#rrggbb". */
SPECTRA_VISION_API Image Redact(const Image &img, const std::vector<Rect> &rects, const QString &fill = "auto");

} // namespace spectra
