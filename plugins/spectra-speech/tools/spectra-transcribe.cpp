/* spectra-transcribe <model.bin> <media> [--track N] [--start S] [--duration S]
 *                    [--language xx] [--vad vad.bin] [--cpu]
 * Transcribes one audio track of a recording and prints each segment with
 * its time, to try models and settings on real loop recordings. */

#include <spectra-speech/audio.hpp>
#include <spectra-speech/speech.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>

#include <cstdio>

using namespace spectra::speech;

static QString Time(double seconds)
{
	const int total = (int)seconds;
	return QStringLiteral("%1:%2.%3")
		.arg(total / 60, 2, 10, QLatin1Char('0'))
		.arg(total % 60, 2, 10, QLatin1Char('0'))
		.arg((int)((seconds - total) * 100), 2, 10, QLatin1Char('0'));
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QStringList args = app.arguments().mid(1);
	if (args.size() < 2) {
		std::fprintf(stderr, "usage: spectra-transcribe <model.bin> <media> [--track N] [--start S] "
				     "[--duration S] [--language xx] [--vad vad.bin] [--cpu]\n");
		return 2;
	}

	const QString modelPath = args.takeFirst();
	const QString media = args.takeFirst();
	int track = 0;
	double start = 0.0, duration = -1.0;
	bool gpu = true;
	Options options;
	for (int i = 0; i < args.size(); i++) {
		const QString &arg = args[i];
		const QString next = i + 1 < args.size() ? args[i + 1] : QString();
		if (arg == "--track") {
			track = next.toInt() - 1;
			i++;
		} else if (arg == "--start") {
			start = next.toDouble();
			i++;
		} else if (arg == "--duration") {
			duration = next.toDouble();
			i++;
		} else if (arg == "--language") {
			options.language = next;
			i++;
		} else if (arg == "--vad") {
			options.vadModelPath = next;
			i++;
		} else if (arg == "--cpu") {
			gpu = false;
		}
	}

	std::printf("%s\n", qPrintable(SystemInfo()));
	std::printf("%s has %d audio track(s)\n", qPrintable(media), AudioTrackCount(media));

	QString error;
	QElapsedTimer timer;
	timer.start();
	std::vector<float> samples;
	if (!ReadAudio(media, track, start, duration, samples, &error)) {
		std::fprintf(stderr, "%s\n", qPrintable(error));
		return 1;
	}
	const double audioSeconds = (double)samples.size() / kSampleRate;
	std::printf("decoded %.1f s of audio in %lld ms\n", audioSeconds, timer.restart());

	auto model = Model::Load(modelPath, gpu, &error);
	if (!model) {
		std::fprintf(stderr, "%s\n", qPrintable(error));
		return 1;
	}
	std::printf("loaded %s in %lld ms\n", qPrintable(modelPath), timer.restart());

	std::vector<Segment> segments;
	if (!model->Transcribe(samples, options, segments, {}, &error)) {
		std::fprintf(stderr, "%s\n", qPrintable(error));
		return 1;
	}
	const qint64 ms = timer.elapsed();
	std::printf("transcribed in %lld ms (%.1fx realtime), language %s\n\n", ms,
		    ms > 0 ? audioSeconds * 1000.0 / ms : 0.0, qPrintable(model->LastLanguage()));

	for (const Segment &s : segments) {
		std::printf("[%s - %s] %.2f/%.2f%s %s\n", qPrintable(Time(start + s.start)),
			    qPrintable(Time(start + s.end)), s.confidence, s.noSpeech,
			    IsLikelyHallucination(s) ? " (dropped)" : "", qPrintable(s.text));
	}
	return 0;
}
