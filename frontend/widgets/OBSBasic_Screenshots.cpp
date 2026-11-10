/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <utility/ScreenshotObj.hpp>

#include <qt-wrappers.hpp>

/* The largest game or window capture in the program output that is hooked
 * onto a window. Its own size is the game's resolution, which the canvas
 * (and so a screenshot of the output) is often smaller than. */
static OBSSource ProgramGameCapture()
{
	struct Context {
		OBSSource best;
		uint64_t area = 0;
	} ctx;

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<Context *>(param);
			const char *id = obs_source_get_unversioned_id(source);
			if (strcmp(id, "game_capture") != 0 && strcmp(id, "window_capture") != 0) {
				return true;
			}
			if (!obs_source_active(source)) {
				return true;
			}
			calldata_t cd = {0};
			bool hooked = false;
			if (proc_handler_call(obs_source_get_proc_handler(source), "get_hooked", &cd)) {
				hooked = calldata_bool(&cd, "hooked");
			}
			calldata_free(&cd);
			const uint64_t area = (uint64_t)obs_source_get_width(source) * obs_source_get_height(source);
			if (hooked && area > c->area) {
				c->best = OBSSource(source);
				c->area = area;
			}
			return true;
		},
		&ctx);
	return ctx.best;
}

void OBSBasic::Screenshot(OBSSource source)
{
	if (!!screenshotData) {
		blog(LOG_WARNING, "Cannot take new screenshot, "
				  "screenshot currently in progress");
		return;
	}

	screenshotData = new ScreenshotObj(source);
}

void OBSBasic::ScreenshotGame()
{
	if (OBSSource game = ProgramGameCapture()) {
		blog(LOG_INFO, "Screenshot of '%s' at %ux%u", obs_source_get_name(game), obs_source_get_width(game),
		     obs_source_get_height(game));
		Screenshot(game);
	} else {
		Screenshot();
	}
}

void OBSBasic::ScreenshotSelectedSource()
{
	OBSSceneItem item = GetCurrentSceneItem();
	if (item) {
		Screenshot(obs_sceneitem_get_source(item));
	} else {
		blog(LOG_INFO, "Could not take a source screenshot: "
			       "no source selected");
	}
}

void OBSBasic::ScreenshotProgram()
{
	Screenshot(GetProgramSource());
}

void OBSBasic::ScreenshotScene()
{
	Screenshot(GetCurrentSceneSource());
}
