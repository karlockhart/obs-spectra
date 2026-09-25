#include "recorder.hpp"

#include <spectra-vision/imaging.hpp>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <algorithm>
#include <chrono>

namespace lucida {

namespace {
constexpr int kTurnoverMinLines = 3; /* below this, "every line is new" means chat was empty */
constexpr int kClippedMargin = 3;    /* px: a row this close to the region top was cut by it */
constexpr double kWrapTolerance = 0.02;
constexpr double kWrapMinFill = 0.6;

double Now()
{
	return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

QImage ToQImage(const spectra::Image &img)
{
	QImage q(img.width, img.height, QImage::Format_RGB888);
	for (int y = 0; y < img.height; y++) {
		const uint8_t *src = img.row(y);
		uchar *dst = q.scanLine(y);
		for (int x = 0; x < img.width; x++) {
			dst[x * 3 + 0] = src[x * 3 + 2];
			dst[x * 3 + 1] = src[x * 3 + 1];
			dst[x * 3 + 2] = src[x * 3 + 0];
		}
	}
	return q;
}

int PruneFiles(const QString &folder, const QString &pattern, int days)
{
	if (days <= 0 || folder.isEmpty() || !QFileInfo(folder).isDir()) {
		return 0;
	}
	const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-(qint64)days * 86400);
	int removed = 0;
	QDirIterator it(folder, {pattern}, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		QString file = it.next();
		if (QFileInfo(file).lastModified() < cutoff && QFile::remove(file)) {
			removed++;
		}
	}
	return removed;
}
} // namespace

QString Tick::Describe() const
{
	if (!foundWindow) {
		return QStringLiteral("no game window");
	}
	if (!changed) {
		return QStringLiteral("unchanged (d=%1)").arg(distance, 0, 'f', 3);
	}
	QString s = QStringLiteral("%1 entries, %2 new, ts=%3(%4), ocr %5 ms, d=%6")
			    .arg(entries)
			    .arg(added)
			    .arg(frameTs.value_or(0))
			    .arg(tsSource)
			    .arg((int)ocrMs)
			    .arg(distance, 0, 'f', 3);
	if (regions) {
		s += QStringLiteral(", %1 region(s)").arg(regions);
	}
	if (turnover) {
		s += QStringLiteral(" TURNOVER");
	}
	return s;
}

std::vector<int> EntryStarts(const std::vector<spectra::Row> &rows, int x0, int x1)
{
	const int width = std::max(1, x1 - x0);
	/* the box's wrap point, as this frame shows it */
	float wrapX = (float)x0;
	if (!rows.empty()) {
		wrapX = rows[0].x1;
		for (const spectra::Row &r : rows) {
			wrapX = std::max(wrapX, r.x1);
		}
	}
	const double tolerance = kWrapTolerance * width;
	std::vector<int> starts;
	for (size_t i = 0; i < rows.size(); i++) {
		if (i == 0 || spectra::MatchTimestamp(rows[i].Text()) >= 0) {
			starts.push_back((int)i);
			continue;
		}
		const spectra::Row &above = rows[i - 1];
		bool wrapped = (above.x1 - x0) >= kWrapMinFill * width && above.x1 >= wrapX - tolerance;
		if (!wrapped) {
			starts.push_back((int)i);
		}
	}
	return starts;
}

std::vector<spectra::ChatEntry> ReadChat(const spectra::Image &img, const spectra::Image &crop,
					 const spectra::Rect &rect, spectra::OcrEngine &ocr)
{
	std::vector<spectra::OcrBox> boxes;
	for (const spectra::OcrBox &b : ocr.Read(crop, 0.3f)) {
		boxes.push_back(b.Shifted((float)rect.x0, (float)rect.y0));
	}
	std::vector<spectra::Row> rows;
	for (spectra::Row &r : spectra::GroupRows(boxes)) {
		if (spectra::IsMeaningful(r.Text())) {
			rows.push_back(std::move(r));
		}
	}
	std::vector<spectra::Rect> rects = spectra::RowRects(rows, rect.x0, rect.y0, rect.y1);
	std::vector<spectra::ChatEntry> entries = BuildChat(img, rows, rects, EntryStarts(rows, rect.x0, rect.x1));
	/* The top row may be half scrolled off the box; drop it only when the
	 * region edge actually clips it */
	if (!entries.empty() && entries[0].Partial() && !entries[0].rows.empty() &&
	    entries[0].rows[0].y0 <= rect.y0 + kClippedMargin) {
		entries.erase(entries.begin());
	}
	return entries;
}

std::vector<spectra::ChatEntry> BuildChat(const spectra::Image &img, const std::vector<spectra::Row> &rows,
					  const std::vector<spectra::Rect> &rects, const std::vector<int> &starts)
{
	/* Obscura's BuildEntries does the parsing and colour work, one group of
	 * rows at a time, so the entry boundaries are the ones decided here */
	std::vector<spectra::ChatEntry> entries;
	for (size_t n = 0; n < starts.size(); n++) {
		size_t begin = (size_t)starts[n];
		size_t end = n + 1 < starts.size() ? (size_t)starts[n + 1] : rows.size();
		std::vector<spectra::Row> groupRows(rows.begin() + begin, rows.begin() + end);
		std::vector<spectra::Rect> groupRects(rects.begin() + begin, rects.begin() + end);
		for (spectra::ChatEntry &e : spectra::BuildEntries(img, groupRows, groupRects)) {
			entries.push_back(std::move(e));
		}
	}
	for (size_t i = 0; i < entries.size(); i++) {
		entries[i].index = (int)i;
	}
	/* An untimed line that reads like a message in its own right is
	 * labelled by its content, not as a "continuation" */
	for (spectra::ChatEntry &e : entries) {
		if (e.Partial()) {
			QString own = spectra::ChannelOf(e.body, e.tags, false);
			if (own != QLatin1String("other")) {
				e.channel = own;
			}
		}
	}
	return entries;
}

QString DatedPath(const QString &folder, long long frameTs, const QString &suffix)
{
	QString day = QDateTime::fromSecsSinceEpoch(frameTs).toString(QStringLiteral("yyyy-MM-dd"));
	QDir dir(QDir(folder).filePath(day));
	dir.mkpath(QStringLiteral("."));
	QString path = dir.filePath(QString::number(frameTs) + suffix);
	for (int n = 2; QFileInfo::exists(path); n++) {
		path = dir.filePath(QStringLiteral("%1-%2%3").arg(frameTs).arg(n).arg(suffix));
	}
	return path;
}

Recorder::Recorder(RecorderConfig config_, Store &store_, spectra::OcrEngine &ocr_, Grab grab_)
	: base(config_),
	  config(std::move(config_)),
	  store(store_),
	  ocr(ocr_),
	  grab(std::move(grab_)),
	  interval(config.interval)
{
	wallClock = [] {
		return (long long)Now();
	};
	Configure();
}

void Recorder::Configure()
{
	regions.reset();
	signature.reset();
	if (!config.carnivore) {
		return;
	}
	regions = std::make_unique<RegionTracker>(store.ScreenRegions(), WithChatBox(config.regions, config.chatRegion),
						  config.onlyDrawn);
}

void Recorder::UseProfile(const std::optional<GameProfile> &profile)
{
	config = base;
	profileName.clear();
	if (profile) {
		profileName = profile->name;
		if (std::optional<LucidaProfile> p = profile->Lucida()) {
			config.carnivore = p->carnivore;
			config.chatRegion = p->chatRegion;
			config.hudRegion = p->hudRegion;
			config.readHud = p->readHud;
			config.onlyDrawn = p->onlyDrawn;
			config.regions = p->regions;
		}
	}
	Configure();
}

Tick Recorder::Step()
{
	Tick tick;
	tick.at = Now();

	std::optional<GrabbedFrame> frame = grab ? grab() : std::nullopt;
	if (!frame || frame->image.empty()) {
		interval = config.idleInterval;
		signature.reset();
		tick.interval = interval;
		return tick;
	}
	const spectra::Image &img = frame->image;
	tick.foundWindow = true;
	if (profileFor && profileExe != frame->executable) {
		profileExe = frame->executable;
		UseProfile(profileFor(frame->executable));
	}
	tick.profile = profileName;

	if (!sessionId || frame->target != sessionTarget) {
		if (sessionId) {
			store.EndSession(*sessionId);
		}
		sessionTarget = frame->target;
		sessionId = store.StartSession(frame->target, img.width, img.height);
	}

	/* carnivore mode gates on (and reads) the whole frame */
	const spectra::Rect r = config.carnivore ? spectra::Rect{0, 0, img.width, img.height}
						 : config.chatRegion.ToPixels(img.width, img.height);
	const spectra::Image crop = config.carnivore ? spectra::Image() : spectra::Crop(img, r.x0, r.y0, r.x1, r.y1);
	const spectra::Image &area = config.carnivore ? img : crop;
	if (img.width != lastWidth || img.height != lastHeight) {
		/* a resolution change invalidates the signature */
		signature.reset();
		lastWidth = img.width;
		lastHeight = img.height;
	}

	std::vector<float> sig = spectra::Signature(spectra::TextMask(area), area.width, area.height);
	auto [changed, distance] =
		spectra::SignatureChanged(signature ? &*signature : nullptr, sig, config.gateThreshold);
	signature = std::move(sig);
	tick.distance = distance;
	if (!changed) {
		interval = config.adaptive ? std::min(config.maxInterval, interval * 1.2) : config.interval;
		tick.interval = interval;
		return tick;
	}
	tick.changed = true;

	auto began = std::chrono::steady_clock::now();
	std::vector<spectra::ChatEntry> entries;
	if (regions) {
		std::vector<spectra::Rect> exclude;
		if (config.readHud) {
			exclude.push_back(config.hudRegion.ToPixels(img.width, img.height));
		}
		entries = ReadScreen(img, ocr, *regions, tick.at, exclude, &tick.regions);
		for (ScreenRegion *region : regions->TakeDirty()) {
			store.SaveScreenRegion(*region);
		}
	} else {
		entries = ReadChat(img, crop, r, ocr);
	}
	auto [frameTs, tsSource] = FrameTimestamp(img);
	tick.ocrMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();

	std::vector<Sighting> seen;
	std::vector<long long> ids = store.AddFrame(entries, frameTs, tsSource, sessionId, &seen);
	if (!ids.empty() && locateVideo) {
		if (std::optional<VideoSpot> spot = locateVideo(tick.at)) {
			store.SetVideo(ids, *spot);
		}
	}
	const int added = (int)ids.size();
	const bool turnover = added >= kTurnoverMinLines && added == (int)entries.size();
	Adapt(added, turnover);
	if (added && config.keepFrames) {
		KeepFrame(img, frameTs, seen);
	}
	if (added && config.keepCrops && !config.carnivore) {
		SaveCrop(crop, frameTs);
	}

	tick.interval = interval;
	tick.entries = (int)entries.size();
	tick.added = added;
	tick.frameTs = frameTs;
	tick.tsSource = tsSource;
	tick.turnover = turnover;
	return tick;
}

std::pair<long long, QString> Recorder::FrameTimestamp(const spectra::Image &img)
{
	if (config.readHud) {
		if (auto ts = spectra::ReadUnixTimestamp(img, config.hudRegion, ocr)) {
			return {*ts, QStringLiteral("hud")};
		}
	}
	return {wallClock(), QStringLiteral("wall")};
}

void Recorder::Adapt(int added, bool turnover)
{
	if (!config.adaptive) {
		interval = config.interval;
	} else if (turnover) {
		/* messages were missed between ticks: look more often */
		interval = std::max(config.minInterval, interval / 2);
	} else if (added) {
		interval = config.interval;
	} else {
		interval = std::min(config.maxInterval, interval * 1.2);
	}
}

void Recorder::KeepFrame(const spectra::Image &img, long long frameTs, const std::vector<Sighting> &lines)
{
	if (config.framesDir.isEmpty()) {
		return;
	}
	QString path = DatedPath(config.framesDir, frameTs, QStringLiteral(".jpg"));
	if (ToQImage(img).save(path, "JPG", config.frameQuality)) {
		store.AttachFrame(lines, path, img.width, img.height, frameTs, sessionId);
	}
}

void Recorder::SaveCrop(const spectra::Image &crop, long long frameTs)
{
	if (config.cropsDir.isEmpty()) {
		return;
	}
	QString path = DatedPath(config.cropsDir, frameTs, QStringLiteral(".webp"));
	ToQImage(crop).save(path, "WEBP", config.cropQuality);
}

int Recorder::PruneImages()
{
	int removed = 0;
	for (const QString &path : store.PruneFrames(config.frameRetentionDays)) {
		if (QFile::remove(path)) {
			removed++;
		}
	}
	removed += PruneFiles(config.framesDir, QStringLiteral("*.jpg"), config.frameRetentionDays);
	removed += PruneFiles(config.cropsDir, QStringLiteral("*.webp"), config.cropRetentionDays);
	return removed;
}

void Recorder::Close()
{
	if (sessionId) {
		store.EndSession(*sessionId);
		sessionId.reset();
	}
}

} // namespace lucida
