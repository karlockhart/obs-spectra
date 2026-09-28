#pragma once

#include "lucida-controller.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace lucida {

/*
 * Transcribes the speech in Spectra's loop recording into the log (Spectra),
 * one finished segment at a time, on its own worker thread: each speaker
 * track separately when the loop records them, else the full mix.
 *
 * The log records which segments are done, so after a restart it carries on
 * where it stopped. Segments go as soon as they're finished, or only once
 * the game has closed and the loop stopped.
 */
class SpeechController : public QObject {
	Q_OBJECT

public:
	explicit SpeechController(QObject *parent = nullptr);
	~SpeechController() override;

	/* Takes the speech settings, the log and the tag rules */
	void Apply(const Settings &settings);
	void Stop();

	QString Status() const;

signals:
	void statusChanged(const QString &status);
	/* A segment's transcript was stored */
	void transcribed(const QString &segment, int lines);

private:
	struct Config {
		SpeechSettings speech;
		QString dbPath;
		QList<TagRule> tagRules;
		bool tolerateTypos = true;
	};
	struct Next {
		QString path;
		long long size = 0;
	};

	mutable std::mutex mutex;
	std::condition_variable wakeup;
	std::thread worker;
	bool stopRequested = false;
	bool configChanged = false;
	Config config;
	/* Loop state, refreshed on the UI thread (the worker can't read the
	 * profile safely) */
	QString loopDir;
	bool loopActive = false;
	QString status;
	QTimer loopPoll;

	void PollLoop();
	void Run();
	bool Wait(int ms);
	void SetStatus(const QString &text);
};

} // namespace lucida
