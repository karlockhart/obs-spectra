#include "lucida-controller.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QStandardPaths>

#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define SECTION "Lucida"

namespace lucida {

namespace {

constexpr auto kPruneEvery = std::chrono::hours(1);
constexpr int kLoopPollMs = 2000;

QString ConfigString(config_t *config, const char *section, const char *name)
{
	const char *v = config_get_string(config, section, name);
	return QString::fromUtf8(v ? v : "");
}

void LoadRegion(config_t *c, const char *prefix, spectra::Region &r)
{
	const std::string p(prefix);
	r.left = config_get_double(c, SECTION, (p + "Left").c_str());
	r.top = config_get_double(c, SECTION, (p + "Top").c_str());
	r.right = config_get_double(c, SECTION, (p + "Right").c_str());
	r.bottom = config_get_double(c, SECTION, (p + "Bottom").c_str());
}

void SaveRegion(config_t *c, const char *prefix, const spectra::Region &r)
{
	const std::string p(prefix);
	config_set_double(c, SECTION, (p + "Left").c_str(), r.left);
	config_set_double(c, SECTION, (p + "Top").c_str(), r.top);
	config_set_double(c, SECTION, (p + "Right").c_str(), r.right);
	config_set_double(c, SECTION, (p + "Bottom").c_str(), r.bottom);
}

void SetDefaults(config_t *c)
{
	const Settings d;
	const RecorderConfig &r = d.recorder;
	const QString folder = Settings::DefaultFolder();
	config_set_default_bool(c, SECTION, "Enabled", d.enabled);
	config_set_default_double(c, SECTION, "Interval", r.interval);
	config_set_default_double(c, SECTION, "MinInterval", r.minInterval);
	config_set_default_double(c, SECTION, "MaxInterval", r.maxInterval);
	config_set_default_bool(c, SECTION, "Adaptive", r.adaptive);
	config_set_default_double(c, SECTION, "IdleInterval", r.idleInterval);
	config_set_default_int(c, SECTION, "OcrThreads", d.ocrThreads);
	config_set_default_bool(c, SECTION, "ReadHud", r.readHud);
	config_set_default_double(c, SECTION, "GateThreshold", r.gateThreshold);
	config_set_default_string(c, SECTION, "DbPath", QDir(folder).filePath("chatlog.db").toUtf8().constData());
	config_set_default_int(c, SECTION, "RetentionDays", d.retentionDays);
	config_set_default_bool(c, SECTION, "KeepFrames", true);
	config_set_default_string(c, SECTION, "FramesDir", QDir(folder).filePath("frames").toUtf8().constData());
	config_set_default_int(c, SECTION, "FrameQuality", r.frameQuality);
	config_set_default_int(c, SECTION, "FrameRetentionDays", r.frameRetentionDays);
	config_set_default_bool(c, SECTION, "KeepCrops", r.keepCrops);
	config_set_default_string(c, SECTION, "CropsDir", QDir(folder).filePath("crops").toUtf8().constData());
	config_set_default_int(c, SECTION, "CropQuality", r.cropQuality);
	config_set_default_int(c, SECTION, "CropRetentionDays", r.cropRetentionDays);
	config_set_default_string(c, SECTION, "TargetProcess", d.targetProcess.toUtf8().constData());
	config_set_default_bool(c, SECTION, "FollowLoop", d.followLoop);
	config_set_default_string(c, SECTION, "TagRules", TagRulesToJson(d.tagRules).toUtf8().constData());
	config_set_default_bool(c, SECTION, "TolerateTypos", d.tolerateTypos);
	const spectra::Region chat = spectra::kDefaultChatRegion, hud = spectra::kDefaultHudRegion;
	for (auto [prefix, region] : {std::pair{"Chat", chat}, std::pair{"Hud", hud}}) {
		const std::string p(prefix);
		config_set_default_double(c, SECTION, (p + "Left").c_str(), region.left);
		config_set_default_double(c, SECTION, (p + "Top").c_str(), region.top);
		config_set_default_double(c, SECTION, (p + "Right").c_str(), region.right);
		config_set_default_double(c, SECTION, (p + "Bottom").c_str(), region.bottom);
	}
}

#ifdef _WIN32
void LowerThreadPriority()
{
	/* keep the game ahead of OCR */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}
#else
void LowerThreadPriority() {}
#endif

} // namespace

QString Settings::DefaultFolder()
{
	config_t *c = obs_frontend_get_profile_config();
	QString path = c ? ConfigString(c, "Spectra", "LucidaPath") : QString();
	if (path.isEmpty()) {
		path = QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("Spectra/Lucida");
	}
	return QDir::cleanPath(path);
}

Settings Settings::Load()
{
	Settings s;
	config_t *c = obs_frontend_get_profile_config();
	if (!c) {
		return s;
	}
	SetDefaults(c);
	RecorderConfig &r = s.recorder;
	s.enabled = config_get_bool(c, SECTION, "Enabled");
	r.interval = config_get_double(c, SECTION, "Interval");
	r.minInterval = config_get_double(c, SECTION, "MinInterval");
	r.maxInterval = config_get_double(c, SECTION, "MaxInterval");
	r.adaptive = config_get_bool(c, SECTION, "Adaptive");
	r.idleInterval = config_get_double(c, SECTION, "IdleInterval");
	s.ocrThreads = (int)config_get_int(c, SECTION, "OcrThreads");
	r.readHud = config_get_bool(c, SECTION, "ReadHud");
	r.gateThreshold = config_get_double(c, SECTION, "GateThreshold");
	s.dbPath = ConfigString(c, SECTION, "DbPath");
	s.retentionDays = (int)config_get_int(c, SECTION, "RetentionDays");
	r.keepFrames = config_get_bool(c, SECTION, "KeepFrames");
	r.framesDir = ConfigString(c, SECTION, "FramesDir");
	r.frameQuality = (int)config_get_int(c, SECTION, "FrameQuality");
	r.frameRetentionDays = (int)config_get_int(c, SECTION, "FrameRetentionDays");
	r.keepCrops = config_get_bool(c, SECTION, "KeepCrops");
	r.cropsDir = ConfigString(c, SECTION, "CropsDir");
	r.cropQuality = (int)config_get_int(c, SECTION, "CropQuality");
	r.cropRetentionDays = (int)config_get_int(c, SECTION, "CropRetentionDays");
	s.targetProcess = ConfigString(c, SECTION, "TargetProcess");
	s.followLoop = config_get_bool(c, SECTION, "FollowLoop");
	s.tagRules = TagRulesFromJson(ConfigString(c, SECTION, "TagRules"));
	s.tolerateTypos = config_get_bool(c, SECTION, "TolerateTypos");
	LoadRegion(c, "Chat", r.chatRegion);
	LoadRegion(c, "Hud", r.hudRegion);
	return s;
}

void Settings::Save() const
{
	config_t *c = obs_frontend_get_profile_config();
	if (!c) {
		return;
	}
	const RecorderConfig &r = recorder;
	config_set_bool(c, SECTION, "Enabled", enabled);
	config_set_double(c, SECTION, "Interval", r.interval);
	config_set_double(c, SECTION, "MinInterval", r.minInterval);
	config_set_double(c, SECTION, "MaxInterval", r.maxInterval);
	config_set_bool(c, SECTION, "Adaptive", r.adaptive);
	config_set_double(c, SECTION, "IdleInterval", r.idleInterval);
	config_set_int(c, SECTION, "OcrThreads", ocrThreads);
	config_set_bool(c, SECTION, "ReadHud", r.readHud);
	config_set_double(c, SECTION, "GateThreshold", r.gateThreshold);
	config_set_string(c, SECTION, "DbPath", dbPath.toUtf8().constData());
	config_set_int(c, SECTION, "RetentionDays", retentionDays);
	config_set_bool(c, SECTION, "KeepFrames", r.keepFrames);
	config_set_string(c, SECTION, "FramesDir", r.framesDir.toUtf8().constData());
	config_set_int(c, SECTION, "FrameQuality", r.frameQuality);
	config_set_int(c, SECTION, "FrameRetentionDays", r.frameRetentionDays);
	config_set_bool(c, SECTION, "KeepCrops", r.keepCrops);
	config_set_string(c, SECTION, "CropsDir", r.cropsDir.toUtf8().constData());
	config_set_int(c, SECTION, "CropQuality", r.cropQuality);
	config_set_int(c, SECTION, "CropRetentionDays", r.cropRetentionDays);
	config_set_string(c, SECTION, "TargetProcess", targetProcess.toUtf8().constData());
	config_set_bool(c, SECTION, "FollowLoop", followLoop);
	config_set_string(c, SECTION, "TagRules", TagRulesToJson(tagRules).toUtf8().constData());
	config_set_bool(c, SECTION, "TolerateTypos", tolerateTypos);
	SaveRegion(c, "Chat", r.chatRegion);
	SaveRegion(c, "Hud", r.hudRegion);
	config_save_safe(c, "tmp", nullptr);
}

/* ------------------------------------------------------------------------- */

QString LoopDirectory()
{
	/* as the frontend's LoopRecorder::LoopDirectory */
	config_t *c = obs_frontend_get_profile_config();
	QString dir = c ? ConfigString(c, "SpectraLoop", "Path") : QString();
	if (dir.isEmpty()) {
		char *output = obs_frontend_get_current_record_output_path();
		dir = QDir(QString::fromUtf8(output ? output : "")).filePath(QStringLiteral("Spectra Loop"));
		bfree(output);
	}
	return QDir::cleanPath(dir);
}

bool LoopRecordingActive()
{
	obs_output_t *output = obs_get_output_by_name("spectra_loop_output");
	const bool active = output && obs_output_active(output);
	obs_output_release(output);
	return active;
}

Controller::Controller(QObject *parent) : QObject(parent), settings(Settings::Load())
{
	loopDir = LoopDirectory();
	loopPoll.setInterval(kLoopPollMs);
	connect(&loopPoll, &QTimer::timeout, this, &Controller::PollLoop);
	loopPoll.start();
}

Controller::~Controller()
{
	Stop();
}

void Controller::SetStatus(const QString &text)
{
	QMetaObject::invokeMethod(
		this,
		[this, text]() {
			status = text;
			emit statusChanged(text);
		},
		Qt::QueuedConnection);
}

bool Controller::OpenReader()
{
	if (reader && reader->IsOpen() && reader->Path() == settings.dbPath) {
		return true;
	}
	reader = std::make_unique<Store>(settings.dbPath);
	QString error;
	if (!reader->Open(&error) || !reader->IsOpen()) {
		emit failed(QStringLiteral("Could not open the chat log %1: %2").arg(settings.dbPath, error));
		reader.reset();
		return false;
	}
	return true;
}

void Controller::PollLoop()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		loopDir = LoopDirectory();
	}
	if (!settings.followLoop || !settings.enabled || paused) {
		return;
	}
	const bool loop = LoopRecordingActive();
	if (loop && !running) {
		Start();
	} else if (!loop && running) {
		Stop();
		status = obs_module_text("Lucida.Status.WaitingForLoop");
		emit statusChanged(status);
	}
}

void Controller::Start()
{
	if (running || !settings.enabled) {
		return;
	}
	/* the log stays browsable while sampling waits for the loop recording */
	if (!OpenReader()) {
		return;
	}
	if (settings.followLoop && !LoopRecordingActive()) {
		status = obs_module_text("Lucida.Status.WaitingForLoop");
		emit statusChanged(status);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopRequested = false;
		wakeRequested = false;
	}
	running = true;
	SetStatus(obs_module_text("Lucida.Status.Starting"));
	worker = std::thread(&Controller::Run, this, settings);
}

void Controller::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopRequested = true;
	}
	wakeup.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
	running = false;
}

void Controller::SetPaused(bool pause)
{
	if (pause == paused) {
		return;
	}
	paused = pause;
	if (paused) {
		Stop();
		status = obs_module_text("Lucida.Status.Paused");
		emit statusChanged(status);
	} else {
		Start();
	}
}

void Controller::SampleNow()
{
	if (paused) {
		SetPaused(false);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		wakeRequested = true;
	}
	wakeup.notify_all();
}

void Controller::ApplySettings(const Settings &s)
{
	bool wasRunning = running;
	Stop();
	settings = s;
	settings.Save();
	if ((wasRunning || !paused) && settings.enabled) {
		Start();
	} else if (!settings.enabled) {
		status = obs_module_text("Lucida.Status.Disabled");
		emit statusChanged(status);
	}
}

void Controller::Run(Settings s)
{
	LowerThreadPriority();

	std::string error;
	spectra::OcrEngine::Options options;
	options.threads = s.ocrThreads;
	std::unique_ptr<spectra::OcrEngine> ocr = spectra::OcrEngine::Create(options, error);
	if (!ocr) {
		QString msg = QString::fromStdString(error);
		QMetaObject::invokeMethod(this, [this, msg]() { emit failed(msg); }, Qt::QueuedConnection);
		SetStatus(QStringLiteral("%1: %2").arg(obs_module_text("Lucida.Status.Stopped"), msg));
		return;
	}

	Store store(s.dbPath);
	QString dbError;
	if (!store.Open(&dbError) || !store.IsOpen()) {
		QMetaObject::invokeMethod(this, [this, dbError]() { emit failed(dbError); }, Qt::QueuedConnection);
		return;
	}

	Tagger tagger(s.tagRules, s.tolerateTypos);
	store.labeler = [&tagger](const QString &body) {
		return tagger.Tags(body);
	};

	spectra::FrameGrabber grabber;
	grabber.SetTargetProcess(s.targetProcess);
	/* Troubleshooting: SPECTRA_LUCIDA_DUMP=<folder> saves every grabbed frame */
	const QString dumpDir = qEnvironmentVariable("SPECTRA_LUCIDA_DUMP");
	int dumped = 0;
	Recorder recorder(s.recorder, store, *ocr, [&grabber, &dumpDir, &dumped]() {
		std::optional<GrabbedFrame> frame;
		if (std::optional<spectra::SourceFrame> f = grabber.Grab()) {
			frame = GrabbedFrame{std::move(f->image), f->source};
		}
		if (frame && !dumpDir.isEmpty()) {
			QDir().mkpath(dumpDir);
			const spectra::Image &img = frame->image;
			QImage q(img.width, img.height, QImage::Format_RGB888);
			for (int y = 0; y < img.height; y++) {
				const uint8_t *src = img.row(y);
				uchar *dst = q.scanLine(y);
				for (int x = 0; x < img.width; x++) {
					dst[x * 3 + 0] = src[x * 3 + 2];
					dst[x * 3 + 1] = src[x * 3 + 1];
					dst[x * 3 + 2] = src[x * 3 + 0];
				}
			}
			q.save(QDir(dumpDir).filePath(QStringLiteral("frame-%1.png").arg(++dumped, 4, 10, QChar('0'))));
		}
		return frame;
	});

	recorder.locateVideo = [this](double wallTs) -> std::optional<VideoSpot> {
		QString dir;
		{
			std::lock_guard<std::mutex> lock(mutex);
			dir = loopDir;
		}
		return LocateVideo(dir, wallTs, LoopRecordingActive());
	};

	recorder.PruneImages();
	auto lastPrune = std::chrono::steady_clock::now() - kPruneEvery;

	while (true) {
		if (s.retentionDays > 0 && std::chrono::steady_clock::now() - lastPrune >= kPruneEvery) {
			QStringList orphans;
			store.Prune(s.retentionDays, &orphans);
			for (const QString &f : orphans) {
				QFile::remove(f);
			}
			recorder.PruneImages();
			lastPrune = std::chrono::steady_clock::now();
		}

		Tick tick;
		try {
			tick = recorder.Step();
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[Lucida] Tick failed: %s", e.what());
			tick.interval = recorder.Interval();
		}

		if (tick.foundWindow && tick.changed) {
			blog(tick.added ? LOG_INFO : LOG_DEBUG, "[Lucida] %s", tick.Describe().toUtf8().constData());
		}
		const int added = tick.added;
		const double interval = tick.interval;
		const bool found = tick.foundWindow;
		QMetaObject::invokeMethod(
			this,
			[this, added, interval, found]() {
				logged += added;
				QString text = !found ? QString::fromUtf8(obs_module_text("Lucida.Status.Waiting"))
						      : QString::fromUtf8(obs_module_text("Lucida.Status.Running"))
								.arg(logged)
								.arg(interval, 0, 'f', 1);
				status = text;
				emit statusChanged(text);
				emit ticked(added, interval, found);
			},
			Qt::QueuedConnection);

		std::unique_lock<std::mutex> lock(mutex);
		wakeup.wait_for(lock, std::chrono::duration<double>(std::max(0.1, tick.interval)),
				[this] { return stopRequested || wakeRequested; });
		if (stopRequested) {
			break;
		}
		wakeRequested = false;
	}
	recorder.Close();
}

std::optional<VideoSpot> Controller::VideoFor(const LogLine &line) const
{
	if (line.video && QFileInfo::exists(line.video->path)) {
		return line.video;
	}
	QString dir;
	{
		std::lock_guard<std::mutex> lock(mutex);
		dir = loopDir;
	}
	if (line.firstSeen <= 0) {
		return std::nullopt;
	}
	return LocateVideo(dir, line.firstSeen, LoopRecordingActive());
}

void Controller::Relabel()
{
	const QString path = settings.dbPath;
	const QList<TagRule> rules = settings.tagRules;
	const bool typos = settings.tolerateTypos;
	QPointer<Controller> self(this);
	std::thread([self, path, rules, typos] {
		Tagger tagger(rules, typos);
		Store store(path);
		int changed = -1;
		if (store.Open() && store.IsOpen()) {
			store.labeler = [&tagger](const QString &body) {
				return tagger.Tags(body);
			};
			changed = store.Relabel();
		}
		blog(LOG_INFO, "[Lucida] Re-tagged the log: %d line(s) changed", changed);
		QMetaObject::invokeMethod(
			qApp,
			[self, changed] {
				if (self) {
					emit self->relabelled(changed);
				}
			},
			Qt::QueuedConnection);
	}).detach();
}

} // namespace lucida
