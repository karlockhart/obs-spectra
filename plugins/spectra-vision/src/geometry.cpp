#include "geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spectra::detail {

std::vector<std::vector<PointI>> ConnectedComponents(const std::vector<uint8_t> &mask, int width, int height,
						     size_t maxComponents)
{
	std::vector<std::vector<PointI>> components;
	std::vector<uint8_t> seen(mask.size(), 0);
	std::vector<PointI> stack;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t idx = (size_t)y * width + x;
			if (!mask[idx] || seen[idx]) {
				continue;
			}
			if (components.size() >= maxComponents) {
				return components;
			}

			std::vector<PointI> pixels;
			stack.push_back({x, y});
			seen[idx] = 1;
			while (!stack.empty()) {
				PointI p = stack.back();
				stack.pop_back();
				pixels.push_back(p);
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						int nx = p.x + dx, ny = p.y + dy;
						if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
							continue;
						}
						size_t n = (size_t)ny * width + nx;
						if (mask[n] && !seen[n]) {
							seen[n] = 1;
							stack.push_back({nx, ny});
						}
					}
				}
			}
			components.push_back(std::move(pixels));
		}
	}
	return components;
}

static float Cross(const PointF &o, const PointF &a, const PointF &b)
{
	return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

std::vector<PointF> ConvexHull(std::vector<PointF> pts)
{
	std::sort(pts.begin(), pts.end(),
		  [](const PointF &a, const PointF &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
	pts.erase(std::unique(pts.begin(), pts.end(),
			      [](const PointF &a, const PointF &b) { return a.x == b.x && a.y == b.y; }),
		  pts.end());
	if (pts.size() < 3) {
		return pts;
	}

	std::vector<PointF> hull(pts.size() * 2);
	size_t k = 0;
	for (size_t i = 0; i < pts.size(); i++) {
		while (k >= 2 && Cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) {
			k--;
		}
		hull[k++] = pts[i];
	}
	for (size_t i = pts.size() - 1, t = k + 1; i > 0; i--) {
		while (k >= t && Cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0) {
			k--;
		}
		hull[k++] = pts[i - 1];
	}
	hull.resize(k - 1);
	return hull;
}

RotatedRect MinAreaRect(const std::vector<PointF> &points)
{
	RotatedRect best;
	std::vector<PointF> hull = ConvexHull(points);
	if (hull.empty()) {
		return best;
	}
	if (hull.size() == 1) {
		best.center = hull[0];
		return best;
	}
	if (hull.size() == 2) {
		float dx = hull[1].x - hull[0].x, dy = hull[1].y - hull[0].y;
		best.center = {(hull[0].x + hull[1].x) / 2, (hull[0].y + hull[1].y) / 2};
		best.width = std::sqrt(dx * dx + dy * dy);
		best.height = 0.0f;
		best.angle = (float)(std::atan2(dy, dx) * 180.0 / std::numbers::pi);
		return best;
	}

	double bestArea = -1.0;
	const size_t n = hull.size();
	for (size_t i = 0; i < n; i++) {
		const PointF &a = hull[i];
		const PointF &b = hull[(i + 1) % n];
		double ex = b.x - a.x, ey = b.y - a.y;
		double len = std::sqrt(ex * ex + ey * ey);
		if (len == 0) {
			continue;
		}
		ex /= len;
		ey /= len;

		double minU = 1e30, maxU = -1e30, minV = 1e30, maxV = -1e30;
		for (const PointF &p : hull) {
			double u = (p.x - a.x) * ex + (p.y - a.y) * ey;
			double v = -(p.x - a.x) * ey + (p.y - a.y) * ex;
			minU = std::min(minU, u);
			maxU = std::max(maxU, u);
			minV = std::min(minV, v);
			maxV = std::max(maxV, v);
		}
		double area = (maxU - minU) * (maxV - minV);
		if (bestArea < 0 || area < bestArea) {
			bestArea = area;
			double cu = (minU + maxU) / 2, cv = (minV + maxV) / 2;
			best.center = {(float)(a.x + cu * ex - cv * ey), (float)(a.y + cu * ey + cv * ex)};
			best.width = (float)(maxU - minU);
			best.height = (float)(maxV - minV);
			best.angle = (float)(std::atan2(ey, ex) * 180.0 / std::numbers::pi);
		}
	}
	return best;
}

void BoxPoints(const RotatedRect &rect, PointF out[4])
{
	double a = rect.angle * std::numbers::pi / 180.0;
	double b = std::cos(a) * 0.5, c = std::sin(a) * 0.5;
	out[0] = {(float)(rect.center.x - c * rect.height - b * rect.width),
		  (float)(rect.center.y + b * rect.height - c * rect.width)};
	out[1] = {(float)(rect.center.x + c * rect.height - b * rect.width),
		  (float)(rect.center.y - b * rect.height - c * rect.width)};
	out[2] = {2 * rect.center.x - out[0].x, 2 * rect.center.y - out[0].y};
	out[3] = {2 * rect.center.x - out[1].x, 2 * rect.center.y - out[1].y};
}

float PolygonMean(const std::vector<float> &map, int width, int height, const PointF *poly, int count)
{
	float minX = poly[0].x, maxX = poly[0].x, minY = poly[0].y, maxY = poly[0].y;
	for (int i = 1; i < count; i++) {
		minX = std::min(minX, poly[i].x);
		maxX = std::max(maxX, poly[i].x);
		minY = std::min(minY, poly[i].y);
		maxY = std::max(maxY, poly[i].y);
	}
	int xmin = std::clamp((int)std::floor(minX), 0, width - 1);
	int xmax = std::clamp((int)std::ceil(maxX), 0, width - 1);
	int ymin = std::clamp((int)std::floor(minY), 0, height - 1);
	int ymax = std::clamp((int)std::ceil(maxY), 0, height - 1);

	/* Polygon vertices truncated to integers, as cv::fillPoly receives them */
	std::vector<PointF> ip(count);
	for (int i = 0; i < count; i++) {
		ip[i] = {(float)(int)(poly[i].x - xmin), (float)(int)(poly[i].y - ymin)};
	}
	/* Orientation-independent inclusive point-in-convex-polygon test */
	float orient = 0.0f;
	for (int i = 0; i < count; i++) {
		orient += Cross(ip[0], ip[i], ip[(i + 1) % count]);
	}

	double sum = 0.0;
	size_t n = 0;
	for (int y = 0; y <= ymax - ymin; y++) {
		for (int x = 0; x <= xmax - xmin; x++) {
			PointF p{(float)x, (float)y};
			bool inside = true;
			for (int i = 0; i < count && inside; i++) {
				float c = Cross(ip[i], ip[(i + 1) % count], p);
				if ((orient >= 0 && c < -1e-3f) || (orient < 0 && c > 1e-3f)) {
					inside = false;
				}
			}
			if (inside) {
				sum += map[(size_t)(y + ymin) * width + (x + xmin)];
				n++;
			}
		}
	}
	return n ? (float)(sum / n) : 0.0f;
}

float PolygonArea(const PointF *poly, int count)
{
	double a = 0.0;
	for (int i = 0; i < count; i++) {
		const PointF &p = poly[i];
		const PointF &q = poly[(i + 1) % count];
		a += (double)p.x * q.y - (double)q.x * p.y;
	}
	return (float)std::fabs(a / 2.0);
}

float PolygonPerimeter(const PointF *poly, int count)
{
	double l = 0.0;
	for (int i = 0; i < count; i++) {
		const PointF &p = poly[i];
		const PointF &q = poly[(i + 1) % count];
		l += std::hypot((double)q.x - p.x, (double)q.y - p.y);
	}
	return (float)l;
}

} // namespace spectra::detail
