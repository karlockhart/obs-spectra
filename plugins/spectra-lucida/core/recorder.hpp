#pragma once

#include "carnivore.hpp"
#include "store.hpp"

#include <spectra-vision/chat.hpp>
#include <spectra-vision/image.hpp>
#include <spectra-vision/ocr.hpp>

#include <QString>

#include <functional>
#include <memory>
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
	/* Carnivore mode: read every block of text on screen, not just the chat
	 * box (which becomes the region named "chat") */
	bool carnivore = false;
	/* Carnivore mode: regions drawn in the region editor, and whether text
	 * outside them is "other" instead of new regions */
	std::vector<RegionRule> regions;
	bool onlyDrawn = false;
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
	int regions = 0; /* carnivore mode: text blocks read */
	QString profile; /* the game profile in use, if any */

	QString Describe() const;
};

/* A frame from the game plus a description of where it came from */
struct GrabbedFrame {
	spectra::Image image;
	QString target;     /* e.g. the capture source's name */
	QString executable; /* the process it is hooked onto, for game profiles */
};

/* Which rows begin a new chat entry: a timestamped row always does; any
 * other row does unless the row above ran to the wrap point of the box */
std::vector<int> EntryStarts(const std::vector<spectra::Row> &rows, int x0, int x1);

/* Rows into chat entries, a new entry at each index in starts (Obscura's
 * parsing per entry; an untimed line that reads like a message in its own
 * right is labelled by its content) */
std::vector<spectra::ChatEntry> BuildChat(const spectra::Image &img, const std::vector<spectra::Row> &rows,
					  const std::vector<spectra::Rect> &rects, const std::vector<int> &starts);

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
	/* Where the loop recording was at a wall-clock time; lines are tagged
	 * with it as they are added (unset: no video metadata) */
	std::function<std::optional<VideoSpot>(double wallTs)> locateVideo;
	/* The game profile for an executable; its Lucida section replaces the
	 * config's reading settings while that game is captured */
	std::function<std::optional<GameProfile>(const QString &executable)> profileFor;

private:
	RecorderConfig base; /* as given, before any game profile */
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
	std::unique_ptr<RegionTracker> regions; /* carnivore mode */
	std::optional<QString> profileExe;      /* executable the profile was chosen for */
	QString profileName;

	void UseProfile(const std::optional<GameProfile> &profile);
	void Configure();

	std::pair<long long, QString> FrameTimestamp(const spectra::Image &img);
	void Adapt(int added, bool turnover);
	void KeepFrame(const spectra::Image &img, long long frameTs, const std::vector<Sighting> &lines);
	void SaveCrop(const spectra::Image &crop, long long frameTs);
};

/* <folder>/<local date>/<frameTs><suffix>, with "-2", "-3"... if taken */
QString DatedPath(const QString &folder, long long frameTs, const QString &suffix);

} // namespace lucida
