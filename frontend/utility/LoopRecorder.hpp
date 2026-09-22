#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include "LoopCapture.hpp"

#include <vector>

class OBSBasic;

/*
 * Spectra loop recording: continuously records the program output to a
 * folder of short Matroska segments, deletes the oldest segments to stay
 * under a disk quota, and turns the last N seconds into a single portable
 * clip on request. Optionally starts and stops itself when a matching game
 * process (e.g. "FiveM*") starts and exits.
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
	QStringList ProcessPatterns() const;

	LoopCapture *Capture() const { return capture; }

	bool Start(const QString &label = QString());
	void Stop();
	bool Active() const;

	/* Saves the last `seconds` of the loop as one file in the clips folder. */
	void ClipLast(int seconds);

	/* Current disk usage of the loop folder, in bytes. */
	quint64 UsedBytes() const;

	/* Re-reads settings, e.g. after the settings dialog was accepted. */
	void SettingsChanged();

	/* Called by OBSBasic from the output's signals */
	void OnStarted();
	void OnStopped(int code, const QString &error);
	void OnFileChanged(const QString &nextFile);

signals:
	void activeChanged(bool active);
	void clipStarted(int seconds);
	void clipSaved(const QString &path);
	void clipFailed(const QString &error);

private:
	struct PendingClip {
		int seconds;
		QDateTime requested;
	};

	OBSBasic *main;
	QTimer processTimer;
	QTimer splitTimeout;
	QTimer captureTimer;
	LoopCapture *capture;

	QString label;
	QString currentSegment;
	QStringList sessionSegments;
	std::vector<PendingClip> pendingClips;
	QHash<QString, int> lockedSegments;

	bool autoStarted = false;
	bool suppressAutoStart = false;
	int missingPolls = 0;

	void CheckProcesses();
	void UpdateCapture();
	void EnforceQuota();
	void ProcessPendingClips(bool segmentJustFinalized = true);
	void ExportClip(int seconds, const QDateTime &requested, double lagSeconds);
	void Lock(const QStringList &files);
	void Unlock(const QStringList &files);
	QString LabelForPattern(const QString &pattern) const;
};
