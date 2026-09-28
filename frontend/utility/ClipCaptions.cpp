#include "ClipCaptions.hpp"

#include <spectra-speech/audio.hpp>
#include <spectra-speech/models.hpp>
#include <spectra-speech/speech.hpp>

#include <OBSApp.hpp>

#include <QFile>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <thread>

namespace ClipCaptions {

namespace {

/* Loop tracks with speaker tracks on: 1 mix, 2 me, 3 TeamSpeak, 4 game */
constexpr int kSpeakerTracks = 4;

QString ConfigString(config_t *config, const char *name)
{
	const char *value = config_get_string(config, "Lucida", name);
	return QString::fromUtf8(value ? value : "");
}

bool ConfigBool(config_t *config, const char *name, bool fallback)
{
	return config_has_user_value(config, "Lucida", name) || config_has_default_value(config, "Lucida", name)
		       ? config_get_bool(config, "Lucida", name)
		       : fallback;
}

bool IsSilent(const std::vector<float> &samples)
{
	return std::none_of(samples.begin(), samples.end(), [](float s) { return std::fabs(s) > 1e-3f; });
}

QString SrtTime(double seconds)
{
	const qint64 ms = std::max<qint64>(std::llround(seconds * 1000.0), 0);
	return QStringLiteral("%1:%2:%3,%4")
		.arg(ms / 3600000, 2, 10, QLatin1Char('0'))
		.arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
		.arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
		.arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace

bool LoadSettings(config_t *profile, Settings &settings, QString &error)
{
	using namespace spectra::speech;
	const ModelInfo *model = FindWhisperModel(ConfigString(profile, "SpeechModel"));
	if (!model) {
		model = FindWhisperModel(DefaultWhisperModel());
	}
	if (!model || !ModelInstalled(*model)) {
		error = QTStr("Spectra.ClipMaker.Captions.NoModel");
		return false;
	}
	settings.modelPath = ModelPath(*model);
	settings.vadModelPath = ModelInstalled(VadModel()) ? ModelPath(VadModel()) : QString();
	settings.language = ConfigString(profile, "SpeechLanguage");
	if (settings.language.isEmpty()) {
		settings.language = QStringLiteral("auto");
	}
	settings.prompt = ConfigString(profile, "SpeechPrompt");
	settings.useGpu = ConfigBool(profile, "SpeechGpu", true);
	return true;
}

bool Transcribe(const std::vector<Source> &sources, const Settings &settings, std::vector<Cue> &cues,
		const std::function<bool(float)> &progress, QString &error)
{
	using namespace spectra::speech;
	cues.clear();

	struct Job {
		const Source *source;
		int track;
		QString speaker;
	};
	std::vector<Job> jobs;
	for (const Source &source : sources) {
		const int tracks = AudioTrackCount(source.path, &error);
		if (tracks < 0) {
			return false;
		}
		if (tracks >= kSpeakerTracks) {
			jobs.push_back({&source, 1, QStringLiteral("me")});
			jobs.push_back({&source, 2, QStringLiteral("teamspeak")});
			jobs.push_back({&source, 3, QStringLiteral("game")});
		} else if (tracks > 0) {
			jobs.push_back({&source, 0, QString()});
		}
	}
	if (jobs.empty()) {
		return true;
	}

	auto model = Model::Load(settings.modelPath, settings.useGpu, &error);
	if (!model) {
		return false;
	}

	Options options;
	options.language = settings.language;
	options.prompt = settings.prompt;
	options.vadModelPath = settings.vadModelPath;
	options.threads = settings.useGpu ? 4 : std::max(2, (int)std::thread::hardware_concurrency() / 2);

	for (size_t j = 0; j < jobs.size(); j++) {
		const Job &job = jobs[j];
		auto step = [&](float fraction) {
			return !progress || progress((j + std::clamp(fraction, 0.0f, 1.0f)) / jobs.size());
		};
		if (!step(0.0f)) {
			error = QTStr("Spectra.ClipMaker.Captions.Cancelled");
			return false;
		}

		std::vector<float> samples;
		if (!ReadAudio(job.source->path, job.track, job.source->from, job.source->to - job.source->from,
			       samples, &error)) {
			return false;
		}
		if (IsSilent(samples)) {
			continue;
		}
		std::vector<Segment> segments;
		if (!model->Transcribe(samples, options, segments, step, &error)) {
			return false;
		}
		for (const Segment &s : segments) {
			if (!IsLikelyHallucination(s)) {
				cues.push_back({job.source->at + s.start, job.source->at + s.end, s.text, job.speaker});
			}
		}
	}

	std::sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
	if (progress) {
		progress(1.0f);
	}
	return true;
}

QString SpeakerName(const QString &speaker)
{
	if (speaker == QLatin1String("me")) {
		return QTStr("Spectra.ClipMaker.Captions.Me");
	}
	if (speaker == QLatin1String("teamspeak")) {
		return QTStr("Spectra.ClipMaker.Captions.TeamSpeak");
	}
	if (speaker == QLatin1String("game")) {
		return QTStr("Spectra.ClipMaker.Captions.Game");
	}
	return QString();
}

QString Display(const Cue &cue, bool showSpeaker)
{
	const QString who = showSpeaker ? SpeakerName(cue.speaker) : QString();
	return who.isEmpty() ? cue.text : QStringLiteral("%1: %2").arg(who, cue.text);
}

QImage Render(const QString &text, int width, int height)
{
	QImage image(std::max(width, 1), std::max(height, 1), QImage::Format_ARGB32);
	image.fill(Qt::transparent);
	if (text.trimmed().isEmpty()) {
		return image;
	}

	QFont font(QStringLiteral("Segoe UI"));
	font.setPixelSize(std::max(14, height / 22));
	font.setBold(true);
	const QFontMetrics metrics(font);

	/* Wrap by words to 84% of the width */
	const int maxWidth = width * 84 / 100;
	QStringList lines;
	QString line;
	for (const QString &word : text.simplified().split(' ')) {
		const QString candidate = line.isEmpty() ? word : line + ' ' + word;
		if (!line.isEmpty() && metrics.horizontalAdvance(candidate) > maxWidth) {
			lines << line;
			line = word;
		} else {
			line = candidate;
		}
	}
	if (!line.isEmpty()) {
		lines << line;
	}

	const int lineHeight = metrics.lineSpacing();
	const int bottom = height - height * 7 / 100;
	QPainterPath path;
	for (int i = 0; i < lines.size(); i++) {
		const int x = (width - metrics.horizontalAdvance(lines[i])) / 2;
		const int baseline = bottom - (int)(lines.size() - 1 - i) * lineHeight - metrics.descent();
		path.addText(x, baseline, font, lines[i]);
	}

	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	QPen outline(QColor(0, 0, 0, 230), std::max(2.0, font.pixelSize() / 7.0), Qt::SolidLine, Qt::RoundCap,
		     Qt::RoundJoin);
	painter.strokePath(path, outline);
	painter.fillPath(path, Qt::white);
	painter.end();
	return image;
}

ClipRender::Overlay ToOverlay(const QImage &image, double start, double end)
{
	const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
	ClipRender::Overlay overlay;
	overlay.start = start;
	overlay.end = end;
	overlay.width = argb.width();
	overlay.height = argb.height();
	overlay.bgra.resize((size_t)overlay.width * overlay.height * 4);
	overlay.x0 = overlay.width;
	overlay.y0 = overlay.height;
	for (int y = 0; y < overlay.height; y++) {
		/* ARGB32 is B, G, R, A in memory on little-endian machines */
		const uint8_t *src = argb.constScanLine(y);
		std::copy(src, src + overlay.width * 4, overlay.bgra.begin() + (size_t)y * overlay.width * 4);
		for (int x = 0; x < overlay.width; x++) {
			if (src[x * 4 + 3]) {
				overlay.x0 = std::min(overlay.x0, x);
				overlay.x1 = std::max(overlay.x1, x + 1);
				overlay.y0 = std::min(overlay.y0, y);
				overlay.y1 = std::max(overlay.y1, y + 1);
			}
		}
	}
	return overlay;
}

bool WriteSrt(const QString &path, const std::vector<Cue> &cues, double offset, double length, bool showSpeaker,
	      QString &error)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		error = file.errorString();
		return false;
	}
	int index = 1;
	for (const Cue &cue : cues) {
		const double start = std::max(cue.start - offset, 0.0);
		const double end = std::min(cue.end - offset, length);
		if (end <= start) {
			continue;
		}
		const QString entry = QStringLiteral("%1\n%2 --> %3\n%4\n\n")
					      .arg(index++)
					      .arg(SrtTime(start), SrtTime(end), Display(cue, showSpeaker));
		file.write(entry.toUtf8());
	}
	if (!file.commit()) {
		error = file.errorString();
		return false;
	}
	return true;
}

} // namespace ClipCaptions
