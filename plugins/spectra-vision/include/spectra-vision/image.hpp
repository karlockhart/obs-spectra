#pragma once

#include "export.hpp"

#include <cstdint>
#include <vector>

namespace spectra {

/* 8-bit interleaved image. Colour images are BGR (OpenCV order), which is
 * also what the OCR models expect. */
struct Image {
	int width = 0;
	int height = 0;
	int channels = 3;
	std::vector<uint8_t> data;

	Image() = default;
	Image(int w, int h, int c = 3, uint8_t fill = 0)
		: width(w),
		  height(h),
		  channels(c),
		  data((size_t)(w > 0 ? w : 0) * (h > 0 ? h : 0) * c, fill)
	{
	}

	bool empty() const { return width <= 0 || height <= 0; }
	uint8_t *row(int y) { return data.data() + (size_t)y * width * channels; }
	const uint8_t *row(int y) const { return data.data() + (size_t)y * width * channels; }
	uint8_t *at(int x, int y) { return row(y) + (size_t)x * channels; }
	const uint8_t *at(int x, int y) const { return row(y) + (size_t)x * channels; }
};

/* Copy of the rectangle [x0, x1) x [y0, y1), clamped to the image */
SPECTRA_VISION_API Image Crop(const Image &img, int x0, int y0, int x1, int y1);

/* Bilinear resize with OpenCV INTER_LINEAR sampling and 8-bit rounding */
SPECTRA_VISION_API Image ResizeLinear(const Image &img, int width, int height);

/* Adds constant-colour borders (cv2.copyMakeBorder BORDER_CONSTANT, black) */
SPECTRA_VISION_API Image PadConstant(const Image &img, int top, int bottom, int left, int right);

/* BGRA/BGRX (e.g. from an OBS frame) to BGR */
SPECTRA_VISION_API Image FromBGRA(const uint8_t *data, int width, int height, int linesize);

struct PointF {
	float x = 0.0f;
	float y = 0.0f;
};

/* cv2.getPerspectiveTransform + cv2.warpPerspective(INTER_CUBIC,
 * BORDER_REPLICATE): maps quad (tl, tr, br, bl) onto a width x height image */
SPECTRA_VISION_API Image WarpQuad(const Image &img, const PointF quad[4], int width, int height);

/* Rotates 90 degrees counter-clockwise (np.rot90) */
SPECTRA_VISION_API Image Rotate90(const Image &img);

} // namespace spectra
