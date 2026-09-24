#include "ClipRender.hpp"

#include <util/base.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace ClipRender {

/* ------------------------------------------------------------------------- */
/* Drawing                                                                   */

namespace {

struct PlaneView {
	uint8_t *data;
	int linesize;
	int width, height;
};

/* A layer's shape in one plane's pixel coordinates */
struct Area {
	Shape shape;
	double left, top, right, bottom;
	int x0, y0, x1, y1; /* clamped bounding box, end exclusive */

	Area(const Layer &layer, int width, int height) : shape(layer.shape)
	{
		left = layer.x * width;
		top = layer.y * height;
		right = (layer.x + layer.w) * width;
		bottom = (layer.y + layer.h) * height;
		x0 = std::clamp((int)std::floor(left), 0, width);
		y0 = std::clamp((int)std::floor(top), 0, height);
		x1 = std::clamp((int)std::ceil(right), 0, width);
		y1 = std::clamp((int)std::ceil(bottom), 0, height);
	}

	bool Empty() const { return x1 <= x0 || y1 <= y0; }

	bool Contains(int px, int py) const
	{
		double cx = px + 0.5, cy = py + 0.5;
		if (cx < left || cx >= right || cy < top || cy >= bottom) {
			return false;
		}
		if (shape == Shape::Rectangle) {
			return true;
		}
		double rx = (right - left) / 2.0, ry = (bottom - top) / 2.0;
		double dx = (cx - (left + rx)) / rx, dy = (cy - (top + ry)) / ry;
		return dx * dx + dy * dy <= 1.0;
	}
};

void SolidColor(const Layer &layer, bool fullRange, bool bt709, uint8_t out[3])
{
	const double kr = bt709 ? 0.2126 : 0.299;
	const double kb = bt709 ? 0.0722 : 0.114;
	double r = layer.r / 255.0, g = layer.g / 255.0, b = layer.b / 255.0;
	double y = kr * r + (1.0 - kr - kb) * g + kb * b;
	double cb = (b - y) / (2.0 * (1.0 - kb));
	double cr = (r - y) / (2.0 * (1.0 - kr));

	double Y = fullRange ? y * 255.0 : 16.0 + y * 219.0;
	double U = 128.0 + cb * (fullRange ? 255.0 : 224.0);
	double V = 128.0 + cr * (fullRange ? 255.0 : 224.0);
	out[0] = (uint8_t)std::clamp(std::lround(Y), 0l, 255l);
	out[1] = (uint8_t)std::clamp(std::lround(U), 0l, 255l);
	out[2] = (uint8_t)std::clamp(std::lround(V), 0l, 255l);
}

void FillPlane(const PlaneView &p, const Area &area, uint8_t value)
{
	for (int y = area.y0; y < area.y1; y++) {
		uint8_t *row = p.data + (ptrdiff_t)y * p.linesize;
		for (int x = area.x0; x < area.x1; x++) {
			if (area.Contains(x, y)) {
				row[x] = value;
			}
		}
	}
}

void PixelatePlane(const PlaneView &p, const Area &area, int block)
{
	/* Blocks sit on a frame-wide grid so they don't swim as a shape moves */
	for (int by = (area.y0 / block) * block; by < area.y1; by += block) {
		int byEnd = std::min(by + block, p.height);
		for (int bx = (area.x0 / block) * block; bx < area.x1; bx += block) {
			int bxEnd = std::min(bx + block, p.width);

			uint64_t sum = 0;
			for (int y = by; y < byEnd; y++) {
				const uint8_t *row = p.data + (ptrdiff_t)y * p.linesize;
				for (int x = bx; x < bxEnd; x++) {
					sum += row[x];
				}
			}
			uint64_t count = (uint64_t)(byEnd - by) * (uint64_t)(bxEnd - bx);
			uint8_t avg = (uint8_t)((sum + count / 2) / std::max<uint64_t>(count, 1));

			for (int y = std::max(by, area.y0); y < std::min(byEnd, area.y1); y++) {
				uint8_t *row = p.data + (ptrdiff_t)y * p.linesize;
				for (int x = std::max(bx, area.x0); x < std::min(bxEnd, area.x1); x++) {
					if (area.Contains(x, y)) {
						row[x] = avg;
					}
				}
			}
		}
	}
}

/* One box blur pass over `count` values `stride` apart, clamping at the ends */
void BoxBlur1D(int *values, int count, int stride, int radius, std::vector<int> &scratch)
{
	scratch.resize((size_t)count);
	for (int i = 0; i < count; i++) {
		scratch[(size_t)i] = values[(ptrdiff_t)i * stride];
	}

	const int window = radius * 2 + 1;
	auto at = [&](int i) {
		return scratch[(size_t)std::clamp(i, 0, count - 1)];
	};

	int sum = 0;
	for (int k = -radius; k <= radius; k++) {
		sum += at(k);
	}
	for (int i = 0; i < count; i++) {
		values[(ptrdiff_t)i * stride] = (sum + window / 2) / window;
		sum += at(i + radius + 1) - at(i - radius);
	}
}

void BlurPlane(const PlaneView &p, const Area &area, int radius)
{
	/* Three box passes approximate a gaussian; sample a margin around the
	 * shape so its edges blur into the surroundings. */
	const int passes = 3;
	const int pad = radius * passes;
	int rx0 = std::max(area.x0 - pad, 0), ry0 = std::max(area.y0 - pad, 0);
	int rx1 = std::min(area.x1 + pad, p.width), ry1 = std::min(area.y1 + pad, p.height);
	int w = rx1 - rx0, h = ry1 - ry0;
	if (w <= 0 || h <= 0) {
		return;
	}

	std::vector<int> buf((size_t)w * (size_t)h);
	for (int y = 0; y < h; y++) {
		const uint8_t *row = p.data + (ptrdiff_t)(ry0 + y) * p.linesize + rx0;
		for (int x = 0; x < w; x++) {
			buf[(size_t)y * w + x] = row[x];
		}
	}

	std::vector<int> scratch;
	for (int pass = 0; pass < passes; pass++) {
		for (int y = 0; y < h; y++) {
			BoxBlur1D(&buf[(size_t)y * w], w, 1, radius, scratch);
		}
		for (int x = 0; x < w; x++) {
			BoxBlur1D(&buf[(size_t)x], h, w, radius, scratch);
		}
	}

	for (int y = area.y0; y < area.y1; y++) {
		uint8_t *row = p.data + (ptrdiff_t)y * p.linesize;
		for (int x = area.x0; x < area.x1; x++) {
			if (area.Contains(x, y)) {
				row[x] = (uint8_t)std::clamp(buf[(size_t)(y - ry0) * w + (x - rx0)], 0, 255);
			}
		}
	}
}

} // namespace

int PixelateBlockSize(int strength, int height)
{
	/* Even, so chroma blocks line up with luma blocks */
	int block = (int)std::lround(std::clamp(strength, 1, 100) / 100.0 * height / 10.0);
	return std::max(2, block & ~1);
}

int BlurRadius(int strength, int height)
{
	return std::max(1, (int)std::lround(std::clamp(strength, 1, 100) / 100.0 * height / 40.0));
}

void Apply(const Planes &image, const std::vector<Layer> &layers, double t)
{
	const int cw = (image.width + 1) / 2, ch = (image.height + 1) / 2;
	const PlaneView planes[3] = {
		{image.data[0], image.linesize[0], image.width, image.height},
		{image.data[1], image.linesize[1], cw, ch},
		{image.data[2], image.linesize[2], cw, ch},
	};

	for (const Layer &layer : layers) {
		if (!layer.ActiveAt(t) || layer.w <= 0.0 || layer.h <= 0.0) {
			continue;
		}

		uint8_t solid[3] = {};
		if (layer.fill == Fill::Solid) {
			SolidColor(layer, image.fullRange, image.bt709, solid);
		}
		const int block = PixelateBlockSize(layer.strength, image.height);
		const int radius = BlurRadius(layer.strength, image.height);

		for (int i = 0; i < 3; i++) {
			const PlaneView &p = planes[i];
			Area area(layer, p.width, p.height);
			if (area.Empty()) {
				continue;
			}
			const int div = i == 0 ? 1 : 2;
			switch (layer.fill) {
			case Fill::Solid:
				FillPlane(p, area, solid[i]);
				break;
			case Fill::Pixelate:
				PixelatePlane(p, area, std::max(block / div, 1));
				break;
			case Fill::Blur:
				BlurPlane(p, area, std::max(radius / div, 1));
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Export                                                                    */

namespace {

struct InputDeleter {
	void operator()(AVFormatContext *ctx) const { avformat_close_input(&ctx); }
};
using InputPtr = std::unique_ptr<AVFormatContext, InputDeleter>;

struct OutputDeleter {
	void operator()(AVFormatContext *ctx) const
	{
		if (!ctx) {
			return;
		}
		if (!(ctx->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&ctx->pb);
		}
		avformat_free_context(ctx);
	}
};
using OutputPtr = std::unique_ptr<AVFormatContext, OutputDeleter>;

struct CodecDeleter {
	void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;

struct PacketDeleter {
	void operator()(AVPacket *pkt) const { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct FrameDeleter {
	void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct SwsDeleter {
	void operator()(SwsContext *ctx) const { sws_freeContext(ctx); }
};
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

std::string AvError(int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

InputPtr OpenInput(const std::string &path, std::string &error)
{
	AVFormatContext *ctx = nullptr;
	int ret = avformat_open_input(&ctx, path.c_str(), nullptr, nullptr);
	if (ret < 0) {
		error = "Could not open '" + path + "': " + AvError(ret);
		return nullptr;
	}
	InputPtr input(ctx);
	ret = avformat_find_stream_info(ctx, nullptr);
	if (ret < 0) {
		error = "Could not read stream info from '" + path + "': " + AvError(ret);
		return nullptr;
	}
	return input;
}

CodecPtr OpenDecoder(const AVStream *st, std::string &error)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		error = std::string("No decoder for '") + avcodec_get_name(st->codecpar->codec_id) + "'";
		return nullptr;
	}
	CodecPtr dec(avcodec_alloc_context3(codec));
	if (!dec || avcodec_parameters_to_context(dec.get(), st->codecpar) < 0) {
		error = "Failed to set up the video decoder";
		return nullptr;
	}
	dec->pkt_timebase = st->time_base;
	dec->thread_count = 0;
	dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	int ret = avcodec_open2(dec.get(), codec, nullptr);
	if (ret < 0) {
		error = "Failed to open the video decoder: " + AvError(ret);
		return nullptr;
	}
	return dec;
}

const AVPixelFormat *SupportedPixelFormats(AVCodecContext *ctx, const AVCodec *codec)
{
	const AVPixelFormat *formats = nullptr;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 13, 100)
	(void)ctx;
	formats = codec->pix_fmts;
#else
	avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&formats, nullptr);
#endif
	return formats;
}

AVPixelFormat PickPixelFormat(AVCodecContext *ctx, const AVCodec *codec)
{
	const AVPixelFormat *formats = SupportedPixelFormats(ctx, codec);
	if (!formats) {
		return AV_PIX_FMT_YUV420P;
	}
	for (AVPixelFormat want : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12}) {
		for (const AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++) {
			if (*f == want) {
				return want;
			}
		}
	}
	return AV_PIX_FMT_NONE;
}

/*
 * Opens the first working H.264 encoder, preferring hardware. Hardware
 * encoders fail to open when the GPU doesn't have them, so each is tried.
 */
CodecPtr OpenEncoder(const AVCodecParameters *in, AVRational frameRate, bool globalHeader, Quality quality,
		     std::string &error)
{
	static const char *names[] = {"h264_nvenc", "h264_amf",    "h264_qsv", "libx264",
				      "h264_mf",    "libopenh264", "mpeg4"};

	/* Bitrate-driven encoders get a target in bits per pixel; the rest
	 * are driven by a constant quality (CRF / CQ) at roughly the same
	 * level. High is ~0.12 bits per pixel: 15 Mbps for 1080p60, a little
	 * above typical loop recordings so the re-encode doesn't visibly
	 * lose quality. Medium halves that and Small quarters it. */
	int64_t bitsPerHundredPixels = 12;
	int64_t minBitrate = 4000000;
	const char *crf = "18";
	const char *cq = "19";
	int mpeg4Qscale = 3;
	switch (quality) {
	case Quality::High:
	case Quality::YouTube1080p:
		break;
	case Quality::Medium:
		bitsPerHundredPixels = 6;
		minBitrate = 2000000;
		crf = "22";
		cq = "24";
		mpeg4Qscale = 5;
		break;
	case Quality::Small:
		bitsPerHundredPixels = 3;
		minBitrate = 1000000;
		crf = "26";
		cq = "29";
		mpeg4Qscale = 8;
		break;
	}
	const double fps = frameRate.num / (double)std::max(frameRate.den, 1);
	const int64_t pixelsPerSecond = (int64_t)in->width * in->height * frameRate.num / std::max(frameRate.den, 1);
	int64_t bitrate = std::max<int64_t>(pixelsPerSecond * bitsPerHundredPixels / 100, minBitrate);
	int width = in->width, height = in->height;
	int gop = std::max((int)std::lround(2.0 * fps), 1);
	/* Constant quality capped at the bitrate, rather than a bitrate target */
	bool capped = false;
	const char *profile = nullptr;
	if (quality == Quality::YouTube1080p) {
		if (height > 1080) {
			width = ((in->width * 1080 / in->height) + 1) & ~1;
			height = 1080;
		}
		const bool highFps = fps > 40.0;
		if (height >= 1080) {
			bitrate = highFps ? 12000000 : 8000000;
		} else if (height >= 720) {
			bitrate = highFps ? 7500000 : 5000000;
		} else {
			bitrate = highFps ? 4000000 : 2500000;
		}
		crf = "19";
		cq = nullptr;
		capped = true;
		profile = "high";
		gop = std::max((int)std::lround(fps / 2.0), 1);
	}

	for (const char *name : names) {
		const AVCodec *codec = avcodec_find_encoder_by_name(name);
		if (!codec) {
			continue;
		}
		CodecPtr enc(avcodec_alloc_context3(codec));
		if (!enc) {
			continue;
		}
		AVPixelFormat format = PickPixelFormat(enc.get(), codec);
		if (format == AV_PIX_FMT_NONE) {
			continue;
		}

		enc->width = width;
		enc->height = height;
		enc->sample_aspect_ratio = in->sample_aspect_ratio;
		enc->pix_fmt = format;
		enc->framerate = frameRate;
		enc->time_base = av_inv_q(frameRate);
		enc->gop_size = gop;
		enc->max_b_frames = 0;
		enc->color_range = in->color_range;
		enc->color_primaries = in->color_primaries;
		enc->color_trc = in->color_trc;
		enc->colorspace = in->color_space;
		enc->thread_count = 0;
		if (globalHeader) {
			enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

		AVDictionary *opts = nullptr;
		const bool isX264 = strcmp(name, "libx264") == 0;
		const bool isNvenc = strcmp(name, "h264_nvenc") == 0;
		if (profile && (isX264 || isNvenc || strcmp(name, "h264_amf") == 0 || strcmp(name, "h264_qsv") == 0)) {
			av_dict_set(&opts, "profile", profile, 0);
		}
		if (isX264) {
			av_dict_set(&opts, "preset", "veryfast", 0);
			av_dict_set(&opts, "crf", crf, 0);
			if (capped) {
				enc->rc_max_rate = bitrate;
				enc->rc_buffer_size = (int)std::min<int64_t>(bitrate * 2, INT32_MAX);
			}
		} else {
			enc->bit_rate = bitrate;
			enc->rc_max_rate = capped ? bitrate * 3 / 2 : bitrate * 2;
			enc->rc_buffer_size = (int)std::min<int64_t>(bitrate * 2, INT32_MAX);
			if (isNvenc) {
				av_dict_set(&opts, "preset", "p5", 0);
				av_dict_set(&opts, "rc", "vbr", 0);
				if (cq) {
					av_dict_set(&opts, "cq", cq, 0);
				}
			} else if (strcmp(name, "h264_amf") == 0) {
				av_dict_set(&opts, "quality", "quality", 0);
				av_dict_set(&opts, "rc", "vbr_peak", 0);
			} else if (strcmp(name, "mpeg4") == 0) {
				enc->flags |= AV_CODEC_FLAG_QSCALE;
				enc->global_quality = FF_QP2LAMBDA * mpeg4Qscale;
			}
		}

		int ret = avcodec_open2(enc.get(), codec, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			blog(LOG_INFO, "[ClipRender] Encoder '%s' unavailable: %s", name, AvError(ret).c_str());
			continue;
		}
		static const char *qualityNames[] = {"high", "medium", "small", "youtube"};
		blog(LOG_INFO, "[ClipRender] Encoding with '%s' (%s quality, %dx%d, %lld kbps target)", name,
		     qualityNames[(int)quality], width, height, (long long)(bitrate / 1000));
		return enc;
	}

	error = "No usable H.264 encoder found";
	return nullptr;
}

AVRational StreamFrameRate(const AVStream *st)
{
	AVRational rate = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
	if (!rate.num || !rate.den || av_q2d(rate) > 240.0 || av_q2d(rate) < 1.0) {
		return {60, 1};
	}
	return av_d2q(av_q2d(rate), 1001000);
}

} // namespace

bool Export(const std::vector<std::string> &inputs, double startSec, double endSec, const std::string &output,
	    const std::vector<Layer> &layers, std::string &error, const ClipExport::ProgressCallback &progress,
	    Quality quality)
{
	if (inputs.empty()) {
		error = "Nothing to export";
		return false;
	}

	std::vector<double> offsets;
	double total = 0.0;
	for (const std::string &path : inputs) {
		double d = ClipExport::ProbeDuration(path);
		if (d < 0.0) {
			error = "Could not read duration of '" + path + "'";
			return false;
		}
		offsets.push_back(total);
		total += d;
	}

	startSec = std::max(0.0, startSec);
	if (endSec < 0.0 || endSec > total) {
		endSec = total;
	}
	if (endSec <= startSec) {
		error = "Empty time range";
		return false;
	}

	const double eps = 0.005;
	size_t first = 0;
	while (first + 1 < inputs.size() && offsets[first + 1] <= startSec + eps) {
		first++;
	}

	InputPtr in = OpenInput(inputs[first], error);
	if (!in) {
		return false;
	}

	int videoIndex = -1;
	for (unsigned i = 0; i < in->nb_streams; i++) {
		if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoIndex = (int)i;
			break;
		}
	}
	if (videoIndex < 0) {
		error = "The recording has no video";
		return false;
	}

	AVFormatContext *ocRaw = nullptr;
	int ret = avformat_alloc_output_context2(&ocRaw, nullptr, nullptr, output.c_str());
	if (ret < 0 || !ocRaw) {
		error = "Unsupported output format for '" + output + "'";
		return false;
	}
	OutputPtr oc(ocRaw);

	const unsigned streamCount = in->nb_streams;
	const AVStream *vst = in->streams[videoIndex];
	const AVRational frameRate = StreamFrameRate(vst);

	CodecPtr enc =
		OpenEncoder(vst->codecpar, frameRate, (oc->oformat->flags & AVFMT_GLOBALHEADER) != 0, quality, error);
	if (!enc) {
		return false;
	}

	std::vector<int> streamMap(streamCount, -1);
	AVStream *vost = nullptr;
	for (unsigned i = 0; i < streamCount; i++) {
		AVStream *ist = in->streams[i];
		if ((int)i == videoIndex) {
			vost = avformat_new_stream(oc.get(), nullptr);
			if (!vost) {
				error = "Failed to allocate output stream";
				return false;
			}
			avcodec_parameters_from_context(vost->codecpar, enc.get());
			vost->time_base = enc->time_base;
			vost->avg_frame_rate = frameRate;
			streamMap[i] = vost->index;
			continue;
		}
		if (ist->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
			continue;
		}
		if (avformat_query_codec(oc->oformat, ist->codecpar->codec_id, FF_COMPLIANCE_NORMAL) != 1) {
			error = std::string("Codec '") + avcodec_get_name(ist->codecpar->codec_id) +
				"' is not supported by the output container";
			return false;
		}
		AVStream *ost = avformat_new_stream(oc.get(), nullptr);
		if (!ost) {
			error = "Failed to allocate output stream";
			return false;
		}
		avcodec_parameters_copy(ost->codecpar, ist->codecpar);
		ost->codecpar->codec_tag = 0;
		ost->time_base = ist->time_base;
		av_dict_copy(&ost->metadata, ist->metadata, 0);
		ost->disposition = ist->disposition;
		streamMap[i] = ost->index;
	}

	if (!(oc->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&oc->pb, output.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			error = "Could not create '" + output + "': " + AvError(ret);
			return false;
		}
	}

	AVDictionary *opts = nullptr;
	if (strcmp(oc->oformat->name, "mp4") == 0 || strcmp(oc->oformat->name, "mov") == 0) {
		av_dict_set(&opts, "movflags", "+faststart", 0);
	}
	ret = avformat_write_header(oc.get(), &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		error = "Could not write header: " + AvError(ret);
		oc.reset();
		os_unlink(output.c_str());
		return false;
	}

	bool failed = false;
	bool cancelled = false;
	bool videoDone = false;
	bool audioDone = true;
	for (unsigned i = 0; i < streamCount; i++) {
		if (streamMap[i] >= 0 && (int)i != videoIndex) {
			audioDone = false;
		}
	}

	std::vector<int64_t> lastDts(oc->nb_streams, AV_NOPTS_VALUE);
	int64_t lastVideoPts = AV_NOPTS_VALUE;
	const double frameDuration = av_q2d(enc->time_base);

	FramePtr work(av_frame_alloc());
	work->format = AV_PIX_FMT_YUV420P;
	work->width = enc->width;
	work->height = enc->height;
	if (av_frame_get_buffer(work.get(), 0) < 0) {
		error = "Out of memory";
		return false;
	}
	FramePtr encFrame;
	if (enc->pix_fmt != AV_PIX_FMT_YUV420P) {
		encFrame.reset(av_frame_alloc());
		encFrame->format = enc->pix_fmt;
		encFrame->width = enc->width;
		encFrame->height = enc->height;
		if (av_frame_get_buffer(encFrame.get(), 0) < 0) {
			error = "Out of memory";
			return false;
		}
	}
	SwsPtr toWork, toEnc;
	PacketPtr outPkt(av_packet_alloc());

	auto writeEncoded = [&]() {
		while (!failed) {
			int r = avcodec_receive_packet(enc.get(), outPkt.get());
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
				break;
			}
			if (r < 0) {
				error = "Encoding failed: " + AvError(r);
				failed = true;
				break;
			}
			av_packet_rescale_ts(outPkt.get(), enc->time_base, vost->time_base);
			outPkt->stream_index = vost->index;
			r = av_interleaved_write_frame(oc.get(), outPkt.get());
			if (r < 0) {
				error = "Failed to write packet: " + AvError(r);
				failed = true;
			}
		}
	};

	/* Censors, retimes and encodes one decoded frame from input `f`. */
	auto encodeFrame = [&](AVFrame *frame, size_t f, AVRational tb) {
		int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
		if (ts == AV_NOPTS_VALUE) {
			return;
		}
		double t = offsets[f] + ts * av_q2d(tb);
		if (t < startSec - frameDuration / 2.0) {
			return;
		}
		if (t >= endSec - frameDuration / 2.0) {
			videoDone = true;
			return;
		}

		int64_t pts = llround((t - startSec) / frameDuration);
		if (lastVideoPts != AV_NOPTS_VALUE && pts <= lastVideoPts) {
			return; /* two frames in one output frame slot */
		}
		lastVideoPts = pts;

		if (av_frame_make_writable(work.get()) < 0) {
			error = "Out of memory";
			failed = true;
			return;
		}
		toWork.reset(sws_getCachedContext(toWork.release(), frame->width, frame->height,
						  (AVPixelFormat)frame->format, work->width, work->height,
						  AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
		if (!toWork) {
			error = "Unsupported video format";
			failed = true;
			return;
		}
		/* Keep the range as is; the encoder is told the input's range */
		const int *coefs =
			sws_getCoefficients(frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
		const int full = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
		sws_setColorspaceDetails(toWork.get(), coefs, full, coefs, full, 0, 1 << 16, 1 << 16);
		sws_scale(toWork.get(), frame->data, frame->linesize, 0, frame->height, work->data, work->linesize);

		Planes planes;
		for (int i = 0; i < 3; i++) {
			planes.data[i] = work->data[i];
			planes.linesize[i] = work->linesize[i];
		}
		planes.width = work->width;
		planes.height = work->height;
		planes.fullRange = full != 0;
		planes.bt709 = frame->colorspace != AVCOL_SPC_BT470BG && frame->colorspace != AVCOL_SPC_SMPTE170M &&
			       (frame->colorspace == AVCOL_SPC_BT709 || frame->height >= 720);
		Apply(planes, layers, t);

		AVFrame *send = work.get();
		if (encFrame) {
			if (av_frame_make_writable(encFrame.get()) < 0) {
				error = "Out of memory";
				failed = true;
				return;
			}
			toEnc.reset(sws_getCachedContext(toEnc.release(), work->width, work->height, AV_PIX_FMT_YUV420P,
							 encFrame->width, encFrame->height,
							 (AVPixelFormat)encFrame->format, SWS_POINT, nullptr, nullptr,
							 nullptr));
			if (!toEnc) {
				error = "Unsupported encoder format";
				failed = true;
				return;
			}
			sws_scale(toEnc.get(), work->data, work->linesize, 0, work->height, encFrame->data,
				  encFrame->linesize);
			send = encFrame.get();
		}
		send->pts = pts;
		send->pict_type = AV_PICTURE_TYPE_NONE;
		send->color_range = enc->color_range;
		send->colorspace = enc->colorspace;
		send->color_primaries = enc->color_primaries;
		send->color_trc = enc->color_trc;

		int r = avcodec_send_frame(enc.get(), send);
		if (r < 0) {
			error = "Encoding failed: " + AvError(r);
			failed = true;
			return;
		}
		writeEncoded();

		if (progress && !progress((float)((t - startSec) / (endSec - startSec)))) {
			cancelled = true;
		}
	};

	auto drainDecoder = [&](AVCodecContext *dec, AVFrame *frame, size_t f, AVRational tb) {
		while (!failed && !cancelled) {
			int r = avcodec_receive_frame(dec, frame);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
				break;
			}
			if (r < 0) {
				/* A damaged frame; keep going with the next one */
				blog(LOG_WARNING, "[ClipRender] Decoding error: %s", AvError(r).c_str());
				break;
			}
			if (!videoDone) {
				encodeFrame(frame, f, tb);
			}
			av_frame_unref(frame);
		}
	};

	PacketPtr pkt(av_packet_alloc());
	FramePtr decoded(av_frame_alloc());

	for (size_t f = first; f < inputs.size() && !failed && !cancelled && !(videoDone && audioDone); f++) {
		if (offsets[f] >= endSec) {
			break;
		}
		if (f != first) {
			in = OpenInput(inputs[f], error);
			if (!in) {
				failed = true;
				break;
			}
			if (in->nb_streams != streamCount ||
			    in->streams[videoIndex]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
				error = "Segment '" + inputs[f] + "' has a different stream layout";
				failed = true;
				break;
			}
		} else {
			double localStart = startSec - offsets[f];
			av_seek_frame(in.get(), -1, (int64_t)(localStart * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
		}

		/* A new decoder per segment: each segment starts with its own
		 * headers and a keyframe, and resolutions may differ. */
		const AVRational vtb = in->streams[videoIndex]->time_base;
		CodecPtr dec = OpenDecoder(in->streams[videoIndex], error);
		if (!dec) {
			failed = true;
			break;
		}

		while (!failed && !cancelled && !(videoDone && audioDone) && av_read_frame(in.get(), pkt.get()) >= 0) {
			int si = pkt->stream_index;
			if (si == videoIndex) {
				if (!videoDone) {
					int r = avcodec_send_packet(dec.get(), pkt.get());
					if (r < 0 && r != AVERROR(EAGAIN)) {
						blog(LOG_WARNING, "[ClipRender] Decoding error: %s",
						     AvError(r).c_str());
					}
					drainDecoder(dec.get(), decoded.get(), f, vtb);
				}
				av_packet_unref(pkt.get());
				continue;
			}

			int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (si < 0 || (unsigned)si >= streamCount || streamMap[si] < 0 || ts == AV_NOPTS_VALUE ||
			    audioDone) {
				av_packet_unref(pkt.get());
				continue;
			}

			AVRational tb = in->streams[si]->time_base;
			double t = offsets[f] + ts * av_q2d(tb);
			/* Audio past this segment's video comes from the next one */
			bool overhang = f + 1 < inputs.size() && t >= offsets[f + 1];
			if (t >= endSec) {
				audioDone = true;
			}
			if (t < startSec || t >= endSec || overhang) {
				av_packet_unref(pkt.get());
				continue;
			}

			AVStream *ost = oc->streams[streamMap[si]];
			int64_t shift = llround((offsets[f] - startSec) / av_q2d(tb));
			if (pkt->pts != AV_NOPTS_VALUE) {
				pkt->pts += shift;
			}
			pkt->dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts + shift : pkt->pts;
			av_packet_rescale_ts(pkt.get(), tb, ost->time_base);

			int64_t &last = lastDts[ost->index];
			if (last != AV_NOPTS_VALUE && pkt->dts <= last) {
				pkt->dts = last + 1;
			}
			if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
				pkt->pts = pkt->dts;
			}
			last = pkt->dts;

			pkt->stream_index = ost->index;
			pkt->pos = -1;
			int r = av_interleaved_write_frame(oc.get(), pkt.get());
			av_packet_unref(pkt.get());
			if (r < 0) {
				error = "Failed to write packet: " + AvError(r);
				failed = true;
			}
		}
		av_packet_unref(pkt.get());

		if (!failed && !cancelled && !videoDone) {
			avcodec_send_packet(dec.get(), nullptr);
			drainDecoder(dec.get(), decoded.get(), f, vtb);
		}
	}

	if (!failed && !cancelled) {
		if (lastVideoPts == AV_NOPTS_VALUE) {
			error = "No video frames in the requested range";
			failed = true;
		} else {
			avcodec_send_frame(enc.get(), nullptr);
			writeEncoded();
		}
	}

	if (!failed && !cancelled) {
		ret = av_write_trailer(oc.get());
		if (ret < 0) {
			error = "Failed to finalize output: " + AvError(ret);
			failed = true;
		}
	}

	oc.reset();

	if (failed || cancelled) {
		if (cancelled) {
			error = "Cancelled";
		}
		os_unlink(output.c_str());
		return false;
	}

	if (progress) {
		progress(1.0f);
	}
	return true;
}

} // namespace ClipRender
