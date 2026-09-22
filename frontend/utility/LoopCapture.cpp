#include "LoopCapture.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QRegularExpression>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <util/windows/window-helpers.h>
#endif

/* Private-settings tag identifying the sources Spectra manages, so they are
 * found again even if the user renames them. */
#define CAPTURE_TAG "spectra_capture"
#define TAG_GAME "game"
#define TAG_WINDOW "window"

/* Seconds game capture may try to hook before window capture is used. */
static constexpr int FALLBACK_SECONDS = 15;
/* Seconds a newly shown window capture gets to start before it counts as
 * not capturing. */
static constexpr int WINDOW_GRACE_SECONDS = 6;

/* win-capture setting values */
static constexpr int PRIORITY_EXE = 2;
static constexpr int METHOD_WGC = 2;

namespace {

struct GameWindow {
	QString title;
	QString windowClass;
	QString exe;
	qint64 area = 0;

	bool Valid() const { return !exe.isEmpty(); }
};

bool MatchesAny(const QString &exe, const QStringList &patterns)
{
	for (const QString &pattern : patterns) {
		QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
				      QRegularExpression::CaseInsensitiveOption);
		if (re.match(exe).hasMatch()) {
			return true;
		}
	}
	return false;
}

#ifdef _WIN32
struct FindContext {
	const QStringList *patterns;
	DWORD selfPid;
	GameWindow best;
};

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param)
{
	FindContext *ctx = reinterpret_cast<FindContext *>(param);

	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER)) {
		return TRUE;
	}
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
		return TRUE;
	}

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == ctx->selfPid) {
		return TRUE;
	}

	RECT rect;
	if (!GetClientRect(hwnd, &rect)) {
		return TRUE;
	}
	qint64 area = (qint64)(rect.right - rect.left) * (rect.bottom - rect.top);
	if (area <= ctx->best.area) {
		return TRUE;
	}

	struct dstr exe = {0};
	if (!ms_get_window_exe(&exe, hwnd)) {
		return TRUE;
	}
	QString exeName = QString::fromUtf8(exe.array);
	dstr_free(&exe);
	if (!MatchesAny(exeName, *ctx->patterns)) {
		return TRUE;
	}

	struct dstr title = {0};
	struct dstr windowClass = {0};
	ms_get_window_title(&title, hwnd);
	ms_get_window_class(&windowClass, hwnd);
	ctx->best = {QString::fromUtf8(title.array ? title.array : ""),
		     QString::fromUtf8(windowClass.array ? windowClass.array : ""), exeName, area};
	dstr_free(&title);
	dstr_free(&windowClass);
	return TRUE;
}
#endif

/* The game's main window: the largest visible top-level window of a process
 * matching the patterns (so the game wins over its launcher). */
GameWindow FindGameWindow(const QStringList &patterns)
{
#ifdef _WIN32
	FindContext ctx = {&patterns, GetCurrentProcessId(), {}};
	EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&ctx));
	return ctx.best;
#else
	UNUSED_PARAMETER(patterns);
	return {};
#endif
}

QString EncodeWindowPart(QString str)
{
	str.replace("#", "#22");
	str.replace(":", "#3A");
	return str;
}

/* "title:class:exe" as stored in win-capture's "window" setting */
QString WindowString(const GameWindow &window)
{
	return EncodeWindowPart(window.title) + ":" + EncodeWindowPart(window.windowClass) + ":" +
	       EncodeWindowPart(window.exe);
}

bool SourceHooked(obs_source_t *source, QString *exe = nullptr)
{
	calldata_t cd = {0};
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	bool hooked = false;
	if (proc_handler_call(ph, "get_hooked", &cd)) {
		hooked = calldata_bool(&cd, "hooked");
		if (exe) {
			const char *name = calldata_string(&cd, "executable");
			*exe = QString::fromUtf8(name ? name : "");
		}
	}
	calldata_free(&cd);
	return hooked && obs_source_get_width(source) > 0;
}

QString CaptureTag(obs_source_t *source)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	return QString::fromUtf8(obs_data_get_string(priv, CAPTURE_TAG));
}

OBSSourceAutoRelease FindTagged(const char *tag)
{
	struct Context {
		const char *tag;
		obs_source_t *found;
	} ctx = {tag, nullptr};

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			Context *c = static_cast<Context *>(param);
			if (CaptureTag(source) == c->tag) {
				c->found = obs_source_get_ref(source);
				return false;
			}
			return true;
		},
		&ctx);

	return ctx.found;
}

/* A capture the user set up themselves that is already showing the game */
bool ExistingCaptureHooked(obs_scene_t *scene, const QStringList &patterns)
{
	struct Context {
		const QStringList *patterns;
		bool found;
	} ctx = {&patterns, false};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			Context *c = static_cast<Context *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *id = obs_source_get_unversioned_id(source);
			if (!obs_sceneitem_visible(item) || !CaptureTag(source).isEmpty()) {
				return true;
			}
			if (strcmp(id, "game_capture") != 0 && strcmp(id, "window_capture") != 0) {
				return true;
			}
			QString exe;
			if (SourceHooked(source, &exe) && MatchesAny(exe, *c->patterns)) {
				c->found = true;
				return false;
			}
			return true;
		},
		&ctx);

	return ctx.found;
}

QString UniqueSourceName(const QString &base)
{
	QString name = base;
	for (int n = 2;; n++) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(QT_TO_UTF8(name));
		if (!existing) {
			return name;
		}
		name = QStringLiteral("%1 %2").arg(base).arg(n);
	}
}

OBSSourceAutoRelease CreateCapture(const char *id, const QString &baseName, const char *tag, const QString &window)
{
	QByteArray nameUtf8 = UniqueSourceName(baseName).toUtf8();
	const char *name = nameUtf8.constData();

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "window", QT_TO_UTF8(window));
	obs_data_set_int(settings, "priority", PRIORITY_EXE);
	/* Record the game's own audio (application audio capture) */
	obs_data_set_bool(settings, "capture_audio", true);

	if (strcmp(id, "game_capture") == 0) {
		obs_data_set_string(settings, "capture_mode", "window");
		obs_data_set_bool(settings, "capture_cursor", false);
	} else {
		obs_data_set_int(settings, "method", METHOD_WGC);
		obs_data_set_bool(settings, "cursor", false);
		obs_data_set_bool(settings, "client_area", true);
	}

	OBSSourceAutoRelease source = obs_source_create(id, name, settings, nullptr);
	if (source) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(source);
		obs_data_set_string(priv, CAPTURE_TAG, tag);
		blog(LOG_INFO, "[Spectra] Created '%s' for loop recording", name);
	}
	return source;
}

void SetCaptureWindow(obs_source_t *source, const QString &window)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	if (QString::fromUtf8(obs_data_get_string(settings, "window")) == window) {
		return;
	}
	OBSDataAutoRelease update = obs_data_create();
	obs_data_set_string(update, "window", QT_TO_UTF8(window));
	obs_data_set_int(update, "priority", PRIORITY_EXE);
	obs_source_update(source, update);
}

/* Returns the scene item showing `source` in `scene`, adding it scaled to
 * fit the canvas at the bottom of the scene if needed. */
obs_sceneitem_t *EnsureInScene(obs_scene_t *scene, obs_source_t *source)
{
	obs_sceneitem_t *item = obs_scene_find_source(scene, obs_source_get_name(source));
	if (item) {
		return item;
	}

	item = obs_scene_add(scene, source);
	if (!item) {
		return nullptr;
	}

	obs_video_info ovi;
	obs_get_video_info(&ovi);
	vec2 bounds;
	vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
	vec2 pos;
	vec2_set(&pos, 0.0f, 0.0f);

	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);
	return item;
}

void SetItemVisible(obs_scene_t *scene, obs_source_t *source, bool visible)
{
	if (!source) {
		return;
	}
	obs_sceneitem_t *item = obs_scene_find_source(scene, obs_source_get_name(source));
	if (item && obs_sceneitem_visible(item) != visible) {
		obs_sceneitem_set_visible(item, visible);
	}
}

} // namespace

LoopCapture::LoopCapture(OBSBasic *main_) : QObject(main_), main(main_) {}

void LoopCapture::Reset()
{
	hookingSince = QDateTime();
	fallbackSince = QDateTime();
	SetState(State::Idle);
}

void LoopCapture::Update(const QStringList &patterns)
{
	if (patterns.isEmpty()) {
		SetState(State::Idle);
		return;
	}

	OBSScene scene = main->GetProgramScene();
	if (!scene) {
		return;
	}

	OBSSourceAutoRelease game = FindTagged(TAG_GAME);
	OBSSourceAutoRelease window = FindTagged(TAG_WINDOW);

	if (ExistingCaptureHooked(scene, patterns)) {
		SetItemVisible(scene, game, false);
		SetItemVisible(scene, window, false);
		hookingSince = QDateTime();
		SetState(State::ExistingCapture);
		return;
	}

	GameWindow target = FindGameWindow(patterns);
	if (!target.Valid()) {
		SetState(State::WaitingForWindow);
		return;
	}

	QString targetString = WindowString(target);
	if (targetString != windowString) {
		if (!windowString.isEmpty()) {
			blog(LOG_INFO, "[Spectra] Game window is now '%s' (%s)", QT_TO_UTF8(target.title),
			     QT_TO_UTF8(target.exe));
		}
		windowString = targetString;
		windowExe = target.exe;
		hookingSince = QDateTime::currentDateTime();
		fallbackSince = QDateTime();
	}

	if (!game) {
		game = CreateCapture("game_capture", QTStr("Spectra.Capture.GameSource"), TAG_GAME, windowString);
		if (!game) {
			SetState(State::NotCapturing);
			return;
		}
	}
	SetCaptureWindow(game, windowString);
	if (window) {
		SetCaptureWindow(window, windowString);
	}

	obs_sceneitem_t *gameItem = EnsureInScene(scene, game);
	if (gameItem && !obs_sceneitem_visible(gameItem)) {
		obs_sceneitem_set_visible(gameItem, true);
	}

	if (SourceHooked(game)) {
		SetItemVisible(scene, window, false);
		hookingSince = QDateTime();
		fallbackSince = QDateTime();
		SetState(State::GameCapture);
		return;
	}

	if (!hookingSince.isValid()) {
		hookingSince = QDateTime::currentDateTime();
	}
	if (hookingSince.secsTo(QDateTime::currentDateTime()) < FALLBACK_SECONDS) {
		SetState(State::Hooking);
		return;
	}

	/* Game capture could not hook: fall back to Windows Graphics Capture. */
	if (!window) {
		window = CreateCapture("window_capture", QTStr("Spectra.Capture.WindowSource"), TAG_WINDOW,
				       windowString);
	}
	if (window) {
		EnsureInScene(scene, window);
		SetItemVisible(scene, window, true);
		if (!fallbackSince.isValid()) {
			fallbackSince = QDateTime::currentDateTime();
		}
	}

	if (window && SourceHooked(window)) {
		SetState(State::WindowCapture);
	} else if (window && fallbackSince.secsTo(QDateTime::currentDateTime()) < WINDOW_GRACE_SECONDS) {
		SetState(State::Hooking);
	} else {
		SetState(State::NotCapturing);
	}
}

void LoopCapture::SetState(State newState)
{
	if (state == newState) {
		return;
	}

	state = newState;
	switch (state) {
	case State::GameCapture:
		blog(LOG_INFO, "[Spectra] Capturing '%s' with game capture", QT_TO_UTF8(windowExe));
		break;
	case State::WindowCapture:
		blog(LOG_INFO, "[Spectra] Game capture did not hook '%s', using window capture", QT_TO_UTF8(windowExe));
		break;
	case State::ExistingCapture:
		blog(LOG_INFO, "[Spectra] Game is already captured by an existing source");
		break;
	case State::NotCapturing:
		blog(LOG_WARNING, "[Spectra] Unable to capture '%s'", QT_TO_UTF8(windowExe));
		break;
	default:
		break;
	}
	emit stateChanged(state);
}

QString LoopCapture::StatusText() const
{
	switch (state) {
	case State::WaitingForWindow:
		return QTStr("Spectra.Capture.Status.Waiting");
	case State::Hooking:
		return QTStr("Spectra.Capture.Status.Hooking").arg(windowExe);
	case State::GameCapture:
		return QTStr("Spectra.Capture.Status.Game").arg(windowExe);
	case State::WindowCapture:
		return QTStr("Spectra.Capture.Status.Window").arg(windowExe);
	case State::ExistingCapture:
		return QTStr("Spectra.Capture.Status.Existing");
	case State::NotCapturing:
		return QTStr("Spectra.Capture.Status.None");
	case State::Idle:
		break;
	}
	return QTStr("Spectra.Capture.Status.Idle");
}
