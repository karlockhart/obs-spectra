#include <spectra-vision/chat.hpp>

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

/* Port of obscura/chat.py, obscura/hud.py, obscura/pipeline.py and
 * obscura.config.Region. Regexes use Unicode properties so \w, \d and \s
 * behave like Python's str patterns. */

namespace spectra {

namespace {

const QRegularExpression::PatternOptions kUnicode = QRegularExpression::UseUnicodePropertiesOption;

const QRegularExpression &TimestampRe()
{
	static const QRegularExpression re(
		QStringLiteral("^(?:[^\\s\\d]{1,2}\\s+)?[^\\w\\[]{0,3}[\\[({|lIiEeCcFf]?\\s*"
			       "(\\d{1,2})\\s*[:.;,]\\s*(\\d{2})\\s*[:.;,]\\s*(\\d{2})\\s*[\\])}|lIJj1]?\\s*"),
		kUnicode);
	return re;
}

const QRegularExpression &VolatileTagRe()
{
	static const QRegularExpression re(QStringLiteral("\\A(?:(id )?\\d+( ?(min|mins|m|s|sec|h))?)\\z"), kUnicode);
	return re;
}

const QRegularExpression &TagRe()
{
	static const QRegularExpression re(QStringLiteral("^[\\s*»]{0,4}\\[([^\\[\\]]{1,28})\\]:?\\s*"), kUnicode);
	return re;
}

const char *const kTabWords[] = {"rp focus", "weazel", "faction", "ooc", "admin", "report"};

/* statistics.median */
double Median(std::vector<double> v)
{
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	size_t n = v.size();
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* len(re.sub(r"\W", "", text)) */
int WordChars(const QString &text)
{
	static const QRegularExpression nonWord(QStringLiteral("\\W"), kUnicode);
	QString s = text;
	s.remove(nonWord);
	return (int)s.size();
}

/* Python round(): half to even */
double PyRound(double v)
{
	return std::nearbyint(v);
}

Row TrimFarBoxes(const Row &row)
{
	std::vector<OcrBox> boxes = row.boxes;
	std::stable_sort(boxes.begin(), boxes.end(), [](const OcrBox &a, const OcrBox &b) { return a.x0 < b.x0; });
	std::vector<OcrBox> kept{boxes[0]};
	for (size_t i = 1; i < boxes.size(); i++) {
		const OcrBox &box = boxes[i];
		if (box.x0 - kept.back().x1 > 4 * (box.y1 - box.y0)) {
			break;
		}
		kept.push_back(box);
	}
	Row trimmed = Row::FromBox(kept[0]);
	for (size_t i = 1; i < kept.size(); i++) {
		trimmed.Add(kept[i]);
	}
	return trimmed;
}

/* OpenCV COLOR_BGR2HSV for 8-bit images (fixed point, hsv_shift 12) */
void BgrToHsv(const uint8_t *bgr, int &h, int &s, int &v)
{
	constexpr int shift = 12;
	int b = bgr[0], g = bgr[1], r = bgr[2];
	int vmax = std::max({r, g, b});
	int vmin = std::min({r, g, b});
	int diff = vmax - vmin;
	int sdiv = vmax ? (int)std::lround((255 << shift) / (double)vmax) : 0;
	int hdiv = diff ? (int)std::lround((180 << shift) / (6.0 * diff)) : 0;

	s = (diff * sdiv + (1 << (shift - 1))) >> shift;
	int hh;
	if (vmax == r) {
		hh = g - b;
	} else if (vmax == g) {
		hh = b - r + 2 * diff;
	} else {
		hh = r - g + 4 * diff;
	}
	hh = (hh * hdiv + (1 << (shift - 1))) >> shift;
	if (hh < 0) {
		hh += 180;
	}
	h = hh;
	v = vmax;
}

} // namespace

QString Row::Text() const
{
	std::vector<const OcrBox *> sorted;
	for (const OcrBox &b : boxes) {
		sorted.push_back(&b);
	}
	std::stable_sort(sorted.begin(), sorted.end(), [](const OcrBox *a, const OcrBox *b) { return a->x0 < b->x0; });
	QStringList parts;
	for (const OcrBox *b : sorted) {
		parts << QString::fromStdString(b->text);
	}
	return parts.join(' ').trimmed();
}

void Row::Add(const OcrBox &box)
{
	boxes.push_back(box);
	x0 = std::min(x0, box.x0);
	y0 = std::min(y0, box.y0);
	x1 = std::max(x1, box.x1);
	y1 = std::max(y1, box.y1);
}

QString ChatEntry::Text() const
{
	QStringList parts;
	for (const Row &r : rows) {
		parts << r.Text();
	}
	return parts.join(' ');
}

int MatchTimestamp(const QString &text, QString *hour, QString *minute, QString *second)
{
	QRegularExpressionMatch m = TimestampRe().match(text);
	if (!m.hasMatch()) {
		return -1;
	}
	if (hour) {
		*hour = m.captured(1);
	}
	if (minute) {
		*minute = m.captured(2);
	}
	if (second) {
		*second = m.captured(3);
	}
	return (int)m.capturedEnd(0);
}

std::vector<Row> GroupRows(const std::vector<OcrBox> &boxes)
{
	if (boxes.empty()) {
		return {};
	}
	std::vector<double> heights;
	for (const OcrBox &b : boxes) {
		heights.push_back(b.y1 - b.y0);
	}
	const double medH = Median(heights);

	std::vector<OcrBox> normal, tall;
	for (const OcrBox &b : boxes) {
		(b.y1 - b.y0 <= 1.7 * medH ? normal : tall).push_back(b);
	}
	std::stable_sort(normal.begin(), normal.end(),
			 [](const OcrBox &a, const OcrBox &b) { return (a.y0 + a.y1) / 2 < (b.y0 + b.y1) / 2; });

	std::vector<Row> rows;
	for (const OcrBox &box : normal) {
		bool placed = false;
		for (Row &row : rows) {
			float overlap = std::min(row.y1, box.y1) - std::max(row.y0, box.y0);
			if (overlap >= 0.5f * std::min(box.y1 - box.y0, row.Height())) {
				row.Add(box);
				placed = true;
				break;
			}
		}
		if (!placed) {
			rows.push_back(Row::FromBox(box));
		}
	}
	for (Row &row : rows) {
		row = TrimFarBoxes(row);
	}

	for (const OcrBox &box : tall) {
		if (WordChars(QString::fromStdString(box.text)) <= 4 || rows.empty()) {
			continue;
		}
		Row *best = &rows[0];
		float bestOverlap = std::min(best->y1, box.y1) - std::max(best->y0, box.y0);
		for (Row &row : rows) {
			float overlap = std::min(row.y1, box.y1) - std::max(row.y0, box.y0);
			if (overlap > bestOverlap) {
				best = &row;
				bestOverlap = overlap;
			}
		}
		best->boxes.push_back(box);
		best->x0 = std::min(best->x0, box.x0);
		best->x1 = std::max(best->x1, box.x1);
	}

	std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.Cy() < b.Cy(); });
	return rows;
}

bool IsMeaningful(const QString &text)
{
	if (WordChars(text) < 2) {
		return false;
	}
	QString low = text.toLower();
	int hits = 0;
	for (const char *word : kTabWords) {
		if (low.contains(QLatin1String(word))) {
			hits++;
		}
	}
	return hits < 3; /* the channel tab bar */
}

std::vector<Rect> RowRects(const std::vector<Row> &rows, int chatLeft, int topBound, int bottomBound)
{
	std::vector<Rect> rects;
	if (rows.empty()) {
		return rects;
	}
	std::vector<double> heights;
	for (const Row &r : rows) {
		heights.push_back(r.Height());
	}
	const double medH = Median(heights);

	std::vector<double> gaps;
	for (size_t i = 1; i < rows.size(); i++) {
		double gap = rows[i].Cy() - rows[i - 1].Cy();
		if (gap < 2.2 * medH) {
			gaps.push_back(gap);
		}
	}
	const double half = (gaps.empty() ? medH * 1.55 : Median(gaps)) / 2;

	for (size_t i = 0; i < rows.size(); i++) {
		const Row &row = rows[i];
		double top = row.Cy() - half, bottom = row.Cy() + half;
		if (i > 0) {
			top = std::max(top, (double)(rows[i - 1].Cy() + row.Cy()) / 2);
		}
		if (i + 1 < rows.size()) {
			bottom = std::min(bottom, (double)(rows[i + 1].Cy() + row.Cy()) / 2);
		}
		top = std::min(top, (double)row.y0 - 1);
		bottom = std::max(bottom, (double)row.y1 + 1);
		rects.push_back({chatLeft, std::max(topBound, (int)std::floor(top)),
				 (int)std::ceil(row.x1 + 0.4 * medH), std::min(bottomBound, (int)std::ceil(bottom))});
	}
	return rects;
}

QString NormaliseTag(const QString &tag)
{
	static const QRegularExpression nonTag(QStringLiteral("[^a-z0-9 ]"));
	static const QRegularExpression spaces(QStringLiteral("\\s+"), kUnicode);
	QString s = tag.toLower();
	s.replace(nonTag, QStringLiteral(" "));
	s.replace(spaces, QStringLiteral(" "));
	return s.trimmed();
}

ParsedBody ParseBody(const QString &text)
{
	ParsedBody result;
	QString h, m, s;
	int end = MatchTimestamp(text, &h, &m, &s);
	QString body = text;
	if (end >= 0) {
		result.time = QStringLiteral("%1:%2:%3").arg(h.toInt(), 2, 10, QChar('0')).arg(m, s);
		body = text.mid(end);
	}

	QString rest = body;
	while (result.tags.size() < 3) {
		QRegularExpressionMatch t = TagRe().match(rest);
		if (!t.hasMatch()) {
			break;
		}
		QString tag = NormaliseTag(t.captured(1));
		if (!tag.isEmpty()) {
			result.tags << tag;
		}
		rest = rest.mid(t.capturedEnd(0));
	}
	result.body = body.trimmed();
	return result;
}

QString ChannelOf(const QString &body, const QStringList &tagsIn, bool partial)
{
	static const QRegularExpression pm(QStringLiteral("\\bpm (to|from)\\b"), kUnicode);
	static const QRegularExpression says(QStringLiteral("\\bsays\\b"), kUnicode);

	QString low = body.toLower();
	QStringList tags;
	for (const QString &t : tagsIn) {
		if (!VolatileTagRe().match(t).hasMatch()) {
			tags << t;
		}
	}
	if (!tags.isEmpty()) {
		QString key = tags.mid(0, 2).join(QStringLiteral(" / "));
		if (pm.match(low).hasMatch()) {
			key += QStringLiteral(" / pm");
		}
		return key;
	}
	if (partial) {
		return QStringLiteral("continuation");
	}
	if (low.startsWith('*')) {
		return QStringLiteral("emote");
	}
	if (low.contains(QLatin1String("whisper"))) {
		return QStringLiteral("whisper");
	}
	if (says.match(low).hasMatch()) {
		return QStringLiteral("say");
	}
	if (low.startsWith(QLatin1String("(("))) {
		return QStringLiteral("ooc");
	}
	return QStringLiteral("other");
}

std::array<float, 13> ColourFeatures(const Image &img, const Rect &rect)
{
	std::array<float, 13> out{};
	int x0 = std::clamp(rect.x0, 0, img.width), x1 = std::clamp(rect.x1, 0, img.width);
	int y0 = std::clamp(rect.y0, 0, img.height), y1 = std::clamp(rect.y1, 0, img.height);
	if (x1 <= x0 || y1 <= y0) {
		return out;
	}

	std::array<long long, 12> hist{};
	long long text = 0, coloured = 0;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			int h, s, v;
			BgrToHsv(img.at(x, y), h, s, v);
			if (v > 150) {
				text++;
				if (s > 90) {
					coloured++;
					int bin = h * 12 / 180;
					if (bin < 12) {
						hist[bin]++;
					}
				}
			}
		}
	}
	const double total = (double)std::max(text, 1LL);
	for (int i = 0; i < 12; i++) {
		out[i] = (float)(PyRound(hist[i] / total * 1e4) / 1e4);
	}
	out[12] = (float)(PyRound(coloured / total * 1e4) / 1e4);
	return out;
}

std::vector<ChatEntry> BuildEntries(const Image &img, const std::vector<Row> &rows, const std::vector<Rect> &rects)
{
	std::vector<ChatEntry> entries;
	for (size_t i = 0; i < rows.size() && i < rects.size(); i++) {
		if (MatchTimestamp(rows[i].Text()) >= 0 || entries.empty()) {
			ChatEntry e;
			e.index = (int)entries.size();
			e.rows.push_back(rows[i]);
			e.rects.push_back(rects[i]);
			entries.push_back(std::move(e));
		} else {
			entries.back().rows.push_back(rows[i]);
			entries.back().rects.push_back(rects[i]);
		}
	}

	const ChatEntry *prev = nullptr;
	for (ChatEntry &entry : entries) {
		ParsedBody parsed = ParseBody(entry.Text());
		entry.time = parsed.time;
		entry.body = parsed.body;
		entry.tags = parsed.tags;
		entry.channel = ChannelOf(entry.body, entry.tags, entry.Partial());
		if (prev && entry.tags.isEmpty() && entry.time && prev->time && *entry.time == *prev->time &&
		    prev->body.trimmed().endsWith(':') && !prev->tags.isEmpty()) {
			entry.channel = prev->channel + QStringLiteral(" / message");
		}
		entry.colour = ColourFeatures(img, entry.rects[0]);
		prev = &entry;
	}
	return entries;
}

/* ------------------------------------------------------------------------- */

Rect Region::ToPixels(int width, int height) const
{
	int x0 = std::clamp((int)PyRound(left * width), 0, width);
	int y0 = std::clamp((int)PyRound(top * height), 0, height);
	int x1 = std::clamp((int)PyRound(right * width), 0, width);
	int y1 = std::clamp((int)PyRound(bottom * height), 0, height);
	return {x0, y0, std::max(x1, x0), std::max(y1, y0)};
}

Region Region::FromPixels(const Rect &rect, int width, int height)
{
	auto r4 = [](double v) {
		return PyRound(v * 1e4) / 1e4;
	};
	return {r4((double)rect.x0 / width), r4((double)rect.y0 / height), r4((double)rect.x1 / width),
		r4((double)rect.y1 / height)};
}

std::optional<long long> ParseUnixTimestamp(const QString &text)
{
	static const QRegularExpression spaces(QStringLiteral("\\s+"), kUnicode);
	static const QRegularExpression runs(QStringLiteral("\\d{10,}"), kUnicode);
	QString compact = text;
	compact.remove(spaces);

	QStringList found;
	auto it = runs.globalMatch(compact);
	while (it.hasNext()) {
		found << it.next().captured(0);
	}
	for (auto r = found.crbegin(); r != found.crend(); ++r) {
		QString last10 = r->right(10);
		long long value = 0;
		for (QChar c : last10) {
			value = value * 10 + c.digitValue();
		}
		if (value >= 1400000000LL && value <= 2200000000LL) {
			return value;
		}
	}
	return std::nullopt;
}

std::optional<long long> ReadUnixTimestamp(const Image &img, const Region &region, OcrEngine &ocr, QString *textOut)
{
	Rect r = region.ToPixels(img.width, img.height);
	Image crop = Crop(img, r.x0, r.y0, r.x1, r.y1);
	QString text = QString::fromStdString(ocr.RecognizeLine(crop));
	std::optional<long long> ts = ParseUnixTimestamp(text);
	if (!ts) {
		/* slower fallback: the text may not fill the region, so detect it first */
		std::vector<OcrBox> boxes = ocr.Read(crop, 0.2f);
		std::stable_sort(boxes.begin(), boxes.end(),
				 [](const OcrBox &a, const OcrBox &b) { return a.x0 < b.x0; });
		QStringList parts;
		for (const OcrBox &b : boxes) {
			parts << QString::fromStdString(b.text);
		}
		text = parts.join(' ');
		ts = ParseUnixTimestamp(text);
	}
	if (textOut) {
		*textOut = text;
	}
	return ts;
}

Analysis Analyze(const Image &img, OcrEngine &ocr, const Region &chatRegion, const Region &hudRegion, bool readHud,
		 bool regionOverride)
{
	Analysis a;
	a.chatRect = chatRegion.ToPixels(img.width, img.height);
	const Rect &c = a.chatRect;
	Image crop = Crop(img, c.x0, c.y0, c.x1, c.y1);

	std::vector<OcrBox> boxes;
	for (const OcrBox &b : ocr.Read(crop, 0.3f)) {
		boxes.push_back(b.Shifted((float)c.x0, (float)c.y0));
	}

	std::vector<Row> rows;
	for (Row &r : GroupRows(boxes)) {
		if (IsMeaningful(r.Text())) {
			rows.push_back(std::move(r));
		}
	}
	a.entries = BuildEntries(img, rows, RowRects(rows, c.x0, c.y0, c.y1));

	if (readHud) {
		a.unixTs = ReadUnixTimestamp(img, hudRegion, ocr, &a.hudText);
		if (!a.unixTs) {
			a.notes << QStringLiteral("No timestamp found at the top of the screen (read: '%1')")
					   .arg(a.hudText);
		}
	}
	if (a.entries.empty()) {
		a.notes << (regionOverride ? QStringLiteral("No chat lines detected")
					   : QStringLiteral("No chat lines detected in the chat region"));
	}
	return a;
}

} // namespace spectra
