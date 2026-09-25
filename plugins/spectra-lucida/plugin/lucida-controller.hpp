#pragma once

#include <spectra-grab/frame-grabber.hpp>
#include "recorder.hpp"
#include "store.hpp"
#include "profiles.hpp"
#include "tagger.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace lucida {

/* Lucida settings, stored in the Spectra profile (section "Lucida") */
struct Settings {
	bool enabled = true;
	RecorderConfig recorder;
	int ocrThreads = 2;
	QString dbPath;
	int retentionDays = 0; /* 0 = keep forever */
	QString targetProcess = QStringLiteral("FiveM.*GTAProcess\\.exe");
	/* Run only while Spectra's loop recording is running */
	bool followLoop = true;
	QList<TagRule> tagRules = DefaultTagRules();
	bool tolerateTypos = true;

	static Settings Load();
	void Save() const;
	/* Folder for the log, screenshots and crops (Spectra's Lucida folder) */
	static QString DefaultFolder();
};

/* The per-game profiles file (see GameProfiles) */
QString ProfilesPath();

/* Spectra's loop recording folder and state */
QString LoopDirectory();
bool LoopRecordingActive();

/* Runs Lucida's sampling loop on a worker thread inside Spectra */
class Controller : public QObject {
	Q_OBJECT

public:
	explicit Controller(QObject *parent = nullptr);
	~Controller() override;

	void Start();
	void Stop();
	bool Running() const { return running; }

	void SetPaused(bool paused);
	bool Paused() const { return paused; }
	void SampleNow();

	/* Carnivore mode: read all the text on screen, not just the chat box */
	void SetCarnivore(bool on);
	/* Restarts sampling so edited game profiles apply */
	void ReloadProfiles();
	bool Carnivore() const { return settings.recorder.carnivore; }

	const Settings &CurrentSettings() const { return settings; }
	void ApplySettings(const Settings &settings);

	/* Read-only connection for the UI (safe alongside the worker's) */
	Store *Reader() const { return reader.get(); }
	/* The video spot of a line: stored when it was logged, else looked up
	 * from when it was first seen */
	std::optional<VideoSpot> VideoFor(const LogLine &line) const;

	/* Re-tags the whole log with the current rules in the background */
	void Relabel();

	QString Status() const { return status; }

signals:
	void ticked(int added, double interval, bool foundWindow);
	void statusChanged(const QString &status);
	void failed(const QString &message);
	void relabelled(int changed);
	void carnivoreChanged(bool on);

private:
	Settings settings;
	std::unique_ptr<Store> reader;

	std::thread worker;
	mutable std::mutex mutex;
	std::condition_variable wakeup;
	bool stopRequested = false;
	bool wakeRequested = false;
	bool running = false;
	bool paused = false;
	QString status;
	long long logged = 0;
	int regionsRead = 0; /* text blocks in carnivore mode's last reading */
	QTimer loopPoll;
	QString loopDir; /* guarded by mutex; read by the worker */

	bool OpenReader();
	void PollLoop();
	void Run(Settings s);
	void SetStatus(const QString &text);
};

} // namespace lucida
