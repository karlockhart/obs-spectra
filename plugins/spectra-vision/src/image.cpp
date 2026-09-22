#include <spectra-vision/image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>

namespace spectra {

Image Crop(const Image &img, int x0, int y0, int x1, int y1)
{
	x0 = std::clamp(x0, 0, img.width);
	x1 = std::clamp(x1, x0, img.width);
	y0 = std::clamp(y0, 0, img.height);
	y1 = std::clamp(y1, y0, img.height);

	Image out(x1 - x0, y1 - y0, img.channels);
	const size_t rowBytes = (size_t)out.width * img.channels;
	for (int y = 0; y < out.height; y++) {
		memcpy(out.row(y), img.at(x0, y0 + y), rowBytes);
	}
	return out;
}

/* ------------------------------------------------------------------------- */
/* OpenCV INTER_LINEAR (8-bit) */

namespace {
constexpr int kCoefBits = 11;
constexpr int kCoefScale = 1 << kCoefBits;

struct LinearTap {
	int index;
	short alpha0;
	short alpha1;
};

std::vector<LinearTap> LinearTaps(int srcSize, int dstSize)
{
	std::vector<LinearTap> taps(dstSize);
	const double scale = (double)srcSize / dstSize;
	for (int d = 0; d < dstSize; d++) {
		float f = (float)((d + 0.5) * scale - 0.5);
		int s = (int)std::floor(f);
		f -= s;
		if (s < 0) {
			s = 0;
			f = 0.0f;
		}
		if (s >= srcSize - 1) {
			s = srcSize - 1;
			f = 0.0f;
		}
		taps[d].index = s;
		taps[d].alpha0 = (short)std::lround((1.0f - f) * kCoefScale);
		taps[d].alpha1 = (short)std::lround(f * kCoefScale);
	}
	return taps;
}
} // namespace

Image ResizeLinear(const Image &img, int width, int height)
{
	if (img.empty() || width <= 0 || height <= 0) {
		return Image(std::max(width, 0), std::max(height, 0), img.channels);
	}
	if (width == img.width && height == img.height) {
		return img;
	}

	const int cn = img.channels;
	const std::vector<LinearTap> xt = LinearTaps(img.width, width);
	const std::vector<LinearTap> yt = LinearTaps(img.height, height);

	/* Horizontal pass into fixed-point rows, then vertical blend with the
	 * rounding of OpenCV's vectorised VResizeLinear for 8-bit output. */
	std::vector<int> rowCache[2];
	int cachedIndex[2] = {-1, -1};
	auto horizontal = [&](int srcY, std::vector<int> &out) {
		out.resize((size_t)width * cn);
		const uint8_t *src = img.row(srcY);
		for (int x = 0; x < width; x++) {
			const LinearTap &t = xt[x];
			const int s1 = std::min(t.index + 1, img.width - 1);
			for (int c = 0; c < cn; c++) {
				out[(size_t)x * cn + c] =
					src[(size_t)t.index * cn + c] * t.alpha0 + src[(size_t)s1 * cn + c] * t.alpha1;
			}
		}
	};
	auto rowFor = [&](int srcY) -> const std::vector<int> & {
		for (int i = 0; i < 2; i++) {
			if (cachedIndex[i] == srcY) {
				return rowCache[i];
			}
		}
		int slot = cachedIndex[0] == -1 ? 0 : (cachedIndex[1] == -1 ? 1 : (srcY & 1));
		horizontal(srcY, rowCache[slot]);
		cachedIndex[slot] = srcY;
		return rowCache[slot];
	};

	Image out(width, height, cn);
	for (int y = 0; y < height; y++) {
		const LinearTap &t = yt[y];
		const int s1 = std::min(t.index + 1, img.height - 1);
		std::vector<int> r0 = rowFor(t.index);
		const std::vector<int> &r1 = rowFor(s1);
		uint8_t *dst = out.row(y);
		for (size_t i = 0; i < r0.size(); i++) {
			int v = (((t.alpha0 * (r0[i] >> 4)) >> 16) + ((t.alpha1 * (r1[i] >> 4)) >> 16) + 2) >> 2;
			dst[i] = (uint8_t)std::clamp(v, 0, 255);
		}
	}
	return out;
}

Image PadConstant(const Image &img, int top, int bottom, int left, int right)
{
	Image out(img.width + left + right, img.height + top + bottom, img.channels, 0);
	for (int y = 0; y < img.height; y++) {
		memcpy(out.at(left, top + y), img.row(y), (size_t)img.width * img.channels);
	}
	return out;
}

Image FromBGRA(const uint8_t *data, int width, int height, int linesize)
{
	Image out(width, height, 3);
	for (int y = 0; y < height; y++) {
		const uint8_t *src = data + (size_t)y * linesize;
		uint8_t *dst = out.row(y);
		for (int x = 0; x < width; x++) {
			dst[x * 3 + 0] = src[x * 4 + 0];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 2];
		}
	}
	return out;
}

Image Rotate90(const Image &img)
{
	Image out(img.height, img.width, img.channels);
	for (int y = 0; y < out.height; y++) {
		for (int x = 0; x < out.width; x++) {
			memcpy(out.at(x, y), img.at(img.width - 1 - y, x), img.channels);
		}
	}
	return out;
}

/* ------------------------------------------------------------------------- */
/* Perspective warp with OpenCV's bicubic (A = -0.75, 1/32 sub-pixel table) */

namespace {
using Mat3 = std::array<double, 9>;

/* Solves the 8x8 system of cv::getPerspectiveTransform */
bool PerspectiveTransform(const PointF src[4], const PointF dst[4], Mat3 &m)
{
	double a[8][9] = {};
	for (int i = 0; i < 4; i++) {
		a[i][0] = a[i + 4][3] = src[i].x;
		a[i][1] = a[i + 4][4] = src[i].y;
		a[i][2] = a[i + 4][5] = 1;
		a[i][6] = -src[i].x * dst[i].x;
		a[i][7] = -src[i].y * dst[i].x;
		a[i + 4][6] = -src[i].x * dst[i].y;
		a[i + 4][7] = -src[i].y * dst[i].y;
		a[i][8] = dst[i].x;
		a[i + 4][8] = dst[i].y;
	}
	for (int col = 0; col < 8; col++) {
		int pivot = col;
		for (int r = col + 1; r < 8; r++) {
			if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
				pivot = r;
			}
		}
		if (std::fabs(a[pivot][col]) < 1e-12) {
			return false;
		}
		std::swap(a[col], a[pivot]);
		for (int r = 0; r < 8; r++) {
			if (r == col) {
				continue;
			}
			double f = a[r][col] / a[col][col];
			for (int c = col; c < 9; c++) {
				a[r][c] -= f * a[col][c];
			}
		}
	}
	for (int i = 0; i < 8; i++) {
		m[i] = a[i][8] / a[i][i];
	}
	m[8] = 1.0;
	return true;
}

bool Invert(const Mat3 &m, Mat3 &inv)
{
	double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
		     m[2] * (m[3] * m[7] - m[4] * m[6]);
	if (std::fabs(det) < 1e-18) {
		return false;
	}
	double d = 1.0 / det;
	inv = {(m[4] * m[8] - m[5] * m[7]) * d, (m[2] * m[7] - m[1] * m[8]) * d, (m[1] * m[5] - m[2] * m[4]) * d,
	       (m[5] * m[6] - m[3] * m[8]) * d, (m[0] * m[8] - m[2] * m[6]) * d, (m[2] * m[3] - m[0] * m[5]) * d,
	       (m[3] * m[7] - m[4] * m[6]) * d, (m[1] * m[6] - m[0] * m[7]) * d, (m[0] * m[4] - m[1] * m[3]) * d};
	return true;
}

constexpr int kTabBits = 5;
constexpr int kTabSize = 1 << kTabBits;
constexpr int kRemapBits = 15;
constexpr int kRemapScale = 1 << kRemapBits;

void CubicCoeffs(float x, float c[4])
{
	const float A = -0.75f;
	c[0] = ((A * (x + 1) - 5 * A) * (x + 1) + 8 * A) * (x + 1) - 4 * A;
	c[1] = ((A + 2) * x - (A + 3)) * x * x + 1;
	c[2] = ((A + 2) * (1 - x) - (A + 3)) * (1 - x) * (1 - x) + 1;
	c[3] = 1.f - c[0] - c[1] - c[2];
}

/* Fixed-point 4x4 kernels for every 1/32 sub-pixel offset, normalised to
 * sum exactly to the fixed-point scale (as cv::initInterTab2D does) */
struct CubicTable {
	std::array<std::array<int, 16>, kTabSize * kTabSize> tab;

	CubicTable()
	{
		for (int fy = 0; fy < kTabSize; fy++) {
			float cy[4];
			CubicCoeffs((float)fy / kTabSize, cy);
			for (int fx = 0; fx < kTabSize; fx++) {
				float cx[4];
				CubicCoeffs((float)fx / kTabSize, cx);
				std::array<int, 16> &k = tab[fy * kTabSize + fx];
				int sum = 0;
				for (int i = 0; i < 4; i++) {
					for (int j = 0; j < 4; j++) {
						k[i * 4 + j] = (int)std::lround(cy[i] * cx[j] * kRemapScale);
						sum += k[i * 4 + j];
					}
				}
				if (sum != kRemapScale) {
					int diff = sum - kRemapScale;
					int ksize2 = 4 / 2;
					int mk1 = ksize2 * 4 + ksize2, Mk1 = mk1, mk2 = mk1, Mk2 = mk1;
					for (int i = ksize2; i < ksize2 + 2; i++) {
						for (int j = ksize2; j < ksize2 + 2; j++) {
							int idx = i * 4 + j;
							if (k[idx] < k[mk1]) {
								mk1 = idx;
							} else if (k[idx] > k[Mk1]) {
								Mk1 = idx;
							}
						}
					}
					(void)mk2;
					(void)Mk2;
					if (diff < 0) {
						k[Mk1] -= diff;
					} else {
						k[mk1] -= diff;
					}
				}
			}
		}
	}
};

const CubicTable &Table()
{
	static const CubicTable table;
	return table;
}
} // namespace

Image WarpQuad(const Image &img, const PointF quad[4], int width, int height)
{
	Image out(std::max(width, 0), std::max(height, 0), img.channels);
	if (img.empty() || width <= 0 || height <= 0) {
		return out;
	}

	const PointF dst[4] = {{0, 0}, {(float)width, 0}, {(float)width, (float)height}, {0, (float)height}};
	Mat3 m, inv;
	if (!PerspectiveTransform(quad, dst, m) || !Invert(m, inv)) {
		return out;
	}

	const CubicTable &table = Table();
	const int cn = img.channels;
	for (int y = 0; y < height; y++) {
		uint8_t *d = out.row(y);
		for (int x = 0; x < width; x++) {
			double w = inv[6] * x + inv[7] * y + inv[8];
			w = w ? kTabSize / w : 0.0;
			double fx =
				std::clamp((inv[0] * x + inv[1] * y + inv[2]) * w, (double)INT_MIN, (double)INT_MAX);
			double fy =
				std::clamp((inv[3] * x + inv[4] * y + inv[5]) * w, (double)INT_MIN, (double)INT_MAX);
			int X = (int)std::lrint(fx);
			int Y = (int)std::lrint(fy);
			int sx = (X >> kTabBits) - 1;
			int sy = (Y >> kTabBits) - 1;
			const std::array<int, 16> &k =
				table.tab[(Y & (kTabSize - 1)) * kTabSize + (X & (kTabSize - 1))];

			for (int c = 0; c < cn; c++) {
				int sum = 0;
				for (int i = 0; i < 4; i++) {
					int yy = std::clamp(sy + i, 0, img.height - 1);
					const uint8_t *r = img.row(yy);
					for (int j = 0; j < 4; j++) {
						int xx = std::clamp(sx + j, 0, img.width - 1);
						sum += r[xx * cn + c] * k[i * 4 + j];
					}
				}
				d[x * cn + c] =
					(uint8_t)std::clamp((sum + (1 << (kRemapBits - 1))) >> kRemapBits, 0, 255);
			}
		}
	}
	return out;
}

} // namespace spectra
