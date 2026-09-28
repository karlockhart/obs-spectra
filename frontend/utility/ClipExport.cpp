#include "ClipExport.hpp"

#include <util/base.h>
#include <util/platform.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>

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

struct PacketDeleter {
	void operator()(AVPacket *pkt) const { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

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

int FindVideoStream(AVFormatContext *ctx)
{
	for (unsigned i = 0; i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			return (int)i;
		}
	}
	return -1;
}

int64_t FrameDuration(const AVStream *st)
{
	AVRational rate = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
	if (!rate.num || !rate.den) {
		return 1;
	}
	return std::max<int64_t>(av_rescale_q(1, av_inv_q(rate), st->time_base), 1);
}

/* End of the last packet of `streamIndex` (or of any stream if negative),
 * reading from the current position to EOF. */
double ScanEnd(AVFormatContext *ctx, int streamIndex)
{
	double end = -1.0;
	PacketPtr pkt(av_packet_alloc());
	while (av_read_frame(ctx, pkt.get()) >= 0) {
		if (streamIndex < 0 || pkt->stream_index == streamIndex) {
			AVStream *st = ctx->streams[pkt->stream_index];
			int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (ts != AV_NOPTS_VALUE) {
				int64_t dur = pkt->duration > 0 ? pkt->duration
								: (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
									   ? FrameDuration(st)
									   : 0);
				end = std::max(end, (ts + dur) * av_q2d(st->time_base));
			}
		}
		av_packet_unref(pkt.get());
	}
	return end;
}

/*
 * Length of a segment on the joined timeline. For files with video this is
 * where the video ends (audio may overhang by a few frames), found cheaply by
 * seeking to the last keyframe and reading to EOF.
 */
double DurationOf(AVFormatContext *ctx)
{
	double container = (ctx->duration != AV_NOPTS_VALUE && ctx->duration > 0) ? (double)ctx->duration / AV_TIME_BASE
										  : -1.0;

	int video = FindVideoStream(ctx);
	if (video < 0) {
		if (container > 0.0) {
			return container;
		}
		return ScanEnd(ctx, -1);
	}

	if (container > 0.0) {
		AVStream *st = ctx->streams[video];
		int64_t target = (int64_t)(container / av_q2d(st->time_base));
		if (av_seek_frame(ctx, video, target, AVSEEK_FLAG_BACKWARD) >= 0) {
			double end = ScanEnd(ctx, video);
			if (end > 0.0) {
				return end;
			}
		}
		av_seek_frame(ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
	}

	/* No index or duration (e.g. an unfinalized file): scan everything. */
	return ScanEnd(ctx, video);
}

/*
 * Matroska stores presentation timestamps only, so video packets with
 * B-frames come back without a DTS. Rebuild it: the DTS sequence is the
 * sorted PTS sequence delayed by the reorder depth.
 */
struct DtsGenerator {
	int delay = 0;
	int64_t frameDuration = 1;
	int64_t count = 0;
	int64_t firstPts = AV_NOPTS_VALUE;
	std::priority_queue<int64_t, std::vector<int64_t>, std::greater<int64_t>> pending;

	int64_t Next(int64_t pts)
	{
		if (firstPts == AV_NOPTS_VALUE) {
			firstPts = pts;
		}
		pending.push(pts);
		int64_t dts;
		if (count < delay) {
			dts = firstPts - (delay - count) * frameDuration;
		} else {
			dts = pending.top();
			pending.pop();
		}
		count++;
		return dts;
	}
};

bool IsCopiedStream(const AVStream *st)
{
	return st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
}

} // namespace

namespace ClipExport {

double ProbeDuration(const std::string &path)
{
	std::string error;
	InputPtr input = OpenInput(path, error);
	if (!input) {
		blog(LOG_WARNING, "[ClipExport] %s", error.c_str());
		return -1.0;
	}
	return DurationOf(input.get());
}

bool Export(const std::vector<std::string> &inputs, double startSec, double endSec, const std::string &output,
	    std::string &error, const ProgressCallback &progress)
{
	if (inputs.empty()) {
		error = "Nothing to export";
		return false;
	}

	/* Place every input on one joined timeline. */
	std::vector<double> offsets;
	double total = 0.0;
	for (const std::string &path : inputs) {
		double d = ProbeDuration(path);
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

	size_t first = 0;
	/* Tolerance for timestamp rounding at segment joins */
	const double eps = 0.005;
	while (first + 1 < inputs.size() && offsets[first + 1] <= startSec + eps) {
		first++;
	}

	InputPtr in = OpenInput(inputs[first], error);
	if (!in) {
		return false;
	}

	AVFormatContext *oc_raw = nullptr;
	int ret = avformat_alloc_output_context2(&oc_raw, nullptr, nullptr, output.c_str());
	if (ret < 0 || !oc_raw) {
		error = "Unsupported output format for '" + output + "'";
		return false;
	}
	OutputPtr oc(oc_raw);
	oc->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;

	const unsigned streamCount = in->nb_streams;
	std::vector<int> streamMap(streamCount, -1);
	int videoIndex = -1;
	/* Only the first audio track, the full mix: loop recordings can also
	 * have one track per speaker, for transcripts */
	bool audioCopied = false;

	for (unsigned i = 0; i < streamCount; i++) {
		AVStream *ist = in->streams[i];
		if (!IsCopiedStream(ist)) {
			continue;
		}
		if (ist->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (audioCopied) {
				continue;
			}
			audioCopied = true;
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
		if (videoIndex < 0 && ist->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoIndex = (int)i;
		}
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

	/* Seek near the cut. Container indexes are often coarse (Matroska cues
	 * mark cluster starts), so this can land well before the cut; the exact
	 * keyframe is chosen while reading below. */
	double localStart = startSec - offsets[first];
	av_seek_frame(in.get(), -1, (int64_t)(localStart * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

	std::vector<int64_t> lastDts(oc->nb_streams, AV_NOPTS_VALUE);
	std::vector<DtsGenerator> dtsGen(oc->nb_streams);
	if (videoIndex >= 0) {
		AVStream *vst = in->streams[videoIndex];
		DtsGenerator &gen = dtsGen[streamMap[videoIndex]];
		/* A larger delay than needed is harmless; too small breaks ordering. */
		gen.delay = std::max(vst->codecpar->video_delay, 4);
		gen.frameDuration = FrameDuration(vst);
	}

	/* Joined-timeline time that becomes t=0 in the output: the last video
	 * keyframe at or before the cut (or the cut itself for audio-only). */
	double base = videoIndex < 0 ? startSec : -1.0;
	bool cancelled = false;
	bool failed = false;

	enum class Result { Written, Skipped, End, Stop };

	/* Filters, retimes and writes one packet read from input `f`. */
	auto writePacket = [&](AVPacket *p, size_t f, double t, AVRational inTb) -> Result {
		int si = p->stream_index;
		if (t < base) {
			return Result::Skipped;
		}
		/* Audio overhanging the end of this segment's video overlaps the
		 * next segment; the next segment carries that audio instead. */
		if (si != videoIndex && f + 1 < inputs.size() && t >= offsets[f + 1]) {
			return Result::Skipped;
		}
		if (t >= endSec) {
			/* Stopping at the first video packet past the end (in decode
			 * order) keeps every written frame decodable. */
			return (si == videoIndex || videoIndex < 0) ? Result::End : Result::Skipped;
		}

		AVStream *ost = oc->streams[streamMap[si]];
		int64_t shift = llround((offsets[f] - base) / av_q2d(inTb));
		if (p->pts != AV_NOPTS_VALUE) {
			p->pts += shift;
		}
		if (p->dts != AV_NOPTS_VALUE) {
			p->dts += shift;
		} else if (si == videoIndex && p->pts != AV_NOPTS_VALUE) {
			p->dts = dtsGen[streamMap[si]].Next(p->pts);
		} else {
			p->dts = p->pts;
		}
		av_packet_rescale_ts(p, inTb, ost->time_base);

		int64_t &last = lastDts[ost->index];
		if (p->dts != AV_NOPTS_VALUE) {
			if (last != AV_NOPTS_VALUE && p->dts <= last) {
				p->dts = last + 1;
			}
			if (p->pts != AV_NOPTS_VALUE && p->pts < p->dts) {
				p->pts = p->dts;
			}
			last = p->dts;
		}

		p->stream_index = ost->index;
		p->pos = -1;
		int err = av_interleaved_write_frame(oc.get(), p);
		if (err < 0) {
			error = "Failed to write packet: " + AvError(err);
			failed = true;
			return Result::Stop;
		}

		if (progress && !progress((float)((t - base) / (endSec - base)))) {
			cancelled = true;
			return Result::Stop;
		}
		return Result::Written;
	};

	/* Packets from the current keyframe candidate onward, held until it is
	 * known to be the last keyframe at or before the cut. */
	struct Held {
		PacketPtr pkt;
		size_t f;
		double t;
		AVRational tb;
	};
	std::vector<Held> held;
	double candidate = -1.0;

	bool reachedEnd = false;
	auto flushHeld = [&]() {
		base = candidate;
		for (Held &h : held) {
			if (reachedEnd || failed || cancelled) {
				break;
			}
			Result r = writePacket(h.pkt.get(), h.f, h.t, h.tb);
			if (r == Result::End) {
				reachedEnd = true;
			}
		}
		held.clear();
	};

	PacketPtr pkt(av_packet_alloc());

	for (size_t f = first; f < inputs.size() && !reachedEnd && !cancelled && !failed; f++) {
		if (offsets[f] >= endSec) {
			break;
		}
		if (f != first) {
			in = OpenInput(inputs[f], error);
			if (!in) {
				failed = true;
				break;
			}
			if (in->nb_streams != streamCount) {
				error = "Segment '" + inputs[f] + "' has a different stream layout";
				failed = true;
				break;
			}
		}

		while (!reachedEnd && !failed && !cancelled && av_read_frame(in.get(), pkt.get()) >= 0) {
			int si = pkt->stream_index;
			int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (si < 0 || (unsigned)si >= streamCount || streamMap[si] < 0 || ts == AV_NOPTS_VALUE) {
				av_packet_unref(pkt.get());
				continue;
			}

			AVRational tb = in->streams[si]->time_base;
			double t = offsets[f] + ts * av_q2d(tb);

			if (base < 0.0) {
				bool isVideo = si == videoIndex;
				bool isKey = isVideo && (pkt->flags & AV_PKT_FLAG_KEY);

				if (isKey && (t <= startSec + eps || candidate < 0.0)) {
					held.clear();
					candidate = t;
				}
				if (candidate >= 0.0) {
					held.push_back({PacketPtr(av_packet_clone(pkt.get())), f, t, tb});
				}
				av_packet_unref(pkt.get());

				/* Once a video frame at or past the cut shows up, no later
				 * keyframe can be at or before it. */
				if (candidate >= 0.0 && isVideo && (t > startSec + eps || candidate > startSec)) {
					flushHeld();
				}
				continue;
			}

			Result r = writePacket(pkt.get(), f, t, tb);
			av_packet_unref(pkt.get());
			if (r == Result::End) {
				reachedEnd = true;
			}
		}
		av_packet_unref(pkt.get());
	}

	if (base < 0.0 && candidate >= 0.0 && !failed && !cancelled) {
		flushHeld();
	}

	if (!failed && !cancelled && base < 0.0) {
		error = "No keyframe found in the requested range";
		failed = true;
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

} // namespace ClipExport
