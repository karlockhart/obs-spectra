#pragma once

#include "ClipRender.hpp"

#include <util/config-file.h>

#include <QImage>
#include <QString>

#include <functional>
#include <vector>

/*
 * Captions for the Spectra clip maker: Whisper transcribes the clip's range
 * of the loop recording, the captions can be edited, and they are burned into
 * the exported video and/or saved next to it as subtitles (.srt).
 */
namespace ClipCaptions {

struct Cue {
	/* On the clip maker's joined timeline, end exclusive */
	double start = 0.0, end = 0.0;
	QString text;
	/* "me", "teamspeak", "game", or empty (the full mix) */
	QString speaker;
};

/* A stretch of one recording file: seconds `from`..`to` into `path`, which
 * start at `at` on the joined timeline */
struct Source {
	QString path;
	double from = 0.0, to = 0.0;
	double at = 0.0;
};

/* The speech model and options, shared with Lucida's Speech settings */
struct Settings {
	QString modelPath;
	QString vadModelPath;
	QString language;
	QString prompt;
	bool useGpu = true;
};

/* Lucida's speech settings from the profile; false (with error) when the
 * model hasn't been downloaded */
bool LoadSettings(config_t *profile, Settings &settings, QString &error);

/* Transcribes the sources, each speaker track on its own when a recording
 * has them. progress: 0..1, return false to cancel. */
bool Transcribe(const std::vector<Source> &sources, const Settings &settings, std::vector<Cue> &cues,
		const std::function<bool(float)> &progress, QString &error);

/* "Me", "TeamSpeak", "Game"; empty for the mix */
QString SpeakerName(const QString &speaker);
/* The text shown for a cue, optionally with who is speaking */
QString Display(const Cue &cue, bool showSpeaker);

/* The caption drawn over a transparent frame of the video's size: white text
 * with a dark outline, centred near the bottom, wrapped */
QImage Render(const QString &text, int width, int height);
ClipRender::Overlay ToOverlay(const QImage &image, double start, double end);

/* SubRip subtitles for the cues between offset and offset + length, timed
 * from the clip's start */
bool WriteSrt(const QString &path, const std::vector<Cue> &cues, double offset, double length, bool showSpeaker,
	      QString &error);

} // namespace ClipCaptions
