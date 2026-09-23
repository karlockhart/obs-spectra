#include <spectra-grab/frame-grabber.hpp>

#include <obs.hpp>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

namespace spectra {

namespace {

struct Candidate {
	OBSSource source;
	QString name;
	QString exe;
};

bool HookedExecutable(obs_source_t *source, QString &exe)
{
	calldata_t cd = {0};
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	bool hooked = false;
	if (proc_handler_call(ph, "get_hooked", &cd)) {
		hooked = calldata_bool(&cd, "hooked");
		const char *e = calldata_string(&cd, "executable");
		exe = QString::fromUtf8(e ? e : "");
	}
	calldata_free(&cd);
	return hooked && obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0;
}

} // namespace

FrameGrabber::FrameGrabber()
{
	SetTargetProcess(QStringLiteral("FiveM.*GTAProcess\\.exe"));
}

FrameGrabber::~FrameGrabber()
{
	FreeGraphics();
}

void FrameGrabber::SetTargetProcess(const QString &regex)
{
	target = QRegularExpression(regex, QRegularExpression::CaseInsensitiveOption);
}

void FrameGrabber::FreeGraphics()
{
	if (!texrender && !stage) {
		return;
	}
	obs_enter_graphics();
	gs_texrender_destroy(texrender);
	gs_stagesurface_destroy(stage);
	obs_leave_graphics();
	texrender = nullptr;
	stage = nullptr;
	stageWidth = stageHeight = 0;
}

std::optional<SourceFrame> FrameGrabber::Grab()
{
	/* Find a capture showing the game */
	struct Context {
		const QRegularExpression *target;
		Candidate found;
	} ctx{&target, {}};

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<Context *>(param);
			const char *id = obs_source_get_unversioned_id(source);
			if (strcmp(id, "game_capture") != 0 && strcmp(id, "window_capture") != 0) {
				return true;
			}
			if (!obs_source_showing(source)) {
				return true;
			}
			QString exe;
			if (!HookedExecutable(source, exe)) {
				return true;
			}
			if (c->target->isValid() && !c->target->pattern().isEmpty() &&
			    !c->target->match(exe).hasMatch()) {
				return true;
			}
			c->found = {OBSSource(source), QString::fromUtf8(obs_source_get_name(source)), exe};
			return false;
		},
		&ctx);

	if (!ctx.found.source) {
		return std::nullopt;
	}

	obs_source_t *source = ctx.found.source;
	const uint32_t cx = obs_source_get_width(source);
	const uint32_t cy = obs_source_get_height(source);
	if (!cx || !cy) {
		return std::nullopt;
	}

	/* Render the source into a texture and copy it to a staging surface,
	 * as OBS's screenshot code does */
	obs_enter_graphics();
	if (!texrender) {
		texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	}
	if (!stage || stageWidth != cx || stageHeight != cy) {
		gs_stagesurface_destroy(stage);
		stage = gs_stagesurface_create(cx, cy, GS_BGRA);
		stageWidth = cx;
		stageHeight = cy;
	}

	bool rendered = false;
	gs_texrender_reset(texrender);
	if (gs_texrender_begin(texrender, cx, cy)) {
		vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_inc_showing(source);
		obs_source_video_render(source);
		obs_source_dec_showing(source);
		gs_blend_state_pop();
		gs_texrender_end(texrender);
		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		rendered = true;
	}

	std::optional<SourceFrame> frame;
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (rendered && gs_stagesurface_map(stage, &data, &linesize)) {
		frame = SourceFrame{FromBGRA(data, (int)cx, (int)cy, (int)linesize), ctx.found.name, ctx.found.exe};
		gs_stagesurface_unmap(stage);
	}
	obs_leave_graphics();
	return frame;
}

} // namespace spectra
