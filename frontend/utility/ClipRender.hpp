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

/* How hard a re-encode compresses. High keeps the look of the recording
 * (about 15 Mbps for 1080p60); Medium is about half of that and Small a
 * quarter, for clips that are going to be shared. YouTube1080p follows
 * YouTube's upload recommendations: at most 1080p, 12 Mbps at 60 fps or
 * 8 Mbps at 30 fps, High profile, a keyframe every half second. */
enum class Quality { High, Medium, Small, YouTube1080p };

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

/* Pixelate block edge / blur radius in luma pixels for a frame height */
int PixelateBlockSize(int strength, int height);
int BlurRadius(int strength, int height);

/* Draws the layers active at `t` into `image`, in order. */
void Apply(const Planes &image, const std::vector<Layer> &layers, double t);

/*
 * Like ClipExport::Export, but decodes and re-encodes the video so the cut is
 * frame accurate and `layers` are burned in. Audio is copied as is. Layer
 * times are on the same joined timeline as `startSec` / `endSec`.
 */
bool Export(const std::vector<std::string> &inputs, double startSec, double endSec, const std::string &output,
	    const std::vector<Layer> &layers, std::string &error,
	    const ClipExport::ProgressCallback &progress = nullptr, Quality quality = Quality::High);

} // namespace ClipRender
