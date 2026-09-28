#include "lucida-speech.hpp"
#include "lucida-host.hpp"
#include "tagger.hpp"
#include "video.hpp"

#include <spectra-speech/audio.hpp>
#include <spectra-speech/models.hpp>
#include <spectra-speech/speech.hpp>

#include <obs-module.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lucida {

namespace {

constexpr int kLoopPollMs = 2000;
constexpr int kIdleWaitMs = 10000;
/* A segment the loop closed this recently may still be being flushed */
constexpr int kSettleSec = 10;

/* Loop tracks with speaker tracks on: 1 mix, 2 me, 3 TeamSpeak, 4 game */
constexpr int kSpeakerTracks = 4;

struct Job {
	int track;
	QString speaker;
};

bool IsSilent(const std::vector<float> &samples)
{
	for (float s : samples) {
		if (std::fabs(s) > 1e-3f) {
			return false;
		}
	}
	return true;
}

} // namespace

SpeechController::SpeechController(QObject *parent) : QObject(parent)
{
	loopPoll.setInterval(kLoopPollMs);
	connect(&loopPoll, &QTimer::timeout, this, &SpeechController::PollLoop);
	loopPoll.start();
	PollLoop();
	worker = std::thread(&SpeechController::Run, this);
}

SpeechController::~SpeechController()
{
	Stop();
}

void SpeechController::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopRequested = true;
	}
	wakeup.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

void SpeechController::SetStoredSources(std::function<std::vector<lan::StoredSource>()> sources)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		config.storedSources = std::move(sources);
		configChanged = true;
	}
	wakeup.notify_all();
}

void SpeechController::Apply(const Settings &settings)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		config.speech = settings.speech;
		config.dbPath = settings.dbPath;
		config.tagRules = settings.tagRules;
		config.tolerateTypos = settings.tolerateTypos;
		configChanged = true;
	}
	wakeup.notify_all();
}

QString SpeechController::Status() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return status;
}

void SpeechController::PollLoop()
{
	const QString dir = LoopDirectory();
	const bool active = LoopRecordingActive();
	bool stopped = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopped = loopActive && !active;
		loopDir = dir;
		loopActive = active;
	}
	/* The last segment of a session is finished once the loop stops */
	if (stopped) {
		wakeup.notify_all();
	}
}

void SpeechController::SetStatus(const QString &text)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (status == text) {
			return;
		}
		status = text;
	}
	QMetaObject::invokeMethod(this, [this, text]() { emit statusChanged(text); }, Qt::QueuedConnection);
}

bool SpeechController::Wait(int ms)
{
	std::unique_lock<std::mutex> lock(mutex);
	wakeup.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return stopRequested || configChanged; });
	configChanged = false;
	return !stopRequested;
}

void SpeechController::Run()
{
#ifdef _WIN32
	/* keep the game ahead of transcription */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
	using namespace spectra::speech;

	std::map<QString, std::unique_ptr<Store>> stores; /* by path */
	std::unique_ptr<Tagger> tagger;
	std::unique_ptr<Model> model;
	QString loadedModel;
	bool loadedGpu = true;
	QDateTime retryDownloadAt;

	auto stopping = [this]() {
		std::lock_guard<std::mutex> lock(mutex);
		return stopRequested;
	};

	for (;;) {
		Config c;
		QString dir;
		bool recording = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopRequested) {
				break;
			}
			c = config;
			configChanged = false;
			dir = loopDir;
			recording = loopActive;
		}

		/* Fetch the model and the voice activity model if they're missing,
		 * transcription on or not: the Clip Maker's captions use them too */
		if (c.speech.autoDownload && !ModelDirectory().isEmpty() &&
		    (!retryDownloadAt.isValid() || QDateTime::currentDateTime() >= retryDownloadAt)) {
			const ModelInfo *wanted = FindWhisperModel(c.speech.model);
			if (!wanted) {
				wanted = FindWhisperModel(DefaultWhisperModel());
			}
			for (const ModelInfo *m : {&VadModel(), wanted}) {
				if (!m || ModelInstalled(*m)) {
					continue;
				}
				blog(LOG_INFO, "[Lucida] Downloading the speech model %s",
				     m->file.toUtf8().constData());
				QString error;
				const bool ok = DownloadModel(
					*m,
					[&](qint64 received, qint64 total) {
						SetStatus(Text("Lucida.Speech.Status.Downloading")
								  .arg(m->title)
								  .arg(total > 0 ? (int)(received * 100 / total) : 0));
						return !stopping();
					},
					&error);
				if (stopping()) {
					break;
				}
				if (!ok) {
					blog(LOG_WARNING, "[Lucida] Could not download %s: %s",
					     m->file.toUtf8().constData(), error.toUtf8().constData());
					SetStatus(Text("Lucida.Speech.Status.DownloadFailed").arg(error));
					retryDownloadAt = QDateTime::currentDateTime().addSecs(600);
					break;
				}
				blog(LOG_INFO, "[Lucida] Downloaded %s", m->file.toUtf8().constData());
			}
			if (stopping()) {
				break;
			}
		}

		/* What to transcribe: this PC's loop, unless its storage PC does it,
		 * and the loops paired PCs keep here and asked to have transcribed.
		 * Each segment is transcribed on one PC only. */
		struct Target {
			QString loopDir;
			QString dbPath;
			bool recording = false;
			double since = 0.0;
			QString pc; /* empty: this PC */
		};
		std::vector<Target> targets;
		const bool own = c.speech.enabled && !c.speech.onStorage;
		const bool waitingForGame = own && c.speech.when == SpeechSettings::When::AfterGame && recording;
		if (own && !waitingForGame) {
			targets.push_back({dir, c.dbPath, recording, c.speech.since, QString()});
		}
		if (c.speech.forOthers && c.storedSources) {
			for (const lan::StoredSource &s : c.storedSources()) {
				if (s.speech) {
					/* a transcript of its own: the copy of its log keeps its ids */
					targets.push_back(
						{s.LoopFolder(), s.SpeechLogPath(), false, s.speechSince, s.name});
				}
			}
		}
		if (targets.empty()) {
			model.reset();
			loadedModel.clear();
			SetStatus(waitingForGame                           ? Text("Lucida.Speech.Status.WaitingForGame")
				  : c.speech.enabled && c.speech.onStorage ? Text("Lucida.Speech.Status.OnStorage")
									   : QString());
			if (!Wait(c.speech.forOthers ? kIdleWaitMs : 60000)) {
				break;
			}
			continue;
		}

		/* The oldest finished segment without a transcript, this PC's first */
		std::optional<Next> next;
		Store *store = nullptr;
		QString pc;
		int pending = 0;
		bool failed = false;
		for (const Target &t : targets) {
			std::unique_ptr<Store> &slot = stores[t.dbPath];
			if (!slot) {
				slot = std::make_unique<Store>(t.dbPath);
				QString error;
				if (!slot->Open(&error)) {
					slot.reset();
					stores.erase(t.dbPath);
					if (t.pc.isEmpty()) {
						SetStatus(Text("Lucida.Speech.Status.NoLog").arg(error));
						failed = true;
					}
					continue;
				}
			}
			Store *s = slot.get();
			if (t.pc.isEmpty()) {
				/* this PC's own lines are tagged here; paired PCs tag theirs */
				tagger = std::make_unique<Tagger>(c.tagRules, c.tolerateTypos);
				Tagger *tags = tagger.get();
				s->labeler = [tags](const QString &body) {
					return tags->Tags(body);
				};
			}
			QDir loop(t.loopDir);
			const QFileInfoList files = loop.entryInfoList({"*.mkv"}, QDir::Files, QDir::Name);
			const QDateTime settled = QDateTime::currentDateTime().addSecs(-kSettleSec);
			for (qsizetype i = 0; i < files.size(); i++) {
				const QFileInfo &fi = files[i];
				const double start = SegmentStart(fi.fileName());
				if (!(start > 0.0) || (t.since > 0.0 && start < t.since - 1.0)) {
					continue;
				}
				/* The newest one is still being written while the loop runs */
				if ((t.recording && i == files.size() - 1) ||
				    (t.pc.isEmpty() && fi.lastModified() > settled)) {
					continue;
				}
				const QString path = QDir::cleanPath(fi.absoluteFilePath());
				const auto seg = s->GetSpeechSegment(path);
				if (seg && seg->size == fi.size()) {
					continue;
				}
				pending++;
				if (!next) {
					next = Next{path, fi.size()};
					store = s;
					pc = t.pc;
				}
			}
		}
		/* forget logs of PCs no longer transcribed for */
		for (auto it = stores.begin(); it != stores.end();) {
			const bool used = std::any_of(targets.begin(), targets.end(),
						      [&](const Target &t) { return t.dbPath == it->first; });
			it = used ? std::next(it) : stores.erase(it);
		}
		if (!next) {
			if (!failed) {
				SetStatus(Text("Lucida.Speech.Status.UpToDate"));
			}
			if (!Wait(kIdleWaitMs)) {
				break;
			}
			continue;
		}

		const ModelInfo *info = FindWhisperModel(c.speech.model);
		if (!info) {
			info = FindWhisperModel(DefaultWhisperModel());
		}
		if (!ModelInstalled(*info)) {
			model.reset();
			loadedModel.clear();
			SetStatus(Text("Lucida.Speech.Status.NoModel"));
			if (!Wait(30000)) {
				break;
			}
			continue;
		}
		if (!model || loadedModel != info->id || loadedGpu != c.speech.useGpu) {
			model.reset();
			SetStatus(Text("Lucida.Speech.Status.Loading"));
			QString error;
			model = Model::Load(ModelPath(*info), c.speech.useGpu, &error);
			if (!model) {
				SetStatus(error);
				if (!Wait(30000)) {
					break;
				}
				continue;
			}
			loadedModel = info->id;
			loadedGpu = c.speech.useGpu;
		}

		Options options;
		options.language = c.speech.language;
		options.prompt = c.speech.prompt;
		options.threads = c.speech.useGpu ? 4 : std::max(2, (int)std::thread::hardware_concurrency() / 2);
		if (ModelInstalled(VadModel())) {
			options.vadModelPath = ModelPath(VadModel());
		}

		const QString name =
			pc.isEmpty() ? QFileInfo(next->path).completeBaseName()
				     : QStringLiteral("%1: %2").arg(pc, QFileInfo(next->path).completeBaseName());
		const double segmentStart = SegmentStart(QFileInfo(next->path).fileName());
		std::vector<Job> jobs;
		QString error;
		const int tracks = AudioTrackCount(next->path, &error);
		if (tracks >= kSpeakerTracks) {
			if (c.speech.me) {
				jobs.push_back({1, QStringLiteral("me")});
			}
			if (c.speech.teamSpeak) {
				jobs.push_back({2, QStringLiteral("teamspeak")});
			}
			if (c.speech.game) {
				jobs.push_back({3, QStringLiteral("game")});
			}
		} else if (tracks > 0) {
			jobs.push_back({0, QString()});
		}
		if (tracks < 0) {
			/* Gone (e.g. deleted by the disk quota) or unreadable */
			if (QFileInfo::exists(next->path)) {
				store->MarkSpeechFailed(next->path, next->size, error);
			}
			continue;
		}

		std::vector<SpeechLine> lines;
		bool ok = true;
		for (size_t j = 0; j < jobs.size() && ok; j++) {
			const Job &job = jobs[j];
			const QString who = job.speaker.isEmpty()
						    ? Text("Lucida.Speech.Mix")
						    : Text(("Lucida.Speech." + job.speaker).toUtf8().constData());
			SetStatus(Text("Lucida.Speech.Status.Reading").arg(name, who).arg(pending));
			std::vector<float> samples;
			if (!ReadAudio(next->path, job.track, 0.0, -1.0, samples, &error)) {
				ok = false;
				break;
			}
			if (IsSilent(samples)) {
				continue;
			}
			std::vector<Segment> segments;
			auto progress = [&](float fraction) {
				SetStatus(Text("Lucida.Speech.Status.Transcribing")
						  .arg(name, who)
						  .arg((int)(fraction * 100))
						  .arg(pending));
				std::lock_guard<std::mutex> lock(mutex);
				return !stopRequested;
			};
			if (!model->Transcribe(samples, options, segments, progress, &error)) {
				ok = false;
				break;
			}
			for (const Segment &s : segments) {
				if (IsLikelyHallucination(s)) {
					continue;
				}
				lines.push_back(SpeechLine{segmentStart + s.start, job.speaker, s.text, s.confidence,
							   VideoSpot{next->path, s.start}});
			}
		}

		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopRequested) {
				/* Not recorded as done: it's picked up again next time */
				break;
			}
		}
		if (!ok) {
			blog(LOG_WARNING, "[Lucida] Could not transcribe %s: %s", next->path.toUtf8().constData(),
			     error.toUtf8().constData());
			store->MarkSpeechFailed(next->path, next->size, error);
			continue;
		}

		std::sort(lines.begin(), lines.end(),
			  [](const SpeechLine &a, const SpeechLine &b) { return a.at < b.at; });
		store->SetSegmentSpeech(next->path, next->size, lines);
		blog(LOG_INFO, "[Lucida] Transcribed %s: %d line(s)", next->path.toUtf8().constData(),
		     (int)lines.size());
		const QString path = next->path;
		const int count = (int)lines.size();
		QMetaObject::invokeMethod(
			this, [this, path, count]() { emit transcribed(path, count); }, Qt::QueuedConnection);
	}
}

} // namespace lucida
