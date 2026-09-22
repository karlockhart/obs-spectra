#pragma once

#include "export.hpp"
#include "image.hpp"

#include <memory>
#include <string>
#include <vector>

namespace spectra {

/* One recognised text line. (x0, y0)-(x1, y1) is the axis-aligned bounding
 * box of `quad` (top-left, top-right, bottom-right, bottom-left) in input
 * image pixels. */
struct OcrBox {
	std::string text; /* UTF-8 */
	float score = 0.0f;
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	PointF quad[4];

	OcrBox Shifted(float dx, float dy) const
	{
		OcrBox b = *this;
		b.x0 += dx;
		b.x1 += dx;
		b.y0 += dy;
		b.y1 += dy;
		for (PointF &p : b.quad) {
			p.x += dx;
			p.y += dy;
		}
		return b;
	}
};

/*
 * Text detection + recognition compatible with RapidOCR 3.9 (PP-OCRv6 small
 * det/rec ONNX models, default parameters, no angle classifier), as used by
 * Obscura and Lucida. Thread-safe: calls are serialised internally.
 */
class SPECTRA_VISION_API OcrEngine {
public:
	struct Options {
		/* Directory holding PP-OCRv6_det_small.onnx and PP-OCRv6_rec_small.onnx;
		 * empty = the models installed next to spectra-vision */
		std::string modelDir;
		/* onnxruntime intra-op threads (inter-op is always 1) */
		int threads = 2;
	};

	static std::unique_ptr<OcrEngine> Create(const Options &options, std::string &error);

	virtual ~OcrEngine() = default;

	/* Detect and recognise text; drops lines scoring below textScore */
	virtual std::vector<OcrBox> Read(const Image &img, float textScore = 0.3f) = 0;

	/* Recognition only, for a crop known to hold one line; texts joined by ' ' */
	virtual std::string RecognizeLine(const Image &img) = 0;
};

/* Default model directory: <data>/spectra-vision/models */
SPECTRA_VISION_API std::string DefaultModelDir();

} // namespace spectra
