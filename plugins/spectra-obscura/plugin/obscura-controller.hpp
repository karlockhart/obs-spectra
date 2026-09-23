#pragma once

#include "learner.hpp"
#include "obscura-config.hpp"

#include <spectra-vision/chat.hpp>
#include <spectra-vision/image.hpp>

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace spectra {
class OcrEngine;
class FrameGrabber;
} // namespace spectra

namespace obscura {

class ReviewWindow;

/* One screenshot on its way through analysis, review and saving */
struct Job {
	spectra::Image image;
	QString source; /* hotkey | crop | watch | open */
	QString srcPath;
	QDateTime capturedAt;
	spectra::Analysis analysis;
	std::vector<Prediction> predictions;
	QString originalPath;
	bool uploadAfter = false;

	QString Label() const;
};
using JobPtr = std::shared_ptr<Job>;

/* Obscura inside Spectra (port of obscura/gui/app.py's Controller): captures
 * the game from OBS on a hotkey, snips screen regions, watches folders,
 * detects chat, suggests what to censor, and learns from each review.
 * Lives on the UI thread; OCR, learning and uploads run on one worker. */
class Controller : public QObject {
	Q_OBJECT

public:
	explicit Controller(QObject *parent = nullptr);
	~Controller() override;

	void Start();
	void Stop();

	/* --- user actions (UI thread) --- */
	void Capture();
	void CaptureRegion();
	void OpenScreenshots(QWidget *parent);
	void SubmitPath(const QString &path, const QString &source);
	void ShowReview();
	void OpenSettings(QWidget *parent);
	void UploadLast();
	void UploadFile(const QString &path);
	void SetPaused(bool paused);
	bool Paused() const { return paused; }
	void CheckUpdates(bool manual);
	bool ExportDefinitions(const QString &path, QString *message);
	void OpenFolder(const QString &folder);

	/* --- from the review window --- */
	void Finalise(const JobPtr &job, const std::vector<bool> &flags, const std::vector<spectra::Rect> &manual,
		      std::optional<long long> unixTs, bool learn, bool uploadAfter = false);
	void Skip(const JobPtr &job);
	void SetChatRegion(const JobPtr &job, const spectra::Rect &rect);

	Config &Cfg() { return cfg; }
	Learner *GetLearner() const { return learner.get(); }
	int Queued() const { return (int)queue.size(); }
	QString LastSaved() const { return lastSaved; }
	QString Status() const { return status; }
	void ApplyConfig();

signals:
	void statusChanged(const QString &status);
	void queueChanged(int queued);
	void lastSavedChanged(const QString &path);

private:
	Config cfg;
	std::unique_ptr<Learner> learner;
	std::unique_ptr<spectra::FrameGrabber> grabber;
	std::unique_ptr<spectra::OcrEngine> ocr; /* worker thread only */
	QPointer<ReviewWindow> review;
	std::deque<JobPtr> queue;
	QString lastSaved;
	QString status;
	bool paused = false;

	QFileSystemWatcher watcher;
	QSet<QString> seenFiles;
	QHash<QString, qint64> recentlySubmitted;

	std::thread worker;
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::function<void()>> tasks;
	bool stopping = false;

	void Post(std::function<void()> task);
	void RunWorker();
	void OnUi(std::function<void()> fn);
	bool EnsureOcr(QString *error);

	void Analyse(const JobPtr &job, const Config &c);
	void AnalyseAsync(const JobPtr &job);
	void OnJobReady(const JobPtr &job);
	void Enqueue(const JobPtr &job);
	void ShowNext();
	bool CropNeedsReview(const Job &job) const;
	bool CanAutoApply(const Job &job) const;
	std::vector<Prediction> Predict(const Job &job) const;
	QString KeepOriginal(const Job &job, const QString &kind, const Config &c);

	void RestartWatcher();
	void OnDirectoryChanged(const QString &dir);
	void InstallDefinitions(const QString &url);

	void Notify(const QString &message, bool error = false);
};

} // namespace obscura
