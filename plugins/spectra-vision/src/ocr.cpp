#include <spectra-vision/ocr.hpp>

#include "geometry.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <numeric>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/*
 * Port of RapidOCR 3.9's pipeline for use_det=True, use_cls=False,
 * use_rec=True with its default config:
 *   Global: min_side_len 30, max_side_len 2000, use_vertical_padding,
 *           min_height 30, width_height_ratio 8
 *   Det:    PP-OCRv6 small, limit_side_len 736 (limit_type min), mean/std 0.5,
 *           thresh 0.3, box_thresh 0.5, max_candidates 1000,
 *           unclip_ratio 1.6, use_dilation, score_mode fast
 *   Rec:    PP-OCRv6 small, rec_img_shape [3, 48, 320], rec_batch_num 6
 */

namespace spectra {

using detail::PointI;
using detail::RotatedRect;

namespace {

std::filesystem::path PathFromUtf8(const std::string &s)
{
	return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string Utf8FromPath(const std::filesystem::path &p)
{
	std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

constexpr int kMinSideLen = 30;
constexpr int kMaxSideLen = 2000;
constexpr int kMinHeight = 30;
constexpr float kWidthHeightRatio = 8.0f;

constexpr int kDetLimitSideLen = 736;
constexpr float kDetThresh = 0.3f;
constexpr float kDetBoxThresh = 0.5f;
constexpr size_t kDetMaxCandidates = 1000;
constexpr float kDetUnclipRatio = 1.6f;
constexpr int kDetMinSize = 3;
constexpr int kBoxSortYThreshold = 10;

constexpr int kRecHeight = 48;
constexpr int kRecWidth = 320;
constexpr size_t kRecBatch = 6;

/* Python round(): half to even */
inline double PyRound(double v)
{
	return std::nearbyint(v);
}

inline int Round32(int v)
{
	return (int)(PyRound(v / 32.0) * 32);
}

struct Quad {
	PointF p[4];
};

/* Resizes so the sides are multiples of 32 (reduce_max_side/increase_min_side) */
Image ResizeTo32(const Image &img, double ratio, double &ratioH, double &ratioW)
{
	int rh = Round32((int)(img.height * ratio));
	int rw = Round32((int)(img.width * ratio));
	if (rh <= 0 || rw <= 0) {
		ratioH = ratioW = 1.0;
		return img;
	}
	ratioH = (double)img.height / rh;
	ratioW = (double)img.width / rw;
	return ResizeLinear(img, rw, rh);
}

Image ResizeWithinBounds(const Image &img, double &ratioH, double &ratioW)
{
	ratioH = ratioW = 1.0;
	Image out = img;
	int maxSide = std::max(out.height, out.width);
	if (maxSide > kMaxSideLen) {
		double ratio = out.height > out.width ? (double)kMaxSideLen / out.height
						      : (double)kMaxSideLen / out.width;
		out = ResizeTo32(out, ratio, ratioH, ratioW);
	}
	int minSide = std::min(out.height, out.width);
	if (minSide < kMinSideLen) {
		double ratio = out.height < out.width ? (double)kMinSideLen / out.height
						      : (double)kMinSideLen / out.width;
		out = ResizeTo32(out, ratio, ratioH, ratioW);
	}
	return out;
}

/* get_mini_boxes: min-area rect corners ordered tl, tr, br, bl */
Quad MiniBox(const RotatedRect &rect, float &shortSide)
{
	PointF pts[4];
	detail::BoxPoints(rect, pts);
	std::stable_sort(pts, pts + 4, [](const PointF &a, const PointF &b) { return a.x < b.x; });

	int i1, i2, i3, i4;
	if (pts[1].y > pts[0].y) {
		i1 = 0;
		i4 = 1;
	} else {
		i1 = 1;
		i4 = 0;
	}
	if (pts[3].y > pts[2].y) {
		i2 = 2;
		i3 = 3;
	} else {
		i2 = 3;
		i3 = 2;
	}
	shortSide = std::min(rect.width, rect.height);
	return Quad{{pts[i1], pts[i2], pts[i3], pts[i4]}};
}

/* order_points_clockwise */
Quad OrderClockwise(const Quad &q)
{
	PointF pts[4] = {q.p[0], q.p[1], q.p[2], q.p[3]};
	std::stable_sort(pts, pts + 4, [](const PointF &a, const PointF &b) { return a.x < b.x; });
	PointF left[2] = {pts[0], pts[1]};
	PointF right[2] = {pts[2], pts[3]};
	if (left[1].y < left[0].y) {
		std::swap(left[0], left[1]);
	}
	if (right[1].y < right[0].y) {
		std::swap(right[0], right[1]);
	}
	return Quad{{left[0], right[0], right[1], left[1]}};
}

float Dist(const PointF &a, const PointF &b)
{
	return std::hypot(a.x - b.x, a.y - b.y);
}

} // namespace

/* ------------------------------------------------------------------------- */

class OcrEngineImpl final : public OcrEngine {
public:
	OcrEngineImpl() : env(ORT_LOGGING_LEVEL_FATAL, "spectra-ocr") {}

	bool Init(const Options &options, std::string &error)
	{
		std::filesystem::path dir =
			PathFromUtf8(options.modelDir.empty() ? DefaultModelDir() : options.modelDir);
		auto det = dir / "PP-OCRv6_det_small.onnx";
		auto rec = dir / "PP-OCRv6_rec_small.onnx";
		if (!std::filesystem::exists(det) || !std::filesystem::exists(rec)) {
			error = "OCR models not found in " + Utf8FromPath(dir);
			return false;
		}

		try {
			Ort::SessionOptions so;
			so.SetIntraOpNumThreads(std::max(1, options.threads));
			so.SetInterOpNumThreads(1);
			so.DisableCpuMemArena();
			so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
			so.SetLogSeverityLevel(4);

			detSession = std::make_unique<Ort::Session>(env, det.c_str(), so);
			recSession = std::make_unique<Ort::Session>(env, rec.c_str(), so);

			Ort::AllocatorWithDefaultOptions alloc;
			detInput = detSession->GetInputNameAllocated(0, alloc).get();
			detOutput = detSession->GetOutputNameAllocated(0, alloc).get();
			recInput = recSession->GetInputNameAllocated(0, alloc).get();
			recOutput = recSession->GetOutputNameAllocated(0, alloc).get();

			Ort::ModelMetadata meta = recSession->GetModelMetadata();
			Ort::AllocatedStringPtr chars = meta.LookupCustomMetadataMapAllocated("character", alloc);
			if (!chars) {
				error = "Recognition model has no character list";
				return false;
			}
			LoadCharacters(chars.get());
		} catch (const Ort::Exception &e) {
			error = e.what();
			return false;
		}
		return true;
	}

	std::vector<OcrBox> Read(const Image &input, float textScore) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<OcrBox> out;
		if (input.empty()) {
			return out;
		}

		try {
			double ratioH, ratioW;
			Image img = ResizeWithinBounds(input, ratioH, ratioW);

			/* apply_vertical_padding */
			int padTop = 0;
			bool tooWide = (float)img.width / img.height > kWidthHeightRatio;
			if (img.height <= kMinHeight || tooWide) {
				int newH = std::max((int)(img.width / kWidthHeightRatio), kMinHeight) * 2;
				padTop = (int)(std::abs(newH - img.height) / 2);
				img = PadConstant(img, padTop, padTop, 0, 0);
			}

			std::vector<Quad> boxes = Detect(img);
			if (boxes.empty()) {
				return out;
			}

			std::vector<Image> crops;
			crops.reserve(boxes.size());
			for (const Quad &q : boxes) {
				crops.push_back(CropQuad(img, q));
			}

			std::vector<std::pair<std::string, float>> rec = Recognize(crops);

			for (size_t i = 0; i < boxes.size(); i++) {
				const std::string &text = rec[i].first;
				if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
					continue;
				}
				if (rec[i].second < textScore) {
					continue;
				}

				OcrBox box;
				box.text = text;
				box.score = rec[i].second;
				for (int k = 0; k < 4; k++) {
					/* map_boxes_to_original */
					float x = boxes[i].p[k].x;
					float y = boxes[i].p[k].y - padTop;
					x = (float)(x * ratioW);
					y = (float)(y * ratioH);
					x = std::clamp(x, 0.0f, (float)input.width);
					y = std::clamp(y, 0.0f, (float)input.height);
					box.quad[k] = {x, y};
				}
				box.x0 = std::min({box.quad[0].x, box.quad[1].x, box.quad[2].x, box.quad[3].x});
				box.x1 = std::max({box.quad[0].x, box.quad[1].x, box.quad[2].x, box.quad[3].x});
				box.y0 = std::min({box.quad[0].y, box.quad[1].y, box.quad[2].y, box.quad[3].y});
				box.y1 = std::max({box.quad[0].y, box.quad[1].y, box.quad[2].y, box.quad[3].y});
				out.push_back(std::move(box));
			}
		} catch (const Ort::Exception &) {
			out.clear();
		}
		return out;
	}

	std::string RecognizeLine(const Image &input) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (input.empty()) {
			return {};
		}
		try {
			double ratioH, ratioW;
			Image img = ResizeWithinBounds(input, ratioH, ratioW);
			std::vector<Image> one{img};
			auto rec = Recognize(one);
			std::string text;
			for (auto &r : rec) {
				if (!text.empty()) {
					text += ' ';
				}
				text += r.first;
			}
			return text;
		} catch (const Ort::Exception &) {
			return {};
		}
	}

private:
	Ort::Env env;
	std::unique_ptr<Ort::Session> detSession;
	std::unique_ptr<Ort::Session> recSession;
	std::string detInput, detOutput, recInput, recOutput;
	std::vector<std::string> characters;
	std::mutex mutex;

	void LoadCharacters(const char *list)
	{
		characters.clear();
		characters.push_back("blank");
		std::string s(list);
		size_t start = 0;
		while (start <= s.size()) {
			size_t end = s.find('\n', start);
			if (end == std::string::npos) {
				if (start < s.size()) {
					characters.push_back(s.substr(start));
				}
				break;
			}
			std::string line = s.substr(start, end - start);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			characters.push_back(line);
			start = end + 1;
		}
		characters.push_back(" ");
	}

	/* --------------------------------------------------------------- */
	/* Detection (TextDetector + DBPostProcess) */

	std::vector<Quad> Detect(const Image &img)
	{
		/* DetPreProcess, limit_type "min" */
		double ratio = 1.0;
		if (std::min(img.height, img.width) < kDetLimitSideLen) {
			ratio = img.height < img.width ? (double)kDetLimitSideLen / img.height
						       : (double)kDetLimitSideLen / img.width;
		}
		int rh = Round32((int)(img.height * ratio));
		int rw = Round32((int)(img.width * ratio));
		if (rh <= 0 || rw <= 0) {
			return {};
		}
		Image resized = ResizeLinear(img, rw, rh);

		std::vector<float> input((size_t)3 * rh * rw);
		const size_t plane = (size_t)rh * rw;
		for (int y = 0; y < rh; y++) {
			const uint8_t *r = resized.row(y);
			for (int x = 0; x < rw; x++) {
				for (int c = 0; c < 3; c++) {
					input[c * plane + (size_t)y * rw + x] = (r[x * 3 + c] / 255.0f - 0.5f) / 0.5f;
				}
			}
		}

		std::array<int64_t, 4> shape{1, 3, rh, rw};
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), 4);
		const char *inNames[] = {detInput.c_str()};
		const char *outNames[] = {detOutput.c_str()};
		auto outputs = detSession->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);

		auto info = outputs[0].GetTensorTypeAndShapeInfo();
		std::vector<int64_t> oshape = info.GetShape();
		const int ph = (int)oshape[2];
		const int pw = (int)oshape[3];
		const float *pred = outputs[0].GetTensorData<float>();
		std::vector<float> map(pred, pred + (size_t)ph * pw);

		/* segmentation > thresh, dilated with a 2x2 kernel (anchor 1,1) */
		std::vector<uint8_t> seg(map.size());
		for (size_t i = 0; i < map.size(); i++) {
			seg[i] = map[i] > kDetThresh;
		}
		std::vector<uint8_t> mask(map.size(), 0);
		for (int y = 0; y < ph; y++) {
			for (int x = 0; x < pw; x++) {
				uint8_t v = 0;
				for (int dy = -1; dy <= 0 && !v; dy++) {
					for (int dx = -1; dx <= 0 && !v; dx++) {
						int sx = x + dx, sy = y + dy;
						if (sx >= 0 && sy >= 0) {
							v = seg[(size_t)sy * pw + sx];
						}
					}
				}
				mask[(size_t)y * pw + x] = v;
			}
		}

		std::vector<Quad> boxes = BoxesFromBitmap(map, mask, pw, ph, img.width, img.height);
		boxes = FilterBoxes(boxes, img.height, img.width);
		SortBoxes(boxes);
		return boxes;
	}

	std::vector<Quad> BoxesFromBitmap(const std::vector<float> &pred, const std::vector<uint8_t> &mask, int width,
					  int height, int destW, int destH)
	{
		std::vector<Quad> boxes;
		auto components = detail::ConnectedComponents(mask, width, height, kDetMaxCandidates);
		for (const auto &pixels : components) {
			std::vector<PointF> pts;
			pts.reserve(pixels.size());
			for (const PointI &p : pixels) {
				pts.push_back({(float)p.x, (float)p.y});
			}

			float sside;
			Quad box = MiniBox(detail::MinAreaRect(pts), sside);
			if (sside < kDetMinSize) {
				continue;
			}

			float score = detail::PolygonMean(pred, width, height, box.p, 4);
			if (kDetBoxThresh > score) {
				continue;
			}

			/* unclip: offset the (integer-truncated) box by area * ratio /
			 * perimeter with round joins; its min-area rect is the box
			 * grown by that distance on every side */
			float distance =
				detail::PolygonArea(box.p, 4) * kDetUnclipRatio / detail::PolygonPerimeter(box.p, 4);
			std::vector<PointF> ipts;
			for (const PointF &p : box.p) {
				ipts.push_back({(float)(int)p.x, (float)(int)p.y});
			}
			RotatedRect r = detail::MinAreaRect(ipts);
			r.width += 2 * distance;
			r.height += 2 * distance;
			Quad expanded = MiniBox(r, sside);
			if (sside < kDetMinSize + 2) {
				continue;
			}

			for (PointF &p : expanded.p) {
				p.x = std::clamp((float)PyRound(p.x / width * destW), 0.0f, (float)destW);
				p.y = std::clamp((float)PyRound(p.y / height * destH), 0.0f, (float)destH);
				p.x = (float)(int)p.x;
				p.y = (float)(int)p.y;
			}
			boxes.push_back(expanded);
		}
		return boxes;
	}

	std::vector<Quad> FilterBoxes(const std::vector<Quad> &boxes, int imgH, int imgW)
	{
		std::vector<Quad> out;
		for (const Quad &b : boxes) {
			Quad q = OrderClockwise(b);
			for (PointF &p : q.p) {
				p.x = (float)(int)std::min(std::max(p.x, 0.0f), (float)(imgW - 1));
				p.y = (float)(int)std::min(std::max(p.y, 0.0f), (float)(imgH - 1));
			}
			int rectW = (int)Dist(q.p[0], q.p[1]);
			int rectH = (int)Dist(q.p[0], q.p[3]);
			if (rectW <= 3 || rectH <= 3) {
				continue;
			}
			out.push_back(q);
		}
		return out;
	}

	static void SortBoxes(std::vector<Quad> &boxes)
	{
		std::stable_sort(boxes.begin(), boxes.end(),
				 [](const Quad &a, const Quad &b) { return a.p[0].y < b.p[0].y; });
		std::vector<int> lineIds(boxes.size(), 0);
		for (size_t i = 1; i < boxes.size(); i++) {
			lineIds[i] = lineIds[i - 1] +
				     ((boxes[i].p[0].y - boxes[i - 1].p[0].y) >= kBoxSortYThreshold ? 1 : 0);
		}
		std::vector<size_t> order(boxes.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			if (lineIds[a] != lineIds[b]) {
				return lineIds[a] < lineIds[b];
			}
			return boxes[a].p[0].x < boxes[b].p[0].x;
		});
		std::vector<Quad> sorted;
		for (size_t i : order) {
			sorted.push_back(boxes[i]);
		}
		boxes.swap(sorted);
	}

	/* get_rotate_crop_image */
	static Image CropQuad(const Image &img, const Quad &q)
	{
		int w = (int)std::max(Dist(q.p[0], q.p[1]), Dist(q.p[2], q.p[3]));
		int h = (int)std::max(Dist(q.p[0], q.p[3]), Dist(q.p[1], q.p[2]));
		Image crop = WarpQuad(img, q.p, w, h);
		if (crop.width > 0 && (double)crop.height / crop.width >= 1.5) {
			crop = Rotate90(crop);
		}
		return crop;
	}

	/* --------------------------------------------------------------- */
	/* Recognition (TextRecognizer + CTCLabelDecode) */

	std::vector<std::pair<std::string, float>> Recognize(const std::vector<Image> &imgs)
	{
		std::vector<std::pair<std::string, float>> results(imgs.size(), {"", 0.0f});
		std::vector<double> ratios(imgs.size());
		for (size_t i = 0; i < imgs.size(); i++) {
			ratios[i] = imgs[i].height ? (double)imgs[i].width / imgs[i].height : 0.0;
		}
		std::vector<size_t> order(imgs.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ratios[a] < ratios[b]; });

		for (size_t beg = 0; beg < imgs.size(); beg += kRecBatch) {
			size_t end = std::min(imgs.size(), beg + kRecBatch);
			double maxRatio = (double)kRecWidth / kRecHeight;
			for (size_t i = beg; i < end; i++) {
				maxRatio = std::max(maxRatio, ratios[order[i]]);
			}
			const int imgW = (int)(kRecHeight * maxRatio);
			const size_t batch = end - beg;
			const size_t plane = (size_t)kRecHeight * imgW;
			std::vector<float> input(batch * 3 * plane, 0.0f);

			for (size_t b = 0; b < batch; b++) {
				const Image &src = imgs[order[beg + b]];
				if (src.empty()) {
					continue;
				}
				double ratio = (double)src.width / src.height;
				int resizedW = (int)std::ceil(kRecHeight * ratio) > imgW
						       ? imgW
						       : (int)std::ceil(kRecHeight * ratio);
				resizedW = std::max(resizedW, 1);
				Image r = ResizeLinear(src, resizedW, kRecHeight);
				float *dst = input.data() + b * 3 * plane;
				for (int y = 0; y < kRecHeight; y++) {
					const uint8_t *row = r.row(y);
					for (int x = 0; x < resizedW; x++) {
						for (int c = 0; c < 3; c++) {
							dst[c * plane + (size_t)y * imgW + x] =
								(row[x * 3 + c] / 255.0f - 0.5f) / 0.5f;
						}
					}
				}
			}

			std::array<int64_t, 4> shape{(int64_t)batch, 3, kRecHeight, imgW};
			Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value tensor =
				Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), 4);
			const char *inNames[] = {recInput.c_str()};
			const char *outNames[] = {recOutput.c_str()};
			auto outputs = recSession->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);

			std::vector<int64_t> oshape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
			const int64_t steps = oshape[1];
			const int64_t classes = oshape[2];
			const float *preds = outputs[0].GetTensorData<float>();

			for (size_t b = 0; b < batch; b++) {
				std::string text;
				std::vector<double> confs;
				int64_t prev = -1;
				for (int64_t t = 0; t < steps; t++) {
					const float *p = preds + (b * steps + t) * classes;
					int64_t idx = std::max_element(p, p + classes) - p;
					float prob = p[idx];
					bool keep = idx != prev && idx != 0;
					prev = idx;
					if (!keep) {
						continue;
					}
					if (idx < (int64_t)characters.size()) {
						text += characters[idx];
					}
					confs.push_back(std::round(prob * 1e5) / 1e5);
				}
				double score = 0.0;
				if (!confs.empty()) {
					score = std::accumulate(confs.begin(), confs.end(), 0.0) / confs.size();
				}
				results[order[beg + b]] = {text, (float)(std::round(score * 1e5) / 1e5)};
			}
		}
		return results;
	}
};

std::unique_ptr<OcrEngine> OcrEngine::Create(const Options &options, std::string &error)
{
	auto engine = std::make_unique<OcrEngineImpl>();
	if (!engine->Init(options, error)) {
		return nullptr;
	}
	return engine;
}

std::string DefaultModelDir()
{
#ifdef _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&DefaultModelDir), &module);
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(module, path, MAX_PATH);
	std::filesystem::path dir = std::filesystem::path(path).parent_path(); /* bin/64bit */
	return Utf8FromPath(dir.parent_path().parent_path() / "data" / "spectra-vision" / "models");
#else
	return "../../data/spectra-vision/models";
#endif
}

} // namespace spectra
