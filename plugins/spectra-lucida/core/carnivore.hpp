#pragma once

#include "profiles.hpp"
#include "store.hpp"

#include <spectra-vision/chat.hpp>
#include <spectra-vision/image.hpp>
#include <spectra-vision/ocr.hpp>

#include <vector>

namespace lucida {

/* Carnivore mode: Lucida reads all the text on screen instead of only the
 * chat box. The frame is OCRed whole, the text is split into blocks (a chat
 * box, a kill feed, a notification...), each block is read as chat and
 * tracked as a screen region, so its lines can be searched and filtered by
 * where on screen they were. */

/* Boxes that read as one block of text: words on a line, and lines stacked
 * with their left, right or centre edges aligned */
struct TextBlock {
	spectra::Rect rect;
	std::vector<spectra::OcrBox> boxes;
};
std::vector<TextBlock> FindTextBlocks(const std::vector<spectra::OcrBox> &boxes);

/* What a block of rows is: nothing to log (only numbers: health, speed,
 * money), labels (street names, buttons, headings: no row with three
 * words) or text (a row with three words or a timestamp) */
enum class BlockKind { None, Labels, Text };
BlockKind Classify(const std::vector<spectra::Row> &rows);

/* The region of label blocks; they are logged and searchable, but are not
 * tracked as regions of their own */
inline const QString kOtherRegion = QStringLiteral("other");

/* Rows of a block into chat entries. With timestamps, Lucida's wrap rules
 * apply; without, every row is its own entry unless it starts in lower
 * case right under a row that ran to the block's wrap point. */
std::vector<spectra::ChatEntry> ReadBlock(const spectra::Image &img, const std::vector<spectra::Row> &rows,
					  const spectra::Rect &rect);

/* Where a block's lines go */
struct Placement {
	QString name;
	RegionMode mode = RegionMode::Read;
};

/* Gives blocks stable region names across frames and sessions.
 *
 * Drawn regions (the region editor's, and the chat box as "chat") come
 * first: a block mostly inside one takes its name and mode, and drawn
 * regions never move. Otherwise a block belongs to the learned region it
 * mostly overlaps (either one inside the other, as chat grows and shrinks)
 * and widens it, or becomes a new learned region named after where it is
 * on screen ("top-left", "centre", "bottom"; "right 2" if taken). With
 * onlyDrawn, nothing is learned and such blocks are "other". */
class RegionTracker {
public:
	explicit RegionTracker(std::vector<ScreenRegion> learned = {}, std::vector<RegionRule> drawn = {},
			       bool onlyDrawn = false);

	/* The region of a block in a width x height frame; with learn false
	 * (labels), only drawn regions are considered, else it is "other" */
	Placement Assign(const spectra::Rect &block, int width, int height, double now, bool learn = true);
	/* Learned regions created or changed since the last call */
	std::vector<ScreenRegion *> TakeDirty();
	const std::vector<ScreenRegion> &Regions() const { return regions; }
	const std::vector<RegionRule> &Drawn() const { return drawn; }
	/* The drawn regions set to Ignore, in pixels */
	std::vector<spectra::Rect> Ignored(int width, int height) const;

	static QString PlaceName(const spectra::Region &area);

private:
	std::vector<ScreenRegion> regions;
	std::vector<RegionRule> drawn;
	bool onlyDrawn;
	std::vector<size_t> dirty;
};

/* The drawn regions plus the chat box as the region "chat" (unless one is
 * drawn with that name) */
std::vector<RegionRule> WithChatBox(std::vector<RegionRule> drawn, const spectra::Region &chatBox);

/* Reads every text block in the frame; boxes centred inside `exclude`
 * (e.g. the HUD clock) are skipped. Entries are in screen order (top to
 * bottom, then left to right) and carry their region's name; each row of
 * a label block is an entry of its own, in a drawn region or "other".
 * Text in ignored regions is skipped. blocks gets how many text blocks
 * were read. */
std::vector<spectra::ChatEntry> ReadScreen(const spectra::Image &img, spectra::OcrEngine &ocr, RegionTracker &tracker,
					   double now, const std::vector<spectra::Rect> &exclude = {},
					   int *blocks = nullptr);

} // namespace lucida
