#include "LoopRecorder.hpp"
#include "ClipExport.hpp"
#include "SpectraSpeakerTracks.hpp"

#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>

#include <algorithm>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <util/windows/window-helpers.h>
#endif

#define LOOP_SECTION "SpectraLoop"

static constexpr int PROCESS_POLL_MS = 3000;
/* Polls a matching process must be gone before an auto-started loop stops,
 * so launcher/game process hand-offs don't cut the recording. */
static constexpr int AUTO_STOP_POLLS = 4;
static constexpr int SPLIT_TIMEOUT_MS = 5000;
static constexpr int STATUS_UPDATE_MS = 2000;
/* Wait before trying again after an automatic start failed */
static constexpr int AUTO_START_RETRY_SEC = 30;

/* The size of a file as it is now. A folder listing (and QFileInfo, which
 * may read it) shows the size from when the file was last closed, which is
 * 0 for a segment still being written. */
static qint64 FileSizeOnDisk(const QString &path)
{
#ifdef _WIN32
	HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
				  FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				  OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return -1;
	}
	LARGE_INTEGER size;
	const bool ok = GetFileSizeEx(file, &size);
	CloseHandle(file);
	return ok ? (qint64)size.QuadPart : -1;
#else
	QFileInfo info(path);
	return info.exists() ? info.size() : -1;
#endif
}

static QStringList RunningProcessNames()
{
	QStringList names;
#ifdef _WIN32
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return names;
	}

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			names << QString::fromWCharArray(entry.szExeFile);
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
#endif
	return names;
}

/* Apps that go fullscreen without being games, ignored by "any fullscreen
 * application" (they can still be added to the list explicitly). */
static const char *fullscreenIgnored[] = {
	"explorer.exe",
	"ApplicationFrameHost.exe",
	"LockApp.exe",
	"SearchHost.exe",
	"ShellExperienceHost.exe",
	"StartMenuExperienceHost.exe",
	"TextInputHost.exe",
	"chrome.exe",
	"msedge.exe",
	"firefox.exe",
	"opera.exe",
	"brave.exe",
	"vivaldi.exe",
	"vlc.exe",
	"mpc-hc64.exe",
	"mpc-be64.exe",
	"PotPlayerMini64.exe",
	"Netflix.exe",
	"POWERPNT.EXE",
	"obs64.exe",
};

/* The exe of the foreground window if it covers its whole monitor, e.g. a
 * game in exclusive or borderless fullscreen. */
static QString FullscreenAppExe()
{
#ifdef _WIN32
	HWND hwnd = GetForegroundWindow();
	if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == GetShellWindow() ||
	    hwnd == GetDesktopWindow()) {
		return QString();
	}

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId()) {
		return QString();
	}

	/* A maximized window with a title bar also covers the monitor when
	 * the taskbar auto-hides, but isn't fullscreen */
	if (IsZoomed(hwnd) && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {
		return QString();
	}

	RECT rect;
	MONITORINFO monitor = {};
	monitor.cbSize = sizeof(monitor);
	if (!GetWindowRect(hwnd, &rect) ||
	    !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
		return QString();
	}
	const RECT &m = monitor.rcMonitor;
	if (rect.left > m.left || rect.top > m.top || rect.right < m.right || rect.bottom < m.bottom) {
		return QString();
	}

	struct dstr exe = {0};
	if (!ms_get_window_exe(&exe, hwnd)) {
		return QString();
	}
	QString exeName = QString::fromUtf8(exe.array);
	dstr_free(&exe);

	for (const char *ignored : fullscreenIgnored) {
		if (exeName.compare(QLatin1String(ignored), Qt::CaseInsensitive) == 0) {
			return QString();
		}
	}
	return exeName;
#else
	return QString();
#endif
}

LoopRecorder::LoopRecorder(OBSBasic *main_)
	: QObject(main_),
	  main(main_),
	  capture(new LoopCapture(main_)),
	  starling(new StarlingLink(main_))
{
	captureTimer.setInterval(PROCESS_POLL_MS);
	connect(&captureTimer, &QTimer::timeout, this, &LoopRecorder::UpdateCapture);

	processTimer.setInterval(PROCESS_POLL_MS);
	connect(&processTimer, &QTimer::timeout, this, &LoopRecorder::CheckProcesses);

	statusTimer.setInterval(STATUS_UPDATE_MS);
	connect(&statusTimer, &QTimer::timeout, this, [this]() {
		CheckWriting();
		emit recordingStatusChanged();
	});

	splitTimeout.setSingleShot(true);
	splitTimeout.setInterval(SPLIT_TIMEOUT_MS);
	/* No split happened, so the newest segment ends before the request */
	connect(&splitTimeout, &QTimer::timeout, this, [this]() { ProcessPendingClips(false); });

	/* Opens armed unless turned off in the settings */
	armed = AutoStartEnabled();

	SettingsChanged();
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */

static QString ConfigString(OBSBasic *main, const char *name)
{
	const char *value = config_get_string(main->Config(), LOOP_SECTION, name);
	return QString::fromUtf8(value ? value : "").trimmed();
}

QString LoopRecorder::LoopDirectory() const
{
	QString dir = ConfigString(main, "Path");
	if (dir.isEmpty()) {
		dir = QDir(QString::fromUtf8(main->GetCurrentOutputPath())).filePath("Spectra Loop");
	}
	return QDir::cleanPath(dir);
}

QString LoopRecorder::ClipsDirectory() const
{
	QString dir = ConfigString(main, "ClipsPath");
	if (dir.isEmpty()) {
		dir = QDir(QString::fromUtf8(main->GetCurrentOutputPath())).filePath("Clips");
	}
	return QDir::cleanPath(dir);
}

quint64 LoopRecorder::QuotaBytes() const
{
	quint64 gb = config_get_uint(main->Config(), LOOP_SECTION, "QuotaGB");
	return std::max<quint64>(gb, 1) * 1024ull * 1024ull * 1024ull;
}

int LoopRecorder::SegmentSeconds() const
{
	return std::clamp((int)config_get_int(main->Config(), LOOP_SECTION, "SegmentSec"), 10, 3600);
}

int LoopRecorder::DefaultClipSeconds() const
{
	return std::max((int)config_get_int(main->Config(), LOOP_SECTION, "ClipSec"), 5);
}

bool LoopRecorder::AutoStartEnabled() const
{
	return config_get_bool(main->Config(), LOOP_SECTION, "AutoStart");
}

bool LoopRecorder::AutoStopEnabled() const
{
	return config_get_bool(main->Config(), LOOP_SECTION, "AutoStop");
}

bool LoopRecorder::AutoCaptureEnabled() const
{
	return config_get_bool(main->Config(), LOOP_SECTION, "AutoCapture");
}

bool LoopRecorder::FitToCanvasEnabled() const
{
	return config_get_bool(main->Config(), LOOP_SECTION, "FitToCanvas");
}

bool LoopRecorder::StarlingVoiceEnabled() const
{
	return config_get_bool(main->Config(), LOOP_SECTION, "StarlingVoice");
}

bool LoopRecorder::AnyFullscreenEnabled() const
{
	return config_get_bool(main->Config(), LOOP_SECTION, "AnyFullscreen");
}

void LoopRecorder::FitGameToCanvas()
{
	if (FitToCanvasEnabled()) {
		capture->FitGameToCanvas(ActivePatterns());
	}
}

void LoopRecorder::ResetCapture()
{
	capture->RemoveManagedSources();
	if (Active()) {
		UpdateCapture();
	}
}

void LoopRecorder::UpdateCapture()
{
	capture->SetFitOnHook(FitToCanvasEnabled());
	if (AutoCaptureEnabled()) {
		capture->Update(ActivePatterns());
	} else {
		capture->Reset();
	}
	/* Captures and the Starling voice come and go while recording */
	if (SpectraSpeakerTracks::Enabled(main->Config())) {
		SpectraSpeakerTracks::Apply();
	}
}

QStringList LoopRecorder::ProcessPatterns() const
{
	QStringList patterns;
	for (const QString &p : ConfigString(main, "Processes").split(QRegularExpression("[,;]"))) {
		if (!p.trimmed().isEmpty()) {
			patterns << p.trimmed();
		}
	}
	return patterns;
}

QStringList LoopRecorder::ActivePatterns() const
{
	QStringList patterns = ProcessPatterns();
	if (!fullscreenExe.isEmpty()) {
		patterns << fullscreenExe;
	}
	return patterns;
}

void LoopRecorder::SettingsChanged()
{
	starling->SetEnabled(StarlingVoiceEnabled());

	if (!AnyFullscreenEnabled()) {
		fullscreenExe.clear();
	}

	if (armed && (!ProcessPatterns().isEmpty() || AnyFullscreenEnabled())) {
		processTimer.start();
		QTimer::singleShot(0, this, &LoopRecorder::CheckProcesses);
	} else {
		processTimer.stop();
	}
}

/* ------------------------------------------------------------------------- */
/* Start / stop                                                              */

void LoopRecorder::SetArmed(bool arm)
{
	if (armed == arm) {
		return;
	}

	armed = arm;
	suppressAutoStart = false;
	missingPolls = 0;
	blog(LOG_INFO, "[Spectra] Loop recording %s", armed ? "armed" : "disarmed");

	if (!armed) {
		autoStarted = false;
		if (Active()) {
			stopCause = StopCause::User;
			main->StopLoopRecording();
		}
	}

	SettingsChanged();
	emit armedChanged(armed);
}

void LoopRecorder::HoldStart(bool hold)
{
	const bool wasHeld = StartHeld();
	startHolds = std::max(startHolds + (hold ? 1 : -1), 0);
	if (StartHeld() == wasHeld) {
		return;
	}

	if (StartHeld()) {
		blog(LOG_INFO, "[Spectra] Loop recording on hold until setup is finished");
	} else {
		blog(LOG_INFO, "[Spectra] Loop recording no longer on hold");
		/* Starts right away if the game is already running */
		SettingsChanged();
	}
}

bool LoopRecorder::Start(const QString &label_)
{
	if (Active()) {
		return true;
	}

	if (StartHeld()) {
		lastStartError = QTStr("Spectra.Loop.Error.OnHold");
		blog(LOG_INFO, "[Spectra] Loop recording not started: %s", QT_TO_UTF8(lastStartError));
		return false;
	}

	QString dir = LoopDirectory();
	if (!QDir().mkpath(dir)) {
		lastStartError = QTStr("Spectra.Loop.Error.Folder").arg(dir);
		blog(LOG_WARNING, "[Spectra] Loop recording not started: %s", QT_TO_UTF8(lastStartError));
		emit clipFailed(lastStartError);
		return false;
	}

	label = label_;
	autoStarted = false;
	currentSegment.clear();
	sessionSegments.clear();

	/* Put the game on screen before the first frame is recorded */
	UpdateCapture();

	QString error;
	if (!main->StartLoopRecording(dir, SegmentSeconds(), &error)) {
		lastStartError = error;
		blog(LOG_WARNING, "[Spectra] Loop recording not started: %s", QT_TO_UTF8(error));
		return false;
	}
	return true;
}

void LoopRecorder::Stop(StopCause cause)
{
	/* Don't let a still-running game restart a loop that was just stopped;
	 * auto-start resumes once the game has exited. */
	suppressAutoStart = true;
	autoStarted = false;
	if (Active()) {
		stopCause = cause;
	}
	main->StopLoopRecording();
}

bool LoopRecorder::Active() const
{
	return main->LoopRecordingActive();
}

void LoopRecorder::OnStarted()
{
	blog(LOG_INFO, "[Spectra] Loop recording started in '%s' (quota %llu GB, %d s segments)",
	     QT_TO_UTF8(LoopDirectory()), QuotaBytes() / (1024ull * 1024ull * 1024ull), SegmentSeconds());
	lastStartError.clear();
	startedAt = QDateTime::currentDateTime();
	stopCause = StopCause::Unexpected;
	stalled = false;
	diskSize = -1;
	diskSizeChanged = startedAt;
	EnforceQuota();
	FitGameToCanvas();
	captureTimer.start();
	statusTimer.start();
	emit activeChanged(true);
	emit recordingStatusChanged();
}

void LoopRecorder::OnStopped(int code, const QString &error)
{
	if (!currentSegment.isEmpty()) {
		sessionSegments << currentSegment;
		currentSegment.clear();
	}

	lastStopCause = code == OBS_OUTPUT_SUCCESS ? stopCause : StopCause::Unexpected;
	stopCause = StopCause::Unexpected;
	stalled = false;

	if (code != OBS_OUTPUT_SUCCESS) {
		blog(LOG_WARNING, "[Spectra] Loop recording stopped with code %d: %s", code, QT_TO_UTF8(error));
	} else if (lastStopCause == StopCause::Unexpected) {
		blog(LOG_WARNING, "[Spectra] Loop recording stopped, not by the user or the game exiting");
	} else {
		blog(LOG_INFO, "[Spectra] Loop recording stopped");
	}

	autoStarted = false;
	captureTimer.stop();
	statusTimer.stop();
	capture->Reset();
	ProcessPendingClips();
	emit activeChanged(false);
	emit segmentsChanged();
	emit recordingStatusChanged();
}

void LoopRecorder::OnFileChanged(const QString &nextFile)
{
	if (!currentSegment.isEmpty()) {
		sessionSegments << currentSegment;
	}
	currentSegment = nextFile;
	segmentStartBytes = main->LoopRecordingTotalBytes();
	/* The watchdog follows the new file; a stall carries over until it grows */
	diskSize = -1;
	diskSizeChanged = QDateTime::currentDateTime();

	EnforceQuota();

	if (!pendingClips.empty()) {
		splitTimeout.stop();
		ProcessPendingClips();
	}

	emit segmentsChanged();
	emit recordingStatusChanged();
}

void LoopRecorder::CheckWriting()
{
	if (!Active() || currentSegment.isEmpty()) {
		return;
	}

	const QDateTime now = QDateTime::currentDateTime();
	const qint64 size = FileSizeOnDisk(currentSegment);
	if (size >= 0 && size != diskSize) {
		diskSize = size;
		diskSizeChanged = now;
		if (stalled) {
			stalled = false;
			blog(LOG_INFO, "[Spectra] Loop recording is writing to '%s' again", QT_TO_UTF8(currentSegment));
			emit writingResumed();
		}
		return;
	}

	if (!stalled && diskSizeChanged.secsTo(now) >= StallWarnSeconds) {
		stalled = true;
		if (size < 0) {
			blog(LOG_WARNING, "[Spectra] Loop recording: '%s' is missing", QT_TO_UTF8(currentSegment));
		} else {
			blog(LOG_WARNING, "[Spectra] Loop recording: '%s' has not grown for %lld s (%lld bytes)",
			     QT_TO_UTF8(currentSegment), diskSizeChanged.secsTo(now), size);
		}
		emit writingStalled();
	}
}

QString LoopRecorder::RecordingStatusText() const
{
	if (!Active()) {
		return QString();
	}

	const QLocale locale;
	auto size = [&](quint64 bytes) {
		return locale.formattedDataSize((qint64)bytes, 1, QLocale::DataSizeTraditionalFormat);
	};

	const quint64 total = main->LoopRecordingTotalBytes();
	QStringList lines;
	if (!currentSegment.isEmpty()) {
		const quint64 segment = total > segmentStartBytes ? total - segmentStartBytes : 0;
		lines << QTStr("Spectra.Loop.Status.Writing")
				 .arg(QDir::toNativeSeparators(QDir::cleanPath(currentSegment)), size(segment));
	}
	lines << QTStr("Spectra.Loop.Status.Session").arg(size(total), startedAt.toString(QStringLiteral("HH:mm")));
	if (stalled) {
		lines << QTStr("Spectra.Loop.Status.Stalled").arg(diskSizeChanged.toString(QStringLiteral("HH:mm:ss")));
	}
	return lines.join('\n');
}

/* ------------------------------------------------------------------------- */
/* Game detection                                                            */

QString LoopRecorder::LabelForPattern(const QString &pattern) const
{
	QString name = pattern;
	name.remove(QRegularExpression("[*?]"));
	name.remove(QRegularExpression("\\.exe$", QRegularExpression::CaseInsensitiveOption));
	name = name.trimmed();
	return name.isEmpty() ? QStringLiteral("Clip") : name;
}

void LoopRecorder::CheckProcesses()
{
	const QStringList running = RunningProcessNames();

	if (AnyFullscreenEnabled()) {
		/* Remember the fullscreen app until it exits, so alt-tabbing out of
		 * it doesn't stop the recording. */
		if (!fullscreenExe.isEmpty() && !running.contains(fullscreenExe, Qt::CaseInsensitive)) {
			fullscreenExe.clear();
		}
		if (fullscreenExe.isEmpty() && !Active()) {
			fullscreenExe = FullscreenAppExe();
			if (!fullscreenExe.isEmpty()) {
				blog(LOG_INFO, "[Spectra] Fullscreen application detected: %s",
				     QT_TO_UTF8(fullscreenExe));
			}
		}
	}

	QStringList patterns = ActivePatterns();
	if (patterns.isEmpty()) {
		return;
	}

	QString matched;
	for (const QString &pattern : patterns) {
		QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
				      QRegularExpression::CaseInsensitiveOption);
		for (const QString &name : running) {
			if (re.match(name).hasMatch()) {
				matched = pattern;
				break;
			}
		}
		if (!matched.isEmpty()) {
			break;
		}
	}

	if (!matched.isEmpty()) {
		missingPolls = 0;
		bool waiting = retryAutoStartAt.isValid() && QDateTime::currentDateTime() < retryAutoStartAt;
		if (!Active() && armed && !suppressAutoStart && !waiting && !StartHeld()) {
			blog(LOG_INFO, "[Spectra] Detected process matching '%s', starting loop recording",
			     QT_TO_UTF8(matched));
			if (Start(LabelForPattern(matched))) {
				autoStarted = true;
				retryAutoStartAt = QDateTime();
			} else {
				/* Keep trying while the game runs instead of giving
				 * up until it exits; the cause may be temporary (e.g.
				 * a settings dialog holding the outputs). */
				retryAutoStartAt = QDateTime::currentDateTime().addSecs(AUTO_START_RETRY_SEC);
				blog(LOG_WARNING, "[Spectra] Retrying loop recording in %d s", AUTO_START_RETRY_SEC);
			}
		}
		return;
	}

	suppressAutoStart = false;
	retryAutoStartAt = QDateTime();
	if (autoStarted && Active() && AutoStopEnabled()) {
		if (++missingPolls >= AUTO_STOP_POLLS) {
			blog(LOG_INFO, "[Spectra] Game process exited, stopping loop recording");
			Stop(StopCause::GameExited);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Quota                                                                     */

quint64 LoopRecorder::UsedBytes() const
{
	quint64 total = 0;
	QDir dir(LoopDirectory());
	for (const QFileInfo &fi : dir.entryInfoList({"*.mkv"}, QDir::Files)) {
		total += (quint64)fi.size();
	}
	return total;
}

void LoopRecorder::EnforceQuota()
{
	QDir dir(LoopDirectory());
	/* Segment names are timestamps, so name order is age order. */
	QFileInfoList files = dir.entryInfoList({"*.mkv"}, QDir::Files, QDir::Name);

	quint64 total = 0;
	for (const QFileInfo &fi : files) {
		total += (quint64)fi.size();
	}

	const quint64 quota = QuotaBytes();
	const QString current = QDir::cleanPath(currentSegment);
	bool removed = false;

	for (const QFileInfo &fi : files) {
		if (total <= quota) {
			break;
		}
		QString path = QDir::cleanPath(fi.absoluteFilePath());
		if (path.compare(current, Qt::CaseInsensitive) == 0 || lockedSegments.contains(path)) {
			continue;
		}
		quint64 size = (quint64)fi.size();
		if (QFile::remove(path)) {
			removed = true;
			total -= size;
			sessionSegments.removeIf([&](const QString &s) {
				return QDir::cleanPath(s).compare(path, Qt::CaseInsensitive) == 0;
			});
		}
	}

	if (removed) {
		emit segmentsChanged();
	}
}

void LoopRecorder::Lock(const QStringList &files)
{
	for (const QString &f : files) {
		lockedSegments[QDir::cleanPath(f)]++;
	}
}

void LoopRecorder::Unlock(const QStringList &files)
{
	for (const QString &f : files) {
		QString key = QDir::cleanPath(f);
		if (--lockedSegments[key] <= 0) {
			lockedSegments.remove(key);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Clipping                                                                  */

void LoopRecorder::ClipLast(int seconds)
{
	if (seconds <= 0) {
		seconds = DefaultClipSeconds();
	}

	if (!Active() && sessionSegments.isEmpty()) {
		emit clipFailed(QTStr("Spectra.Loop.Error.NothingRecorded"));
		return;
	}

	emit clipStarted(seconds);
	pendingClips.push_back({seconds, QDateTime::currentDateTime()});

	if (Active()) {
		/* Finalize the segment being written so the clip reaches "now". The
		 * clip is exported once the output reports the new segment. */
		if (main->SplitLoopRecording()) {
			splitTimeout.start();
			return;
		}
	}

	ProcessPendingClips();
}

void LoopRecorder::ProcessPendingClips(bool segmentJustFinalized)
{
	std::vector<PendingClip> clips;
	clips.swap(pendingClips);
	/* The segment was finalized at the first keyframe after the request, so
	 * it runs slightly past the moment the clip was asked for. */
	QDateTime finalized = QDateTime::currentDateTime();
	for (const PendingClip &clip : clips) {
		double lag = segmentJustFinalized ? std::max<qint64>(clip.requested.msecsTo(finalized), 0) / 1000.0
						  : 0.0;
		ExportClip(clip.seconds, clip.requested, lag);
	}
}

void LoopRecorder::ExportClip(int seconds, const QDateTime &requested, double lagSeconds)
{
	if (sessionSegments.isEmpty()) {
		emit clipFailed(QTStr("Spectra.Loop.Error.NothingRecorded"));
		return;
	}

	/* Hold enough of the newest segments to cover the clip, plus slack for
	 * short segments created by manual splits. */
	int count = std::min<int>((int)sessionSegments.size(), (int)(seconds + lagSeconds) / SegmentSeconds() + 3);
	QStringList candidates = sessionSegments.mid(sessionSegments.size() - count);
	Lock(candidates);

	QString clipsDir = ClipsDirectory();
	QDir().mkpath(clipsDir);
	QString baseName = QStringLiteral("%1 %2").arg(label.isEmpty() ? QStringLiteral("Clip") : label,
						       requested.toString("yyyy-MM-dd HH-mm-ss"));
	QString outBase = QDir(clipsDir).filePath(baseName);

	QPointer<LoopRecorder> self(this);
	std::thread([self, candidates, seconds, lagSeconds, outBase]() {
		std::vector<std::string> segs;
		double covered = 0.0;
		const double needed = seconds + lagSeconds;
		/* Walk back from the newest segment until the clip is covered. */
		for (qsizetype i = candidates.size() - 1; i >= 0 && covered < needed; i--) {
			std::string path = candidates[i].toStdString();
			double d = ClipExport::ProbeDuration(path);
			if (d <= 0.0) {
				continue;
			}
			segs.insert(segs.begin(), path);
			covered += d;
		}

		std::string error;
		QString outPath;
		bool ok = false;
		if (!segs.empty()) {
			double end = std::max(0.0, covered - lagSeconds);
			double start = std::max(0.0, end - seconds);
			for (const char *ext : {".mp4", ".mkv"}) {
				outPath = outBase + ext;
				for (int n = 2; QFileInfo::exists(outPath); n++) {
					outPath = QStringLiteral("%1 (%2)%3").arg(outBase).arg(n).arg(ext);
				}
				ok = ClipExport::Export(segs, start, end, outPath.toStdString(), error);
				if (ok) {
					break;
				}
				blog(LOG_WARNING, "[Spectra] Clip export to %s failed: %s", ext, error.c_str());
			}
		} else {
			error = "No readable segments";
		}

		QString qerror = QString::fromStdString(error);
		QMetaObject::invokeMethod(
			qApp,
			[self, candidates, ok, outPath, qerror]() {
				if (!self) {
					return;
				}
				self->Unlock(candidates);
				if (ok) {
					blog(LOG_INFO, "[Spectra] Saved clip '%s'", QT_TO_UTF8(outPath));
					emit self->clipSaved(outPath);
				} else {
					emit self->clipFailed(qerror);
				}
				self->EnforceQuota();
			},
			Qt::QueuedConnection);
	}).detach();
}
