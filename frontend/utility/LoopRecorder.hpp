#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include "LoopCapture.hpp"
#include "StarlingLink.hpp"

#include <vector>

class OBSBasic;

/*
 * Spectra loop recording: continuously records the program output to a
 * folder of short Matroska segments, deletes the oldest segments to stay
 * under a disk quota, and turns the last N seconds into a single portable
 * clip on request. Optionally starts and stops itself when a matching game
 * process (e.g. "FiveM*") or any fullscreen application starts and exits.
 */
class LoopRecorder : public QObject {
	Q_OBJECT

public:
	explicit LoopRecorder(OBSBasic *main);

	/* Settings (profile config, section "SpectraLoop") */
	QString LoopDirectory() const;
	QString ClipsDirectory() const;
	quint64 QuotaBytes() const;
	int SegmentSeconds() const;
	int DefaultClipSeconds() const;
	bool AutoStartEnabled() const;
	bool AutoStopEnabled() const;
	bool AutoCaptureEnabled() const;
	bool FitToCanvasEnabled() const;
	bool AnyFullscreenEnabled() const;
	/* Record Starling's converted voice instead of the mic while it runs */
	bool StarlingVoiceEnabled() const;
	QStringList ProcessPatterns() const;
	/* The configured patterns plus the detected fullscreen application */
	QStringList ActivePatterns() const;

	LoopCapture *Capture() const { return capture; }
	StarlingLink *Starling() const { return starling; }

	/* Recreates Spectra's capture sources with default settings */
	void ResetCapture();

	/* Fits the game's capture to the canvas, keeping its aspect ratio,
	 * when enabled in the loop recording settings */
	void FitGameToCanvas();

	/* Armed: loop recording starts by itself when a game process matching
	 * the patterns runs, and stops again when it exits. */
	void SetArmed(bool armed);
	bool Armed() const { return armed; }

	/* While held (the setup wizard is open) the loop doesn't start, by
	 * itself or by hand. Holds nest; the last release checks for the game
	 * again right away. */
	void HoldStart(bool hold);
	bool StartHeld() const { return startHolds > 0; }

	/* Why the loop stopped, for telling the user */
	enum class StopCause { Unexpected, User, GameExited };

	bool Start(const QString &label = QString());
	void Stop(StopCause cause = StopCause::User);
	bool Active() const;

	/* Why the last loop recording stopped (read after it stopped) */
	StopCause LastStopCause() const { return lastStopCause; }

	/* True while the segment being written hasn't grown on disk for
	 * StallWarnSeconds, e.g. the muxer hung or the drive went away.
	 * Matroska writes a cluster at least every ~5 s. */
	static constexpr int StallWarnSeconds = 20;
	bool WritingStalled() const { return stalled; }

	/* Saves the last `seconds` of the loop as one file in the clips folder. */
	void ClipLast(int seconds);

	/* Current disk usage of the loop folder, in bytes. */
	quint64 UsedBytes() const;

	/* Segment being written right now (empty when not recording) */
	QString CurrentSegment() const { return Active() ? currentSegment : QString(); }

	/* Which file is being written and how much has been recorded, since
	 * Explorer shows the open segment as 0 KB (empty when not recording) */
	QString RecordingStatusText() const;

	/* Why the last start attempt failed (empty after a successful start) */
	QString LastStartError() const { return lastStartError; }

	/* Locked segments are never deleted by the disk quota */
	void Lock(const QStringList &files);
	void Unlock(const QStringList &files);

	/* Re-reads settings, e.g. after the settings dialog was accepted. */
	void SettingsChanged();

	/* Called by OBSBasic from the output's signals */
	void OnStarted();
	void OnStopped(int code, const QString &error);
	void OnFileChanged(const QString &nextFile);

signals:
	void activeChanged(bool active);
	void armedChanged(bool armed);
	void clipStarted(int seconds);
	void clipSaved(const QString &path);
	void clipFailed(const QString &error);
	/* A segment was finished or deleted */
	void segmentsChanged();
	/* RecordingStatusText changed; every few seconds while recording */
	void recordingStatusChanged();
	/* The segment stopped growing on disk / started growing again */
	void writingStalled();
	void writingResumed();

private:
	struct PendingClip {
		int seconds;
		QDateTime requested;
	};

	OBSBasic *main;
	QTimer processTimer;
	QTimer splitTimeout;
	QTimer captureTimer;
	QTimer statusTimer;
	LoopCapture *capture;
	StarlingLink *starling;

	QString label;
	QString fullscreenExe;
	QString currentSegment;
	/* Output bytes when currentSegment began, to size the open segment */
	quint64 segmentStartBytes = 0;
	QDateTime startedAt;
	/* Stall watchdog: last on-disk size of currentSegment and when it
	 * last changed */
	qint64 diskSize = -1;
	QDateTime diskSizeChanged;
	bool stalled = false;
	StopCause stopCause = StopCause::Unexpected;
	StopCause lastStopCause = StopCause::Unexpected;
	QStringList sessionSegments;
	std::vector<PendingClip> pendingClips;
	QHash<QString, int> lockedSegments;

	bool armed = false;
	bool autoStarted = false;
	bool suppressAutoStart = false;
	int startHolds = 0;
	int missingPolls = 0;
	QDateTime retryAutoStartAt;
	QString lastStartError;

	void CheckProcesses();
	void CheckWriting();
	void UpdateCapture();
	void EnforceQuota();
	void ProcessPendingClips(bool segmentJustFinalized = true);
	void ExportClip(int seconds, const QDateTime &requested, double lagSeconds);
	QString LabelForPattern(const QString &pattern) const;
};
