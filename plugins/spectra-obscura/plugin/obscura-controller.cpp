#include "obscura-controller.hpp"
#include "capture-win.hpp"
#include "naming.hpp"
#include "review-window.hpp"
#include "settings-dialog.hpp"
#include "snip-overlay.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <spectra-censor/censor.hpp>
#include <spectra-grab/frame-grabber.hpp>
#include <spectra-vision/imaging.hpp>
#include <spectra-vision/ocr.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUrl>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace obscura {

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Spectra's Obscura folder (set by the setup wizard), for output when
 * Obscura's config does not name one */
QString SpectraFolder()
{
	config_t *c = obs_frontend_get_profile_config();
	const char *path = c ? config_get_string(c, "Spectra", "ObscuraPath") : nullptr;
	if (path && *path) {
		return QDir::cleanPath(QString::fromUtf8(path));
	}
	return QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("Spectra/Obscura");
}

Config LoadConfig()
{
	const bool exists = QFileInfo::exists(Config::Path());
	Config cfg = Config::Load();
	if (!exists) {
		/* first run inside Spectra: save into Spectra's folders */
		cfg.outputDir = SpectraFolder();
		cfg.originalsDir = QDir(SpectraFolder()).filePath(QStringLiteral("originals"));
		cfg.Save();
	}
	return cfg;
}

std::optional<Definitions> LoadInstalled()
{
	if (!QFileInfo::exists(Config::DefinitionsPath())) {
		return std::nullopt;
	}
	QString error;
	std::optional<Definitions> defs = ReadObx(Config::DefinitionsPath(), &error);
	if (defs) {
		blog(LOG_INFO, "[Obscura] Loaded definitions v%d (%zu channels)", defs->defsVersion,
		     defs->channelStats.size());
	} else {
		blog(LOG_WARNING, "[Obscura] Ignoring installed definitions: %s", error.toUtf8().constData());
	}
	return defs;
}

std::optional<spectra::Image> ReadImage(const QString &path)
{
	QImage q(path);
	if (q.isNull()) {
		return std::nullopt;
	}
	return spectra::censor::FromQImage(q);
}

} // namespace

QString Job::Label() const
{
	if (!srcPath.isEmpty()) {
		return srcPath;
	}
	const QString kind = source == "crop" ? T("Obscura.Job.RegionCapture") : T("Obscura.Job.HotkeyCapture");
	return QStringLiteral("%1 %2").arg(kind, capturedAt.toLocalTime().toString(QStringLiteral("HH:mm:ss")));
}

/* Spectra's in-game notification overlay (the frontend's spectra_notify proc);
 * kind: obscura, upload, screenshot, error, ... */
static void Toast(const char *kind, const QString &title, const QString &text, const QString &key)
{
	const QByteArray t = title.toUtf8(), x = text.toUtf8(), k = key.toUtf8();
	calldata_t cd = {0};
	calldata_set_string(&cd, "kind", kind);
	calldata_set_string(&cd, "title", t.constData());
	calldata_set_string(&cd, "text", x.constData());
	calldata_set_string(&cd, "key", k.constData());
	proc_handler_call(obs_get_proc_handler(), "spectra_notify", &cd);
	calldata_free(&cd);
}

static bool OverlayEnabled()
{
	config_t *profile = obs_frontend_get_profile_config();
	return profile && config_get_bool(profile, "SpectraOverlay", "Enabled");
}

Controller::Controller(QObject *parent) : QObject(parent)
{
	cfg = LoadConfig();
	learner = std::make_unique<Learner>(AppDataDir(), cfg.seedSensitive, LoadInstalled(),
					    cfg.useInstalledDefinitions);
	grabber = std::make_unique<spectra::FrameGrabber>();
	grabber->SetTargetProcess(cfg.targetProcess);
	connect(&watcher, &QFileSystemWatcher::directoryChanged, this, &Controller::OnDirectoryChanged);
}

Controller::~Controller()
{
	Stop();
}

void Controller::Start()
{
	if (worker.joinable()) {
		return;
	}
	stopping = false;
	worker = std::thread([this] { RunWorker(); });
	review = new ReviewWindow(this);
	ApplyConfig();
	/* warm the OCR engine up so the first capture is quick */
	Post([this] {
		QString error;
		if (!EnsureOcr(&error)) {
			OnUi([this, error] { Notify(error, true); });
		}
	});
	if (cfg.autoUpdateDefs) {
		CheckUpdates(false);
	}
	Notify(T("Obscura.Status.Ready"));
}

void Controller::Stop()
{
	{
		std::lock_guard l(mutex);
		if (!worker.joinable()) {
			return;
		}
		stopping = true;
		tasks.clear();
	}
	cv.notify_all();
	worker.join();
	ocr.reset();
	watcher.removePaths(watcher.directories());
	if (review) {
		review->allowClose = true;
		review->close();
		delete review;
	}
}

/* --- worker ------------------------------------------------------------------ */

void Controller::Post(std::function<void()> task)
{
	{
		std::lock_guard l(mutex);
		if (stopping) {
			return;
		}
		tasks.push_back(std::move(task));
	}
	cv.notify_one();
}

void Controller::RunWorker()
{
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); /* keep the game ahead */
#endif
	for (;;) {
		std::function<void()> task;
		{
			std::unique_lock l(mutex);
			cv.wait(l, [this] { return stopping || !tasks.empty(); });
			if (stopping) {
				return;
			}
			task = std::move(tasks.front());
			tasks.pop_front();
		}
		try {
			task();
		} catch (const std::exception &e) {
			const QString msg = QString::fromUtf8(e.what());
			OnUi([this, msg] { Notify(msg, true); });
		}
	}
}

void Controller::OnUi(std::function<void()> fn)
{
	QPointer<Controller> self(this);
	QMetaObject::invokeMethod(
		qApp,
		[self, fn = std::move(fn)] {
			if (self) {
				fn();
			}
		},
		Qt::QueuedConnection);
}

bool Controller::EnsureOcr(QString *error)
{
	if (ocr) {
		return true;
	}
	std::string err;
	spectra::OcrEngine::Options options;
	options.threads = 2;
	ocr = spectra::OcrEngine::Create(options, err);
	if (!ocr && error) {
		*error = QString::fromStdString(err);
	}
	return ocr != nullptr;
}

/* --- configuration ----------------------------------------------------------- */

void Controller::ApplyConfig()
{
	learner->SetSeeds(cfg.seedSensitive);
	learner->SetUseInstalled(cfg.useInstalledDefinitions);
	const QString target = cfg.targetProcess;
	Post([this, target] { grabber->SetTargetProcess(target); });
	RestartWatcher();
}

void Controller::SetPaused(bool value)
{
	paused = value;
	RestartWatcher();
	Notify(paused ? T("Obscura.Status.Paused") : T("Obscura.Status.Ready"));
}

void Controller::RestartWatcher()
{
	if (!watcher.directories().isEmpty()) {
		watcher.removePaths(watcher.directories());
	}
	seenFiles.clear();
	if (paused || !cfg.watchEnabled) {
		return;
	}
	QStringList watching;
	for (const QString &folder : cfg.watchFolders) {
		QDir dir(folder);
		if (!dir.exists()) {
			blog(LOG_WARNING, "[Obscura] Watch folder does not exist: %s", folder.toUtf8().constData());
			continue;
		}
		for (const QString &name : dir.entryList(QDir::Files)) {
			seenFiles.insert(dir.absoluteFilePath(name));
		}
		watching << dir.absolutePath();
	}
	if (!watching.isEmpty()) {
		watcher.addPaths(watching);
		blog(LOG_INFO, "[Obscura] Watching %s", watching.join(", ").toUtf8().constData());
	}
}

void Controller::OnDirectoryChanged(const QString &path)
{
	if (paused) {
		return;
	}
	const QString output = QDir(cfg.outputDir).absolutePath();
	const QString originals = QDir(cfg.originalsDir).absolutePath();
	const QString here = QDir(path).absolutePath();
	if (here.compare(output, Qt::CaseInsensitive) == 0 || here.compare(originals, Qt::CaseInsensitive) == 0) {
		return;
	}
	QDir dir(path);
	for (const QString &name : dir.entryList(QDir::Files)) {
		const QString file = dir.absoluteFilePath(name);
		if (seenFiles.contains(file)) {
			continue;
		}
		seenFiles.insert(file);
		if (!IsImageFile(file) || OurSuffixRe().match(QFileInfo(file).completeBaseName()).hasMatch()) {
			continue;
		}
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (now - recentlySubmitted.value(file, -60000) < 30000) {
			continue;
		}
		recentlySubmitted[file] = now;
		/* wait for the writer to finish (size stable for two polls) on the worker */
		Post([this, file] {
			qint64 last = -1;
			int stable = 0;
			for (int i = 0; i < 80 && stable < 2; i++) {
				QThread::msleep(250);
				QFile f(file);
				const qint64 size = f.open(QIODevice::ReadOnly) ? f.size() : -1;
				stable = size > 0 && size == last ? stable + 1 : 0;
				last = size;
			}
			if (stable < 2) {
				blog(LOG_WARNING, "[Obscura] Gave up waiting for %s to finish writing",
				     file.toUtf8().constData());
				return;
			}
			OnUi([this, file] { SubmitPath(file, QStringLiteral("watch")); });
		});
	}
}

/* --- intake ------------------------------------------------------------------ */

QString Controller::KeepOriginal(const Job &job, const QString &kind, const Config &c)
{
	if (!c.keepOriginals) {
		return {};
	}
	const QString name = QStringLiteral("%1%2_%3_original.png")
				     .arg(c.filenamePrefix, kind,
					  job.capturedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
	QDir().mkpath(c.originalsDir);
	const QString path = UniquePath(QDir(c.originalsDir).filePath(name));
	return spectra::censor::WritePng(path, job.image) ? path : QString();
}

void Controller::Capture()
{
	if (paused) {
		return;
	}
	Toast("obscura", T("Obscura.Toast.Capturing"), QString(), "obscura");
	Notify(T("Obscura.Status.Capturing"), false, true);
	Post([this, c = cfg] {
		auto job = std::make_shared<Job>();
		job->source = QStringLiteral("hotkey");
		job->capturedAt = QDateTime::currentDateTimeUtc();
		/* the game as OBS captures it; the window itself if no capture is hooked */
		if (std::optional<spectra::SourceFrame> frame = grabber->Grab()) {
			job->image = std::move(frame->image);
		} else {
			QString error;
			std::optional<spectra::Image> img = CaptureTargetWindow(c.targetProcess, c.targetTitle, &error);
			if (!img) {
				OnUi([this, error] {
					Notify(T("Obscura.Error.CaptureFailed")
						       .arg(error + " - " + T("Obscura.Error.CheckTarget")),
					       true);
				});
				return;
			}
			job->image = std::move(*img);
		}
		job->originalPath = KeepOriginal(*job, QString(), c);
		Analyse(job, c);
	});
}

void Controller::CaptureRegion()
{
	if (paused) {
		return;
	}
	/* freeze the screen first, so what is dragged over is what gets cropped */
	QTimer::singleShot(150, this, [this] {
		QString error;
		std::optional<spectra::Image> desktop = CaptureDesktop(&error);
		if (!desktop) {
			Notify(T("Obscura.Error.ScreenFailed").arg(error), true);
			return;
		}
		std::optional<spectra::Rect> r = SnipOverlay::SelectRegion(spectra::censor::ToQImage(*desktop));
		if (!r) {
			return;
		}
		auto job = std::make_shared<Job>();
		job->source = QStringLiteral("crop");
		job->capturedAt = QDateTime::currentDateTimeUtc();
		job->uploadAfter = true;
		job->image = spectra::Crop(*desktop, r->x0, r->y0, r->x1, r->y1);
		job->originalPath = KeepOriginal(*job, QStringLiteral("_crop"), cfg);
		Notify(T("Obscura.Status.CheckingCrop").arg(r->x1 - r->x0).arg(r->y1 - r->y0));
		AnalyseAsync(job);
	});
}

void Controller::OpenScreenshots(QWidget *parent)
{
	const QStringList paths = QFileDialog::getOpenFileNames(
		parent, T("Obscura.Open.Title"), QString(),
		QStringLiteral("%1 (*.png *.jpg *.jpeg *.bmp *.webp)").arg(T("Obscura.Open.Images")));
	for (const QString &p : paths) {
		SubmitPath(p, QStringLiteral("open"));
	}
}

void Controller::SubmitPath(const QString &path, const QString &source)
{
	Post([this, path, source, c = cfg] {
		std::optional<spectra::Image> img = ReadImage(path);
		if (!img) {
			const QString name = QFileInfo(path).fileName();
			OnUi([this, name] { Notify(T("Obscura.Error.Unreadable").arg(name), true); });
			return;
		}
		auto job = std::make_shared<Job>();
		job->image = std::move(*img);
		job->source = source;
		job->srcPath = path;
		job->capturedAt = QFileInfo(path).lastModified().toUTC();
		Analyse(job, c);
	});
}

void Controller::AnalyseAsync(const JobPtr &job)
{
	Post([this, job, c = cfg] { Analyse(job, c); });
}

void Controller::Analyse(const JobPtr &job, const Config &c)
{
	/* worker thread */
	QString error;
	if (!EnsureOcr(&error)) {
		OnUi([this, error] { Notify(error, true); });
		return;
	}
	/* a hand-picked crop has no fixed chat region and rarely holds the HUD
	 * clock, so it is searched end to end - a snip of staff chat still gets censored */
	const bool crop = job->source == "crop";
	job->analysis = spectra::Analyze(job->image, *ocr, crop ? spectra::Region{0, 0, 1, 1} : c.chatRegion,
					 c.hudRegion, !crop, crop);
	job->predictions = Predict(*job);
	OnUi([this, job] { OnJobReady(job); });
}

std::vector<Prediction> Controller::Predict(const Job &job) const
{
	std::vector<Features> feats;
	for (const spectra::ChatEntry &e : job.analysis.entries) {
		feats.push_back(EntryFeatures(e));
	}
	return learner->Predict(feats);
}

/* --- review ------------------------------------------------------------------ */

void Controller::OnJobReady(const JobPtr &job)
{
	if (review && review->CurrentJob() == job) { /* re-analysed after a region change */
		review->Load(job, Queued());
		return;
	}
	const spectra::Analysis &a = job->analysis;
	if (job->source == "watch" && a.entries.empty() && !a.unixTs) {
		blog(LOG_INFO, "[Obscura] Ignoring %s: no chat or HUD timestamp", job->srcPath.toUtf8().constData());
		return;
	}
	std::vector<bool> suggested;
	for (const Prediction &p : job->predictions) {
		suggested.push_back(p.Censor());
	}
	if (job->source == "crop") {
		if (!CropNeedsReview(*job)) {
			Finalise(job, suggested, {}, std::nullopt, false, true);
			return;
		}
	} else if (CanAutoApply(*job)) {
		Finalise(job, suggested, {}, a.unixTs, false);
		return;
	}
	Enqueue(job);
}

bool Controller::CropNeedsReview(const Job &job) const
{
	if (cfg.cropReview == "never") {
		return false;
	}
	if (cfg.cropReview == "always") {
		return true;
	}
	for (const Prediction &p : job.predictions) {
		if (p.Censor()) {
			return true;
		}
	}
	return false;
}

bool Controller::CanAutoApply(const Job &job) const
{
	const spectra::Analysis &a = job.analysis;
	if (!cfg.autoApply || learner->Screens() < cfg.autoMinScreens || !a.unixTs || a.entries.empty()) {
		return false;
	}
	for (const Prediction &p : job.predictions) {
		if (p.Confidence() < cfg.autoConfidence) {
			return false;
		}
	}
	return true;
}

void Controller::Enqueue(const JobPtr &job)
{
	queue.push_back(job);
	if (review && !review->CurrentJob()) {
		ShowNext();
	} else {
		if (review) {
			review->SetQueueCount(Queued());
		}
		Notify(T("Obscura.Status.Waiting").arg(Queued()));
	}
	emit queueChanged(Queued());
}

void Controller::ShowNext()
{
	if (!review) {
		return;
	}
	if (queue.empty()) {
		review->Clear();
		emit queueChanged(0);
		return;
	}
	JobPtr job = queue.front();
	queue.pop_front();
	job->predictions = Predict(*job); /* pick up anything learned since it was queued */
	review->Load(job, Queued());
	emit queueChanged(Queued());
}

void Controller::ShowReview()
{
	if (!review) {
		return;
	}
	if (!review->CurrentJob()) {
		ShowNext();
	}
	if (review->CurrentJob()) {
		review->Present();
	} else {
		Notify(T("Obscura.Status.NothingToReview"));
	}
}

void Controller::Finalise(const JobPtr &job, const std::vector<bool> &flags, const std::vector<spectra::Rect> &manual,
			  std::optional<long long> unixTs, bool learn, bool uploadAfter)
{
	const std::vector<spectra::ChatEntry> &entries = job->analysis.entries;
	std::vector<spectra::Rect> rects;
	int censored = 0;
	for (size_t i = 0; i < entries.size() && i < flags.size(); i++) {
		if (flags[i]) {
			censored++;
			rects.insert(rects.end(), entries[i].rects.begin(), entries[i].rects.end());
		}
	}
	rects.insert(rects.end(), manual.begin(), manual.end());

	const spectra::Image out = spectra::Redact(job->image, rects, cfg.fill);
	const QString suffix = TimestampSuffix(unixTs, job->capturedAt, cfg.timestampTz);
	const QString folder = job->source == "open" && cfg.openedToSourceFolder
				       ? QFileInfo(job->srcPath).absolutePath()
				       : cfg.outputDir;
	QString stem = job->srcPath.isEmpty() ? cfg.filenamePrefix : QFileInfo(job->srcPath).completeBaseName();
	if (job->source == "crop") {
		stem += QStringLiteral("_crop");
	}
	QDir().mkpath(folder);
	const QString saved = OutputPath(folder, stem, suffix);
	if (!spectra::censor::WritePng(saved, out)) {
		QMessageBox::critical(nullptr, T("Obscura.Title"), T("Obscura.Error.SaveFailed").arg(saved));
		return;
	}
	if (job->source == "watch" && cfg.keepOriginals && cfg.moveWatchedOriginals &&
	    QFileInfo::exists(job->srcPath)) {
		QDir().mkpath(cfg.originalsDir);
		QFile::rename(job->srcPath,
			      UniquePath(QDir(cfg.originalsDir).filePath(QFileInfo(job->srcPath).fileName())));
	}

	if (learn) {
		std::vector<Features> feats;
		for (const spectra::ChatEntry &e : entries) {
			feats.push_back(EntryFeatures(e));
		}
		Post([this, feats, flags] { learner->AddScreen(feats, flags); });
	}
	lastSaved = saved;
	emit lastSavedChanged(saved);

	QString detail;
	if (!(job->source == "crop" && flags.empty())) {
		detail = T("Obscura.Status.LinesCensored").arg(censored).arg(flags.size());
		if (!unixTs && job->source != "crop") {
			detail += " " + T("Obscura.Status.NoHudTime");
		}
	}
	const QString name = QFileInfo(saved).fileName();
	Toast("obscura", T("Obscura.Toast.Saved"),
	      censored ? T("Obscura.Toast.Censored").arg(censored).arg(flags.size()) : name, "obscura");
	Notify((learn ? T("Obscura.Status.Saved") : T("Obscura.Status.AutoCensored")).arg(name) + detail, false, true);
	blog(LOG_INFO, "[Obscura] %s %s", learn ? "Saved" : "Auto-censored", saved.toUtf8().constData());
	if (uploadAfter || cfg.imgbbAutoUpload) {
		UploadFile(saved);
	}
	if (review && review->CurrentJob() == job) {
		ShowNext();
	}
}

void Controller::Skip(const JobPtr &job)
{
	if (review && review->CurrentJob() == job) {
		ShowNext();
	}
}

void Controller::SetChatRegion(const JobPtr &job, const spectra::Rect &rect)
{
	cfg.chatRegion = spectra::Region::FromPixels(rect, job->image.width, job->image.height);
	cfg.Save();
	AnalyseAsync(job);
}

/* --- imgbb ------------------------------------------------------------------- */

void Controller::UploadLast()
{
	if (!lastSaved.isEmpty()) {
		UploadFile(lastSaved);
	}
}

void Controller::UploadFile(const QString &path)
{
	if (!spectra::censor::LoadImgbbKey()) {
		Notify(T("Obscura.Error.NoKey"), true);
		return;
	}
	Toast("upload", T("Obscura.Toast.Uploading"), QFileInfo(path).fileName(), path);
	Notify(T("Obscura.Status.Uploading").arg(QFileInfo(path).fileName()), false, true);
	const int expiration = cfg.imgbbExpiration;
	Post([this, path, expiration] {
		spectra::censor::UploadResult r = spectra::censor::UploadToImgbb(path, expiration);
		OnUi([this, path, r] {
			if (!r.ok) {
				Toast("error", T("Obscura.Toast.UploadFailed"), r.error, path);
				Notify(T("Obscura.Error.UploadFailed").arg(r.error), true, true);
				return;
			}
			const QString url = r.displayUrl.isEmpty() ? r.url : r.displayUrl;
			if (cfg.imgbbCopyLink) {
				QApplication::clipboard()->setText(url);
			}
			if (cfg.imgbbOpenLink) {
				QDesktopServices::openUrl(QUrl(url));
			}
			Toast("upload",
			      T(cfg.imgbbCopyLink ? "Obscura.Toast.UploadedCopied" : "Obscura.Toast.Uploaded"), url,
			      path);
			Notify(T(cfg.imgbbCopyLink ? "Obscura.Status.UploadedCopied" : "Obscura.Status.Uploaded")
				       .arg(url),
			       false, true);
		});
	});
}

void Controller::OnObsScreenshot(const QString &path)
{
	if (!cfg.imgbbUploadObsShots || path.isEmpty()) {
		return;
	}
	static const QStringList uploadable = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};
	if (!uploadable.contains(QFileInfo(path).suffix().toLower())) {
		/* HDR screenshots are JPEG XR, which imgbb does not take */
		blog(LOG_INFO, "[Obscura] Not uploading %s: imgbb does not accept this format",
		     path.toUtf8().constData());
		return;
	}
	UploadFile(path);
}

/* --- definitions ------------------------------------------------------------- */

void Controller::CheckUpdates(bool manual)
{
	const QString repo = cfg.distRepo;
	const int current = learner->InstalledVersion();
	const bool pre = cfg.includePrereleaseDefs;
	const bool autoInstall = cfg.autoUpdateDefs || manual;
	Post([this, repo, current, pre, manual, autoInstall] {
		QString error;
		std::optional<QByteArray> json = spectra::censor::HttpGet(
			QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(repo), &error,
			15000);
		if (!json) {
			blog(LOG_WARNING, "[Obscura] Definitions update check failed: %s", error.toUtf8().constData());
			if (manual) {
				OnUi([this, error] { Notify(T("Obscura.Error.UpdateCheck").arg(error), true); });
			}
			return;
		}
		std::optional<DefsUpdate> update = FindDefsUpdate(*json, current, pre);
		if (!update) {
			if (manual) {
				OnUi([this] { Notify(T("Obscura.Status.UpToDate")); });
			}
			return;
		}
		if (autoInstall) {
			InstallDefinitions(update->url);
		} else {
			const int v = update->version;
			OnUi([this, v] { Notify(T("Obscura.Status.DefsAvailable").arg(v)); });
		}
	});
}

void Controller::InstallDefinitions(const QString &url)
{
	/* worker thread: download, verify against the release key, then install */
	QString error;
	std::optional<QByteArray> blob = spectra::censor::HttpGet(url, &error, 60000);
	std::optional<Definitions> defs = blob ? LoadsObx(*blob, &error) : std::nullopt;
	if (!defs) {
		OnUi([this, error] { Notify(T("Obscura.Error.DefsFailed").arg(error), true); });
		return;
	}
	QFile out(Config::DefinitionsPath() + ".part");
	if (!out.open(QIODevice::WriteOnly) || out.write(*blob) != blob->size()) {
		OnUi([this] { Notify(T("Obscura.Error.DefsFailed").arg(T("Obscura.Error.WriteDefs")), true); });
		return;
	}
	out.close();
	QFile::remove(Config::DefinitionsPath());
	QFile::rename(Config::DefinitionsPath() + ".part", Config::DefinitionsPath());
	learner->SetInstalled(defs);
	const int version = defs->defsVersion;
	const int channels = (int)defs->channelStats.size();
	OnUi([this, version, channels] { Notify(T("Obscura.Status.DefsInstalled").arg(version).arg(channels)); });
}

bool Controller::ExportDefinitions(const QString &path, QString *message)
{
	const QString keyPath = QDir(AppDataDir()).filePath(QStringLiteral("signing_key.pem"));
	QFile key(keyPath);
	if (!key.open(QIODevice::ReadOnly)) {
		*message = T("Obscura.Export.NoKey").arg(keyPath);
		return false;
	}
	const int version = learner->InstalledVersion() + 1;
	Definitions defs = learner->ExportDefinitions(version);
	QString error;
	if (!WriteObx(path, defs, key.readAll(), &error)) {
		*message = T("Obscura.Export.Failed").arg(error);
		return false;
	}
	*message = T("Obscura.Export.Done").arg(version).arg(path).arg(defs.channelStats.size()).arg(defs.seeds.size());
	return true;
}

/* --- misc -------------------------------------------------------------------- */

void Controller::OpenSettings(QWidget *parent)
{
	SettingsDialog dialog(this, parent);
	if (dialog.exec() == QDialog::Accepted) {
		cfg.Save();
		ApplyConfig();
	}
}

void Controller::OpenFolder(const QString &folder)
{
	QDir().mkpath(folder);
	QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void Controller::Notify(const QString &message, bool error, bool toasted)
{
	if (error && !toasted) {
		Toast("error", T("Obscura.Title"), message, "obscura");
		toasted = true;
	}
	status = message;
	emit statusChanged(message);
	blog(error ? LOG_WARNING : LOG_INFO, "[Obscura] %s", message.toUtf8().constData());
	/* a balloon from Spectra's tray icon, when it has one */
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QSystemTrayIcon *tray = main ? main->findChild<QSystemTrayIcon *>() : nullptr;
	if (tray && tray->isVisible() && (error || !main->isActiveWindow()) && !(toasted && OverlayEnabled())) {
		tray->showMessage(T("Obscura.Title"), message,
				  error ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 4000);
	}
}

} // namespace obscura
