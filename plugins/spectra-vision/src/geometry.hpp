#pragma once

#include <spectra-vision/image.hpp>

#include <vector>

namespace spectra::detail {

struct PointI {
	int x;
	int y;
};

/* cv::RotatedRect equivalent */
struct RotatedRect {
	PointF center;
	float width = 0.0f;
	float height = 0.0f;
	float angle = 0.0f; /* degrees */
};

/* Pixel coordinates of each 8-connected component of a binary mask (non-zero
 * = set). Stands in for cv::findContours(RETR_LIST): only a component's
 * convex hull matters to the DB post-processing that uses it. */
std::vector<std::vector<PointI>> ConnectedComponents(const std::vector<uint8_t> &mask, int width, int height,
						     size_t maxComponents);

std::vector<PointF> ConvexHull(std::vector<PointF> points);

/* Rotating calipers over the convex hull (cv::minAreaRect) */
RotatedRect MinAreaRect(const std::vector<PointF> &points);

/* cv::boxPoints */
void BoxPoints(const RotatedRect &rect, PointF out[4]);

/* Mean of `map` (width x height floats) inside the polygon, over its
 * bounding box clipped to the map (cv::fillPoly + cv::mean) */
float PolygonMean(const std::vector<float> &map, int width, int height, const PointF *poly, int count);

float PolygonArea(const PointF *poly, int count);
float PolygonPerimeter(const PointF *poly, int count);

} // namespace spectra::detail
