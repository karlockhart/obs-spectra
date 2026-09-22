#pragma once

#include "recorder.hpp"

#include <QRegularExpression>
#include <QString>

#include <optional>

struct gs_texture_render;
struct gs_stage_surface;

namespace lucida {

/* Reads frames of the game from OBS: the first visible game/window capture
 * source that is hooked onto a process matching the target pattern, rendered
 * at its native size (independent of the canvas or scene layout). */
class FrameGrabber {
public:
	FrameGrabber();
	~FrameGrabber();

	void SetTargetProcess(const QString &regex);

	/* Called from the sampling thread */
	std::optional<GrabbedFrame> Grab();

private:
	QRegularExpression target;
	gs_texture_render *texrender = nullptr;
	gs_stage_surface *stage = nullptr;
	uint32_t stageWidth = 0;
	uint32_t stageHeight = 0;

	void FreeGraphics();
};

} // namespace lucida
