#pragma once

#include "store.hpp"

#include <spectra-vision/chat.hpp>
#include <spectra-vision/image.hpp>
#include <spectra-vision/ocr.hpp>

#include <QString>

#include <functional>
#include <optional>

namespace lucida {

/* Sampling settings (subset of lucida/config.py that the loop uses) */
struct RecorderConfig {
	double interval = 5.0;
	double minInterval = 3.0;
	double maxInterval = 30.0;
	bool adaptive = true;
	double idleInterval = 15.0;
	spectra::Region chatRegion = spectra::kDefaultChatRegion;
	spectra::Region hudRegion = spectra::kDefaultHudRegion;
	bool readHud = true;
	double gateThreshold = 0.06;
	bool keepFrames = false;
	QString framesDir;
	int frameQuality = 80;
	int frameRetentionDays = 14;
	bool keepCrops = false;
	QString cropsDir;
	int cropQuality = 85;
	int cropRetentionDays = 14;
};

/* One pass of the loop, for logging and the UI */
struct Tick {
	double at = 0.0;
	double interval = 0.0;
	bool foundWindow = false;
	bool changed = false;
	double distance = 0.0;
	int entries = 0;
	int added = 0;
	std::optional<long long> frameTs;
	QString tsSource;
	double ocrMs = 0.0;
	bool turnover = false;

	QString Describe() const;
};

/* A frame from the game plus a description of where it came from */
struct GrabbedFrame {
	spectra::Image image;
	QString target; /* e.g. the capture source's name */
};

/* Which rows begin a new chat entry: a timestamped row always does; any
 * other row does unless the row above ran to the wrap point of the box */
std::vector<int> EntryStarts(const std::vector<spectra::Row> &rows, int x0, int x1);

/* Chat entries in one frame (Obscura's parsing with Lucida's wrap, relabel
 * and clipped-top-row rules) */
std::vector<spectra::ChatEntry> ReadChat(const spectra::Image &img, const spectra::Image &crop,
					 const spectra::Rect &rect, spectra::OcrEngine &ocr);

/* The sampling loop's step function (port of lucida.recorder.Recorder);
 * the caller runs it on a worker thread and sleeps tick.interval between
 * calls */
class Recorder {
public:
	using Grab = std::function<std::optional<GrabbedFrame>()>;
	using Clock = std::function<long long()>;

	Recorder(RecorderConfig config, Store &store, spectra::OcrEngine &ocr, Grab grab);

	Tick Step();
	/* Deletes screenshots and crops past their retention */
	int PruneImages();
	/* Ends the session (call when stopping) */
	void Close();

	const RecorderConfig &Config() const { return config; }
	double Interval() const { return interval; }

	/* Wall clock used when the HUD clock is unreadable (tests override) */
	Clock wallClock;

private:
	RecorderConfig config;
	Store &store;
	spectra::OcrEngine &ocr;
	Grab grab;
	double interval;
	std::optional<std::vector<float>> signature;
	int lastWidth = 0;
	int lastHeight = 0;
	std::optional<long long> sessionId;
	QString sessionTarget;

	std::pair<long long, QString> FrameTimestamp(const spectra::Image &img);
	void Adapt(int added, bool turnover);
	void KeepFrame(const spectra::Image &img, long long frameTs, const std::vector<long long> &ids);
	void SaveCrop(const spectra::Image &crop, long long frameTs);
};

/* <folder>/<local date>/<frameTs><suffix>, with "-2", "-3"... if taken */
QString DatedPath(const QString &folder, long long frameTs, const QString &suffix);

} // namespace lucida
