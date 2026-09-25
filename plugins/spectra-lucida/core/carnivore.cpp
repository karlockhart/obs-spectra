#include "carnivore.hpp"
#include "recorder.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lucida {

namespace {
/* Distances in line heights */
constexpr float kWordGap = 3.0f;     /* between boxes on one line (icons in a kill feed) */
constexpr float kLineGap = 1.0f;     /* between stacked lines of one block */
constexpr float kAlign = 1.5f;       /* how far aligned edges may differ */
constexpr float kHeightRatio = 1.8f; /* lines of one block are about the same size */
constexpr float kPad = 0.5f;         /* around a block when boxing its lines */

constexpr int kMinWords = 3;
constexpr double kWrapTolerance = 0.02;
constexpr double kMinOverlap = 0.5; /* of the smaller of block and region */
constexpr double kMaxGrowth = 1.5;  /* a match widens a region at most this much */

double Area(const spectra::Region &r)
{
	return std::max(0.0, r.right - r.left) * std::max(0.0, r.bottom - r.top);
}

spectra::Region Intersect(const spectra::Region &a, const spectra::Region &b)
{
	return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
		std::min(a.bottom, b.bottom)};
}

spectra::Region Union(const spectra::Region &a, const spectra::Region &b)
{
	return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
		std::max(a.bottom, b.bottom)};
}

/* Do two boxes belong to the same block of text? */
bool Linked(const spectra::OcrBox &a, const spectra::OcrBox &b)
{
	const float ha = a.y1 - a.y0, hb = b.y1 - b.y0;
	const float lo = std::min(ha, hb), hi = std::max(ha, hb);
	if (lo <= 0 || hi > kHeightRatio * lo) {
		return false;
	}
	const float overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
	if (overlap >= 0.5f * lo) {
		/* the same line */
		return std::max(a.x0, b.x0) - std::min(a.x1, b.x1) <= kWordGap * hi;
	}
	const spectra::OcrBox &upper = a.y0 <= b.y0 ? a : b;
	const spectra::OcrBox &lower = a.y0 <= b.y0 ? b : a;
	if (lower.y0 - upper.y1 > kLineGap * hi) {
		return false;
	}
	const float tolerance = kAlign * hi;
	return std::fabs(a.x0 - b.x0) <= tolerance || std::fabs(a.x1 - b.x1) <= tolerance ||
	       std::fabs((a.x0 + a.x1) - (b.x0 + b.x1)) / 2 <= tolerance;
}

int Find(std::vector<int> &parent, int i)
{
	while (parent[i] != i) {
		parent[i] = parent[parent[i]];
		i = parent[i];
	}
	return i;
}

float MedianHeight(const std::vector<spectra::OcrBox> &boxes)
{
	std::vector<float> h;
	for (const spectra::OcrBox &b : boxes) {
		h.push_back(b.y1 - b.y0);
	}
	if (h.empty()) {
		return 0.0f;
	}
	std::nth_element(h.begin(), h.begin() + h.size() / 2, h.end());
	return h[h.size() / 2];
}

/* Untimed text: a row continues the one above only when that one ran to the
 * wrap point and this one carries on mid-sentence */
std::vector<int> UntimedStarts(const std::vector<spectra::Row> &rows, const spectra::Rect &rect)
{
	float wrapX = (float)rect.x0;
	for (const spectra::Row &r : rows) {
		wrapX = std::max(wrapX, r.x1);
	}
	const double tolerance = kWrapTolerance * std::max(1, rect.x1 - rect.x0);
	std::vector<int> starts;
	for (size_t i = 0; i < rows.size(); i++) {
		if (i > 0) {
			const spectra::Row &above = rows[i - 1];
			const QString text = rows[i].Text().trimmed();
			const bool wrapped = above.x1 >= wrapX - tolerance &&
					     std::fabs(rows[i].x0 - above.x0) <= kAlign * above.Height();
			if (wrapped && !text.isEmpty() && text[0].isLower()) {
				continue;
			}
		}
		starts.push_back((int)i);
	}
	return starts;
}
} // namespace

std::vector<TextBlock> FindTextBlocks(const std::vector<spectra::OcrBox> &boxes)
{
	const int n = (int)boxes.size();
	std::vector<int> parent(n);
	std::iota(parent.begin(), parent.end(), 0);
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			if (Linked(boxes[i], boxes[j])) {
				parent[Find(parent, i)] = Find(parent, j);
			}
		}
	}

	std::vector<TextBlock> blocks;
	std::vector<int> blockOf(n, -1);
	for (int i = 0; i < n; i++) {
		const int root = Find(parent, i);
		if (blockOf[root] < 0) {
			blockOf[root] = (int)blocks.size();
			blocks.emplace_back();
		}
		blocks[blockOf[root]].boxes.push_back(boxes[i]);
	}
	for (TextBlock &b : blocks) {
		float x0 = b.boxes[0].x0, y0 = b.boxes[0].y0, x1 = b.boxes[0].x1, y1 = b.boxes[0].y1;
		for (const spectra::OcrBox &box : b.boxes) {
			x0 = std::min(x0, box.x0);
			y0 = std::min(y0, box.y0);
			x1 = std::max(x1, box.x1);
			y1 = std::max(y1, box.y1);
		}
		b.rect = {(int)std::floor(x0), (int)std::floor(y0), (int)std::ceil(x1), (int)std::ceil(y1)};
	}
	std::stable_sort(blocks.begin(), blocks.end(), [](const TextBlock &a, const TextBlock &b) {
		return a.rect.y0 != b.rect.y0 ? a.rect.y0 < b.rect.y0 : a.rect.x0 < b.rect.x0;
	});
	return blocks;
}

BlockKind Classify(const std::vector<spectra::Row> &rows)
{
	BlockKind kind = BlockKind::None;
	for (const spectra::Row &r : rows) {
		const QString text = r.Text();
		if (spectra::MatchTimestamp(text) >= 0) {
			return BlockKind::Text;
		}
		int words = 0;
		for (const QString &word : text.split(' ', Qt::SkipEmptyParts)) {
			if (std::any_of(word.begin(), word.end(), [](QChar c) { return c.isLetter(); })) {
				words++;
			}
		}
		if (words >= kMinWords) {
			return BlockKind::Text;
		}
		if (words) {
			kind = BlockKind::Labels;
		}
	}
	return kind;
}

std::vector<spectra::ChatEntry> ReadBlock(const spectra::Image &img, const std::vector<spectra::Row> &rows,
					  const spectra::Rect &rect)
{
	const bool timed = std::any_of(rows.begin(), rows.end(),
				       [](const spectra::Row &r) { return spectra::MatchTimestamp(r.Text()) >= 0; });
	std::vector<int> starts = timed ? EntryStarts(rows, rect.x0, rect.x1) : UntimedStarts(rows, rect);
	return BuildChat(img, rows, spectra::RowRects(rows, rect.x0, rect.y0, rect.y1), starts);
}

/* ------------------------------------------------------------------------- */

RegionTracker::RegionTracker(std::vector<ScreenRegion> learned, std::vector<RegionRule> drawn_, bool onlyDrawn_)
	: drawn(std::move(drawn_)),
	  onlyDrawn(onlyDrawn_)
{
	/* a drawn region replaces a learned one of the same name */
	for (ScreenRegion &r : learned) {
		if (std::none_of(drawn.begin(), drawn.end(), [&](const RegionRule &d) { return d.name == r.name; }) &&
		    r.name != kOtherRegion) {
			regions.push_back(std::move(r));
		}
	}
}

std::vector<spectra::Rect> RegionTracker::Ignored(int width, int height) const
{
	std::vector<spectra::Rect> out;
	for (const RegionRule &d : drawn) {
		if (d.mode == RegionMode::Ignore) {
			out.push_back(d.area.ToPixels(width, height));
		}
	}
	return out;
}

QString RegionTracker::PlaceName(const spectra::Region &area)
{
	const double cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
	const QString col = cx < 1.0 / 3 ? QStringLiteral("left") : cx < 2.0 / 3 ? QString() : QStringLiteral("right");
	const QString row = cy < 1.0 / 3 ? QStringLiteral("top") : cy < 2.0 / 3 ? QString() : QStringLiteral("bottom");
	if (row.isEmpty() && col.isEmpty()) {
		return QStringLiteral("centre");
	}
	if (row.isEmpty() || col.isEmpty()) {
		return row + col;
	}
	return row + '-' + col;
}

Placement RegionTracker::Assign(const spectra::Rect &block, int width, int height, double now, bool learn)
{
	const double w = std::max(1, width), h = std::max(1, height);
	const spectra::Region area{block.x0 / w, block.y0 / h, block.x1 / w, block.y1 / h};
	const double blockArea = std::max(Area(area), 1e-9);

	/* drawn regions: the one holding most of the block */
	const RegionRule *inside = nullptr;
	double most = 0.0;
	for (const RegionRule &d : drawn) {
		const double share = Area(Intersect(area, d.area)) / blockArea;
		if (share > most) {
			inside = &d;
			most = share;
		}
	}
	if (inside && most >= kMinOverlap) {
		return {inside->mode == RegionMode::Other ? kOtherRegion : inside->name, inside->mode};
	}
	if (!learn || onlyDrawn) {
		return {kOtherRegion, RegionMode::Other};
	}

	size_t best = regions.size();
	double bestScore = 0.0;
	for (size_t i = 0; i < regions.size(); i++) {
		const double shared = Area(Intersect(area, regions[i].area));
		const double score = shared / std::max(std::min(blockArea, Area(regions[i].area)), 1e-9);
		if (score > bestScore) {
			best = i;
			bestScore = score;
		}
	}

	if (best < regions.size() && bestScore >= kMinOverlap) {
		ScreenRegion &r = regions[best];
		const spectra::Region grown = Union(r.area, area);
		if (Area(grown) <= kMaxGrowth * std::max(blockArea, Area(r.area))) {
			r.area = grown;
		}
		if (!r.firstSeen) {
			r.firstSeen = now;
		}
		r.lastSeen = now;
		if (std::find(dirty.begin(), dirty.end(), best) == dirty.end()) {
			dirty.push_back(best);
		}
		return {r.name, RegionMode::Read};
	}

	const QString place = PlaceName(area);
	QString name = place;
	auto taken = [this](const QString &candidate) {
		return std::any_of(regions.begin(), regions.end(),
				   [&](const ScreenRegion &r) { return r.name == candidate; }) ||
		       std::any_of(drawn.begin(), drawn.end(),
				   [&](const RegionRule &d) { return d.name == candidate; });
	};
	for (int n = 2; taken(name); n++) {
		name = QStringLiteral("%1 %2").arg(place).arg(n);
	}
	regions.push_back({0, name, area, now, now});
	dirty.push_back(regions.size() - 1);
	return {name, RegionMode::Read};
}

std::vector<ScreenRegion *> RegionTracker::TakeDirty()
{
	std::vector<ScreenRegion *> out;
	for (size_t i : dirty) {
		out.push_back(&regions[i]);
	}
	dirty.clear();
	return out;
}

std::vector<RegionRule> WithChatBox(std::vector<RegionRule> drawn, const spectra::Region &chatBox)
{
	if (std::none_of(drawn.begin(), drawn.end(),
			 [](const RegionRule &r) { return r.name == QLatin1String("chat"); })) {
		drawn.push_back({QStringLiteral("chat"), chatBox, RegionMode::Read});
	}
	return drawn;
}

std::vector<spectra::ChatEntry> ReadScreen(const spectra::Image &img, spectra::OcrEngine &ocr, RegionTracker &tracker,
					   double now, const std::vector<spectra::Rect> &exclude, int *blockCount)
{
	std::vector<spectra::Rect> skip = exclude;
	for (const spectra::Rect &r : tracker.Ignored(img.width, img.height)) {
		skip.push_back(r);
	}
	std::vector<spectra::OcrBox> boxes;
	for (const spectra::OcrBox &b : ocr.Read(img, 0.3f)) {
		const float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
		const bool excluded = std::any_of(skip.begin(), skip.end(), [&](const spectra::Rect &r) {
			return cx >= r.x0 && cx < r.x1 && cy >= r.y0 && cy < r.y1;
		});
		if (!excluded) {
			boxes.push_back(b);
		}
	}

	std::vector<spectra::ChatEntry> entries;
	int kept = 0;
	for (const TextBlock &block : FindTextBlocks(boxes)) {
		std::vector<spectra::Row> rows;
		for (spectra::Row &r : spectra::GroupRows(block.boxes)) {
			if (spectra::IsMeaningful(r.Text())) {
				rows.push_back(std::move(r));
			}
		}
		const BlockKind kind = rows.empty() ? BlockKind::None : Classify(rows);
		if (kind == BlockKind::None) {
			continue;
		}
		const int pad = (int)std::ceil(kPad * MedianHeight(block.boxes));
		const spectra::Rect rect{std::max(0, block.rect.x0 - pad), std::max(0, block.rect.y0 - pad),
					 std::min(img.width, block.rect.x1 + pad),
					 std::min(img.height, block.rect.y1 + pad)};
		const Placement place = tracker.Assign(block.rect, img.width, img.height, now, kind == BlockKind::Text);
		if (place.mode == RegionMode::Ignore) {
			continue;
		}
		std::vector<spectra::ChatEntry> read;
		if (kind == BlockKind::Text) {
			read = ReadBlock(img, rows, rect);
			kept++;
		} else {
			/* labels: one entry per row ("Carcer Way", "Vinewood Blvd") */
			std::vector<int> starts(rows.size());
			std::iota(starts.begin(), starts.end(), 0);
			read = BuildChat(img, rows, spectra::RowRects(rows, rect.x0, rect.y0, rect.y1), starts);
		}
		for (spectra::ChatEntry &e : read) {
			e.region = place.name;
			entries.push_back(std::move(e));
		}
	}
	for (size_t i = 0; i < entries.size(); i++) {
		entries[i].index = (int)i;
	}
	if (blockCount) {
		*blockCount = kept;
	}
	return entries;
}

} // namespace lucida
