#include "LoopRecorder.hpp"
#include "ClipExport.hpp"

#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QFileInfo>
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
#endif

#define LOOP_SECTION "SpectraLoop"

static constexpr int PROCESS_POLL_MS = 3000;
/* Polls a matching process must be gone before an auto-started loop stops,
 * so launcher/game process hand-offs don't cut the recording. */
static constexpr int AUTO_STOP_POLLS = 4;
static constexpr int SPLIT_TIMEOUT_MS = 5000;

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

LoopRecorder::LoopRecorder(OBSBasic *main_) : QObject(main_), main(main_)
{
	processTimer.setInterval(PROCESS_POLL_MS);
	connect(&processTimer, &QTimer::timeout, this, &LoopRecorder::CheckProcesses);

	splitTimeout.setSingleShot(true);
	splitTimeout.setInterval(SPLIT_TIMEOUT_MS);
	/* No split happened, so the newest segment ends before the request */
	connect(&splitTimeout, &QTimer::timeout, this, [this]() { ProcessPendingClips(false); });

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

void LoopRecorder::SettingsChanged()
{
	if (AutoStartEnabled() && !ProcessPatterns().isEmpty()) {
		processTimer.start();
		QTimer::singleShot(0, this, &LoopRecorder::CheckProcesses);
	} else {
		processTimer.stop();
	}
}

/* ------------------------------------------------------------------------- */
/* Start / stop                                                              */

bool LoopRecorder::Start(const QString &label_)
{
	if (Active()) {
		return true;
	}

	QString dir = LoopDirectory();
	if (!QDir().mkpath(dir)) {
		emit clipFailed(QTStr("Spectra.Loop.Error.Folder").arg(dir));
		return false;
	}

	label = label_;
	autoStarted = false;
	currentSegment.clear();
	sessionSegments.clear();

	return main->StartLoopRecording(dir, SegmentSeconds());
}

void LoopRecorder::Stop()
{
	/* Don't let a still-running game restart a loop that was just stopped;
	 * auto-start resumes once the game has exited. */
	suppressAutoStart = true;
	autoStarted = false;
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
	EnforceQuota();
	emit activeChanged(true);
}

void LoopRecorder::OnStopped(int code, const QString &error)
{
	if (!currentSegment.isEmpty()) {
		sessionSegments << currentSegment;
		currentSegment.clear();
	}

	if (code != OBS_OUTPUT_SUCCESS) {
		blog(LOG_WARNING, "[Spectra] Loop recording stopped with code %d: %s", code, QT_TO_UTF8(error));
	} else {
		blog(LOG_INFO, "[Spectra] Loop recording stopped");
	}

	autoStarted = false;
	ProcessPendingClips();
	emit activeChanged(false);
}

void LoopRecorder::OnFileChanged(const QString &nextFile)
{
	if (!currentSegment.isEmpty()) {
		sessionSegments << currentSegment;
	}
	currentSegment = nextFile;

	EnforceQuota();

	if (!pendingClips.empty()) {
		splitTimeout.stop();
		ProcessPendingClips();
	}
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
	QStringList patterns = ProcessPatterns();
	if (patterns.isEmpty()) {
		return;
	}

	QString matched;
	const QStringList running = RunningProcessNames();
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
		if (!Active() && AutoStartEnabled() && !suppressAutoStart) {
			blog(LOG_INFO, "[Spectra] Detected process matching '%s', starting loop recording",
			     QT_TO_UTF8(matched));
			if (Start(LabelForPattern(matched))) {
				autoStarted = true;
			} else {
				suppressAutoStart = true;
			}
		}
		return;
	}

	suppressAutoStart = false;
	if (autoStarted && Active() && AutoStopEnabled()) {
		if (++missingPolls >= AUTO_STOP_POLLS) {
			blog(LOG_INFO, "[Spectra] Game process exited, stopping loop recording");
			Stop();
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
			total -= size;
			sessionSegments.removeIf([&](const QString &s) {
				return QDir::cleanPath(s).compare(path, Qt::CaseInsensitive) == 0;
			});
		}
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
