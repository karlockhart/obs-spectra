#include <spectra-vision/imaging.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>

namespace spectra {

/* ------------------------------------------------------------------------- */
/* Gate */

namespace {

/* cv::cvtColor(COLOR_BGR2GRAY) for 8-bit: fixed point with 14 bits */
std::vector<uint8_t> Gray(const Image &img)
{
	std::vector<uint8_t> g((size_t)img.width * img.height);
	for (int y = 0; y < img.height; y++) {
		const uint8_t *r = img.row(y);
		for (int x = 0; x < img.width; x++) {
			const uint8_t *p = r + (size_t)x * img.channels;
			if (img.channels >= 3) {
				g[(size_t)y * img.width + x] =
					(uint8_t)((p[0] * 1868 + p[1] * 9617 + p[2] * 4899 + 8192) >> 14);
			} else {
				g[(size_t)y * img.width + x] = p[0];
			}
		}
	}
	return g;
}

/* 5x5 rectangular erosion/dilation, anchor at the centre; pixels outside
 * the image are ignored (OpenCV's default morphology border value) */
std::vector<uint8_t> Morph(const std::vector<uint8_t> &src, int w, int h, bool dilate)
{
	/* separable: rows then columns */
	std::vector<uint8_t> tmp(src.size()), out(src.size());
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int v = dilate ? 0 : 255;
			for (int k = -2; k <= 2; k++) {
				int xx = x + k;
				if (xx < 0 || xx >= w) {
					continue;
				}
				int s = src[(size_t)y * w + xx];
				v = dilate ? std::max(v, s) : std::min(v, s);
			}
			tmp[(size_t)y * w + x] = (uint8_t)v;
		}
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int v = dilate ? 0 : 255;
			for (int k = -2; k <= 2; k++) {
				int yy = y + k;
				if (yy < 0 || yy >= h) {
					continue;
				}
				int s = tmp[(size_t)yy * w + x];
				v = dilate ? std::max(v, s) : std::min(v, s);
			}
			out[(size_t)y * w + x] = (uint8_t)v;
		}
	}
	return out;
}

} // namespace

std::vector<uint8_t> TextMask(const Image &img)
{
	if (img.empty()) {
		return {};
	}
	std::vector<uint8_t> gray = Gray(img);
	std::vector<uint8_t> opened = Morph(Morph(gray, img.width, img.height, false), img.width, img.height, true);
	std::vector<uint8_t> mask(gray.size());
	for (size_t i = 0; i < gray.size(); i++) {
		int tophat = (int)gray[i] - (int)opened[i];
		mask[i] = tophat > 40 ? 1 : 0;
	}
	return mask;
}

std::vector<float> Signature(const std::vector<uint8_t> &mask, int width, int height)
{
	std::vector<float> sig(height > 0 ? height : 0, 0.0f);
	if (mask.size() < (size_t)width * height) {
		return {};
	}
	for (int y = 0; y < height; y++) {
		int sum = 0;
		for (int x = 0; x < width; x++) {
			sum += mask[(size_t)y * width + x];
		}
		sig[y] = (float)sum;
	}
	return sig;
}

double SignatureDistance(const std::vector<float> &a, const std::vector<float> &b)
{
	if (a.empty() || b.empty() || a.size() != b.size()) {
		return 1.0;
	}
	double diff = 0.0, sa = 0.0, sb = 0.0;
	for (size_t i = 0; i < a.size(); i++) {
		diff += std::fabs((double)a[i] - b[i]);
		sa += a[i];
		sb += b[i];
	}
	return diff / std::max({sa, sb, 1.0});
}

std::pair<bool, double> SignatureChanged(const std::vector<float> *previous, const std::vector<float> &current,
					 double threshold)
{
	if (!previous) {
		return {true, 1.0};
	}
	double d = SignatureDistance(*previous, current);
	return {d > threshold, d};
}

/* ------------------------------------------------------------------------- */
/* Redaction */

std::vector<Rect> MergeRects(std::vector<Rect> rects)
{
	std::stable_sort(rects.begin(), rects.end(),
			 [](const Rect &a, const Rect &b) { return a.y0 < b.y0 || (a.y0 == b.y0 && a.x0 < b.x0); });
	std::vector<Rect> merged;
	for (const Rect &r : rects) {
		bool joined = false;
		for (Rect &m : merged) {
			if (r.y0 <= m.y1 + 1 && r.y1 >= m.y0 - 1 && r.x0 <= m.x1 && r.x1 >= m.x0) {
				m = {std::min(m.x0, r.x0), std::min(m.y0, r.y0), std::max(m.x1, r.x1),
				     std::max(m.y1, r.y1)};
				joined = true;
				break;
			}
		}
		if (!joined) {
			merged.push_back(r);
		}
	}
	return merged;
}

namespace {

/* numpy.percentile (linear interpolation) */
double Percentile(std::vector<double> v, double q)
{
	std::sort(v.begin(), v.end());
	double pos = (v.size() - 1) * q / 100.0;
	size_t lo = (size_t)std::floor(pos);
	size_t hi = std::min(lo + 1, v.size() - 1);
	return v[lo] + (v[hi] - v[lo]) * (pos - lo);
}

double MedianOf(std::vector<double> v)
{
	std::sort(v.begin(), v.end());
	size_t n = v.size();
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

std::array<uint8_t, 3> FillColour(const Image &img, const Rect &r, const QString &fill)
{
	if (fill != QLatin1String("auto")) {
		QString hex = fill;
		if (hex.startsWith('#')) {
			hex = hex.mid(1);
		}
		bool ok = false;
		uint rgb = hex.left(6).toUInt(&ok, 16);
		if (ok) {
			return {(uint8_t)(rgb & 0xff), (uint8_t)((rgb >> 8) & 0xff), (uint8_t)((rgb >> 16) & 0xff)};
		}
		return {0, 0, 0};
	}

	std::vector<double> lum;
	for (int y = r.y0; y < r.y1; y++) {
		for (int x = r.x0; x < r.x1; x++) {
			const uint8_t *p = img.at(x, y);
			lum.push_back((p[0] + p[1] + p[2]) / 3.0);
		}
	}
	if (lum.empty()) {
		return {0, 0, 0};
	}
	const double cut = Percentile(lum, 40);
	std::vector<double> ch[3];
	size_t i = 0;
	for (int y = r.y0; y < r.y1; y++) {
		for (int x = r.x0; x < r.x1; x++, i++) {
			if (lum[i] <= cut) {
				const uint8_t *p = img.at(x, y);
				for (int c = 0; c < 3; c++) {
					ch[c].push_back(p[c]);
				}
			}
		}
	}
	return {(uint8_t)MedianOf(ch[0]), (uint8_t)MedianOf(ch[1]), (uint8_t)MedianOf(ch[2])};
}

} // namespace

Image Redact(const Image &img, const std::vector<Rect> &rects, const QString &fill)
{
	std::vector<Rect> clamped;
	for (const Rect &r : rects) {
		if (r.x1 > r.x0 && r.y1 > r.y0) {
			clamped.push_back({std::max(0, r.x0), std::max(0, r.y0), std::min(img.width, r.x1),
					   std::min(img.height, r.y1)});
		}
	}
	Image out = img;
	for (const Rect &m : MergeRects(clamped)) {
		auto colour = FillColour(img, m, fill);
		for (int y = m.y0; y < m.y1; y++) {
			for (int x = m.x0; x < m.x1; x++) {
				uint8_t *p = out.at(x, y);
				p[0] = colour[0];
				p[1] = colour[1];
				p[2] = colour[2];
			}
		}
	}
	return out;
}

} // namespace spectra
