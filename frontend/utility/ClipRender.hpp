#pragma once

#include "ClipExport.hpp"

#include <cstdint>
#include <string>
#include <vector>

/*
 * Censoring for the Spectra clip maker: shapes drawn over the video for part
 * or all of a clip, and an exporter that re-encodes the video to burn them in.
 */
namespace ClipRender {

enum class Shape { Rectangle, Ellipse };
enum class Fill { Solid, Pixelate, Blur };

struct Layer {
	Shape shape = Shape::Rectangle;
	Fill fill = Fill::Solid;
	uint8_t r = 0, g = 0, b = 0;
	/* Pixelate block size / blur radius, 1-100 (relative to frame height) */
	int strength = 50;
	/* Position and size as fractions of the frame (0-1) */
	double x = 0.0, y = 0.0, w = 1.0, h = 1.0;
	/* Visible span in seconds on the joined timeline, end exclusive */
	double start = 0.0, end = 0.0;

	bool ActiveAt(double t) const { return t >= start && t < end; }
};

/* Planar 8-bit YUV 4:2:0 image */
struct Planes {
	uint8_t *data[3] = {};
	int linesize[3] = {};
	int width = 0, height = 0;
	bool fullRange = false;
	bool bt709 = true;
};

/* A picture drawn over the whole frame for a span of time, e.g. a caption:
 * straight-alpha BGRA, scaled to the frame if its size differs. Only the
 * box [x0, x1) x [y0, y1) (in picture pixels) has anything drawn. */
struct Overlay {
	double start = 0.0, end = 0.0; /* on the joined timeline, end exclusive */
	int width = 0, height = 0;
	std::vector<uint8_t> bgra;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

	bool ActiveAt(double t) const { return t >= start && t < end; }
};

/* Pixelate block edge / blur radius in luma pixels for a frame height */
int PixelateBlockSize(int strength, int height);
int BlurRadius(int strength, int height);

/* Draws the layers active at `t` into `image`, in order. */
void Apply(const Planes &image, const std::vector<Layer> &layers, double t);

/* Blends an overlay over `image` */
void Blend(const Planes &image, const Overlay &overlay);

/*
 * Like ClipExport::Export, but decodes and re-encodes the video so the cut is
 * frame accurate and `layers` are burned in, then `overlays` over them.
 * Audio is copied as is. Layer and overlay times are on the same joined
 * timeline as `startSec` / `endSec`.
 */
bool Export(const std::vector<std::string> &inputs, double startSec, double endSec, const std::string &output,
	    const std::vector<Layer> &layers, std::string &error,
	    const ClipExport::ProgressCallback &progress = nullptr, const std::vector<Overlay> &overlays = {});

} // namespace ClipRender
