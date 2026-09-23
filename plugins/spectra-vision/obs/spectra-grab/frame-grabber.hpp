#pragma once

#include <spectra-vision/image.hpp>

#include <QRegularExpression>
#include <QString>

#include <optional>

struct gs_texture_render;
struct gs_stage_surface;

namespace spectra {

struct SourceFrame {
	Image image;        /* BGR */
	QString source;     /* OBS source name */
	QString executable; /* hooked process */
};

/* Reads frames of the game from OBS (shared by Lucida and Obscura): the first visible game/window capture
 * source that is hooked onto a process matching the target pattern, rendered
 * at its native size (independent of the canvas or scene layout). */
class FrameGrabber {
public:
	FrameGrabber();
	~FrameGrabber();

	void SetTargetProcess(const QString &regex);

	/* Call from a worker thread (not the graphics thread) */
	std::optional<SourceFrame> Grab();

private:
	QRegularExpression target;
	gs_texture_render *texrender = nullptr;
	gs_stage_surface *stage = nullptr;
	uint32_t stageWidth = 0;
	uint32_t stageHeight = 0;

	void FreeGraphics();
};

} // namespace spectra
