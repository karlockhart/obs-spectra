#pragma once

#include "export.hpp"
#include "image.hpp"
#include "ocr.hpp"

#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <vector>

namespace spectra {

/* x0, y0, x1, y1 in image pixels */
struct Rect {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;

	bool operator==(const Rect &o) const = default;
};

/* One visual line of chat: the OCR boxes on it (port of obscura.chat.Row) */
struct Row {
	std::vector<OcrBox> boxes;
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;

	float Cy() const { return (y0 + y1) / 2; }
	float Height() const { return y1 - y0; }
	SPECTRA_VISION_API QString Text() const;
	SPECTRA_VISION_API void Add(const OcrBox &box);

	static Row FromBox(const OcrBox &box)
	{
		Row r;
		r.boxes.push_back(box);
		r.x0 = box.x0;
		r.y0 = box.y0;
		r.x1 = box.x1;
		r.y1 = box.y1;
		return r;
	}
};

/* One chat message, possibly wrapped over several rows (obscura.chat.ChatEntry) */
struct ChatEntry {
	int index = 0;
	std::vector<Row> rows;
	std::vector<Rect> rects;
	std::optional<QString> time; /* "HH:MM:SS" */
	QString body;
	QStringList tags;
	QString channel = QStringLiteral("other");
	std::array<float, 13> colour{};
	/* Lucida's carnivore mode: the screen region it was read from */
	QString region;

	/* The first line scrolled off the top, so there is no timestamp */
	bool Partial() const { return !time.has_value(); }
	SPECTRA_VISION_API QString Text() const;
};

/* TIMESTAMP_RE.match: returns the end of the match, or -1. Captures h/m/s. */
SPECTRA_VISION_API int MatchTimestamp(const QString &text, QString *hour = nullptr, QString *minute = nullptr,
				      QString *second = nullptr);

SPECTRA_VISION_API std::vector<Row> GroupRows(const std::vector<OcrBox> &boxes);
SPECTRA_VISION_API bool IsMeaningful(const QString &text);
SPECTRA_VISION_API std::vector<Rect> RowRects(const std::vector<Row> &rows, int chatLeft, int topBound,
					      int bottomBound);
SPECTRA_VISION_API QString NormaliseTag(const QString &tag);

struct ParsedBody {
	std::optional<QString> time;
	QString body;
	QStringList tags;
};
SPECTRA_VISION_API ParsedBody ParseBody(const QString &text);

SPECTRA_VISION_API QString ChannelOf(const QString &body, const QStringList &tags, bool partial);

/* Hue histogram of bright, saturated pixels (12 bins) + coloured fraction */
SPECTRA_VISION_API std::array<float, 13> ColourFeatures(const Image &img, const Rect &rect);

SPECTRA_VISION_API std::vector<ChatEntry> BuildEntries(const Image &img, const std::vector<Row> &rows,
						       const std::vector<Rect> &rects);

/* Region of the screen as fractions (obscura.config.Region) */
struct Region {
	double left = 0.0;
	double top = 0.0;
	double right = 1.0;
	double bottom = 1.0;

	/* Python round() (half to even), clamped, x1 >= x0, y1 >= y0 */
	SPECTRA_VISION_API Rect ToPixels(int width, int height) const;
	SPECTRA_VISION_API static Region FromPixels(const Rect &rect, int width, int height);
};

/* Default FiveM chat and HUD clock regions */
constexpr Region kDefaultChatRegion{0.008, 0.015, 0.41, 0.335};
constexpr Region kDefaultHudRegion{0.44, 0.0, 0.56, 0.028};

/* HUD unix clock ("30 | 1789231305"): rightmost 10+ digit run's last 10
 * digits, within [1400000000, 2200000000] */
SPECTRA_VISION_API std::optional<long long> ParseUnixTimestamp(const QString &text);

/* read_unix_timestamp: recognition-only first, detection fallback */
SPECTRA_VISION_API std::optional<long long> ReadUnixTimestamp(const Image &img, const Region &region, OcrEngine &ocr,
							      QString *textOut = nullptr);

/* obscura.pipeline.analyze without the learner */
struct Analysis {
	std::vector<ChatEntry> entries;
	Rect chatRect;
	std::optional<long long> unixTs;
	QString hudText;
	QStringList notes;
};
SPECTRA_VISION_API Analysis Analyze(const Image &img, OcrEngine &ocr, const Region &chatRegion, const Region &hudRegion,
				    bool readHud, bool regionOverride = false);

} // namespace spectra
