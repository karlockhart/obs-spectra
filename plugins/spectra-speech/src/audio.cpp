#include <spectra-speech/audio.hpp>
#include <spectra-speech/speech.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <vector>

namespace spectra::speech {

namespace {

QString AvError(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return QString::fromUtf8(buffer);
}

struct Input {
	AVFormatContext *format = nullptr;
	~Input() { avformat_close_input(&format); }

	bool Open(const QString &path, QString *error)
	{
		int ret = avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr);
		if (ret >= 0) {
			ret = avformat_find_stream_info(format, nullptr);
		}
		if (ret < 0) {
			if (error) {
				*error = QStringLiteral("Could not open %1: %2").arg(path, AvError(ret));
			}
			return false;
		}
		return true;
	}

	std::vector<int> AudioStreams() const
	{
		std::vector<int> streams;
		for (unsigned i = 0; i < format->nb_streams; i++) {
			if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				streams.push_back((int)i);
			}
		}
		return streams;
	}
};

} // namespace

int AudioTrackCount(const QString &path, QString *error)
{
	Input input;
	if (!input.Open(path, error)) {
		return -1;
	}
	return (int)input.AudioStreams().size();
}

bool ReadAudio(const QString &path, int track, double start, double duration, std::vector<float> &samples,
	       QString *error)
{
	samples.clear();
	auto fail = [&](const QString &message) {
		if (error) {
			*error = message;
		}
		return false;
	};

	Input input;
	if (!input.Open(path, error)) {
		return false;
	}
	const std::vector<int> streams = input.AudioStreams();
	if (track < 0 || track >= (int)streams.size()) {
		return fail(QStringLiteral("%1 has no audio track %2").arg(path).arg(track + 1));
	}
	AVStream *stream = input.format->streams[streams[track]];

	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
	if (!decoder || avcodec_parameters_to_context(decoder, stream->codecpar) < 0 ||
	    avcodec_open2(decoder, codec, nullptr) < 0) {
		avcodec_free_context(&decoder);
		return fail(QStringLiteral("No decoder for the audio in %1").arg(path));
	}

	SwrContext *resampler = nullptr;
	AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
	if (swr_alloc_set_opts2(&resampler, &mono, AV_SAMPLE_FMT_FLT, kSampleRate, &decoder->ch_layout,
				decoder->sample_fmt, decoder->sample_rate, 0, nullptr) < 0 ||
	    swr_init(resampler) < 0) {
		swr_free(&resampler);
		avcodec_free_context(&decoder);
		return fail(QStringLiteral("Could not convert the audio in %1").arg(path));
	}

	start = std::max(start, 0.0);
	if (start > 0.0) {
		const int64_t ts = (int64_t)(start / av_q2d(stream->time_base));
		av_seek_frame(input.format, stream->index, ts, AVSEEK_FLAG_BACKWARD);
	}
	const double streamStart = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * av_q2d(stream->time_base)
									: 0.0;
	const size_t wanted = duration >= 0.0 ? (size_t)std::llround(duration * kSampleRate) : SIZE_MAX;

	/* Samples before `start` (from seeking back to a keyframe) are dropped,
	 * gaps between frames become silence, so index / kSampleRate is always
	 * the time since `start` */
	bool placed = false;
	std::vector<float> converted;
	auto append = [&](AVFrame *frame) {
		const int capacity = swr_get_out_samples(resampler, frame ? frame->nb_samples : 0);
		if (capacity <= 0) {
			return;
		}
		converted.resize((size_t)capacity);
		uint8_t *out = reinterpret_cast<uint8_t *>(converted.data());
		const int got = swr_convert(resampler, &out, capacity,
					    frame ? const_cast<const uint8_t **>(frame->extended_data) : nullptr,
					    frame ? frame->nb_samples : 0);
		if (got <= 0) {
			return;
		}

		size_t skip = 0;
		if (frame && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
			const double t = frame->best_effort_timestamp * av_q2d(stream->time_base) - streamStart;
			const double at = (t - start) * kSampleRate;
			if (!placed) {
				placed = true;
				if (at < 0) {
					skip = (size_t)std::min<double>(-at, got);
				} else {
					samples.resize((size_t)at, 0.0f);
				}
			} else if (at - (double)samples.size() > kSampleRate / 10) {
				samples.resize((size_t)at, 0.0f);
			}
		}
		for (int i = (int)skip; i < got && samples.size() < wanted; i++) {
			samples.push_back(converted[i]);
		}
	};

	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	auto drain = [&]() {
		while (avcodec_receive_frame(decoder, frame) >= 0) {
			append(frame);
			av_frame_unref(frame);
		}
	};
	while (samples.size() < wanted && av_read_frame(input.format, packet) >= 0) {
		if (packet->stream_index == stream->index && avcodec_send_packet(decoder, packet) >= 0) {
			drain();
		}
		av_packet_unref(packet);
	}
	avcodec_send_packet(decoder, nullptr);
	drain();
	if (samples.size() < wanted) {
		append(nullptr);
	}

	av_frame_free(&frame);
	av_packet_free(&packet);
	swr_free(&resampler);
	avcodec_free_context(&decoder);
	return true;
}

} // namespace spectra::speech
