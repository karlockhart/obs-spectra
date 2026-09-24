#include "SpectraOverlay.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.hpp>
#include <util/config-file.h>

#include <QFontMetrics>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#define OVERLAY_SECTION "SpectraOverlay"

namespace {

constexpr int kCardWidth = 360;
constexpr int kCardHeight = 64;
constexpr int kGap = 10;
constexpr int kMargin = 28;
constexpr int kMaxToasts = 4;
constexpr int kEnterMs = 240;
constexpr int kLeaveMs = 280;

/* The splash screen's spectrum */
const QColor kSpectrum[] = {QColor(0xe6, 0x32, 0x32), QColor(0xff, 0x9f, 0x1a), QColor(0xff, 0xe2, 0x3a),
			    QColor(0x3e, 0xd2, 0x7a), QColor(0x5a, 0xc8, 0xfa), QColor(0x9b, 0x5c, 0xf6)};

QPointer<SpectraOverlay> instance;

QColor Accent(SpectraOverlay::Kind kind)
{
	using Kind = SpectraOverlay::Kind;
	switch (kind) {
	case Kind::Loop:
	case Kind::Error:
		return kSpectrum[0];
	case Kind::Upload:
		return kSpectrum[1];
	case Kind::Warning:
		return kSpectrum[2];
	case Kind::Obscura:
		return kSpectrum[3];
	case Kind::Screenshot:
	case Kind::Info:
		return kSpectrum[4];
	case Kind::Clip:
		return kSpectrum[5];
	case Kind::LoopStop:
		return QColor(0x8a, 0x8f, 0x99);
	}
	return kSpectrum[4];
}

void SpectrumStops(QLinearGradient &g, int alpha = 255)
{
	const int n = (int)std::size(kSpectrum);
	for (int i = 0; i < n; i++) {
		QColor c = kSpectrum[i];
		c.setAlpha(alpha);
		g.setColorAt((double)i / (n - 1), c);
	}
}

double EaseOut(double t)
{
	t = std::clamp(t, 0.0, 1.0);
	return 1.0 - std::pow(1.0 - t, 3.0);
}

void DrawGlyph(QPainter &p, SpectraOverlay::Kind kind, const QPointF &c, const QColor &color, qint64 now)
{
	using Kind = SpectraOverlay::Kind;
	QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);

	switch (kind) {
	case Kind::Loop: {
		/* a recording dot with a slow pulse */
		double pulse = 0.5 + 0.5 * std::sin(now / 260.0);
		QColor ring = color;
		ring.setAlphaF(0.35 + 0.45 * pulse);
		p.setPen(QPen(ring, 2.0));
		p.drawEllipse(c, 10.0, 10.0);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(c, 6.0, 6.0);
		break;
	}
	case Kind::LoopStop:
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawRoundedRect(QRectF(c.x() - 7, c.y() - 7, 14, 14), 3, 3);
		break;
	case Kind::Clip: {
		p.drawRoundedRect(QRectF(c.x() - 11, c.y() - 8, 22, 16), 3, 3);
		QPainterPath play;
		play.moveTo(c.x() - 3, c.y() - 4.5);
		play.lineTo(c.x() + 5, c.y());
		play.lineTo(c.x() - 3, c.y() + 4.5);
		play.closeSubpath();
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawPath(play);
		break;
	}
	case Kind::Screenshot: {
		QPainterPath body;
		body.addRoundedRect(QRectF(c.x() - 11, c.y() - 6, 22, 15), 3, 3);
		body.addRect(QRectF(c.x() - 4, c.y() - 9, 8, 3));
		p.drawPath(body.simplified());
		p.drawEllipse(QPointF(c.x(), c.y() + 1.5), 4.0, 4.0);
		break;
	}
	case Kind::Obscura: {
		QPainterPath eye;
		eye.moveTo(c.x() - 12, c.y());
		eye.quadTo(c.x(), c.y() - 11, c.x() + 12, c.y());
		eye.quadTo(c.x(), c.y() + 11, c.x() - 12, c.y());
		p.drawPath(eye);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(c, 3.5, 3.5);
		break;
	}
	case Kind::Upload:
		p.drawLine(QPointF(c.x(), c.y() + 5), QPointF(c.x(), c.y() - 8));
		p.drawLine(QPointF(c.x() - 5, c.y() - 3), QPointF(c.x(), c.y() - 8));
		p.drawLine(QPointF(c.x() + 5, c.y() - 3), QPointF(c.x(), c.y() - 8));
		p.drawLine(QPointF(c.x() - 9, c.y() + 9), QPointF(c.x() + 9, c.y() + 9));
		break;
	case Kind::Warning:
	case Kind::Error: {
		QPainterPath tri;
		tri.moveTo(c.x(), c.y() - 10);
		tri.lineTo(c.x() + 11, c.y() + 9);
		tri.lineTo(c.x() - 11, c.y() + 9);
		tri.closeSubpath();
		p.drawPath(tri);
		p.drawLine(QPointF(c.x(), c.y() - 3), QPointF(c.x(), c.y() + 2));
		p.drawPoint(QPointF(c.x(), c.y() + 6));
		break;
	}
	case Kind::Info:
		p.drawEllipse(c, 10.0, 10.0);
		p.drawLine(QPointF(c.x(), c.y() - 1), QPointF(c.x(), c.y() + 5));
		p.drawPoint(QPointF(c.x(), c.y() - 5));
		break;
	}
}

void ProcNotify(void *, calldata_t *cd)
{
	const char *kind = calldata_string(cd, "kind");
	const char *title = calldata_string(cd, "title");
	const char *text = calldata_string(cd, "text");
	const char *key = calldata_string(cd, "key");
	if (SpectraOverlay *overlay = instance.data()) {
		overlay->Notify(SpectraOverlay::KindFromName(QString::fromUtf8(kind ? kind : "")),
				QString::fromUtf8(title ? title : ""), QString::fromUtf8(text ? text : ""),
				QString::fromUtf8(key ? key : ""));
	}
}

} // namespace

SpectraOverlay::SpectraOverlay(QWidget *parent)
	: QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
				  Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus |
				  Qt::NoDropShadowWindowHint)
{
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setFocusPolicy(Qt::NoFocus);
	setWindowTitle(QStringLiteral("Spectra Notifications"));
	resize(kCardWidth + 2 * kMargin, kMaxToasts * (kCardHeight + kGap) + 2 * kMargin);

	clock.start();
	frame.setInterval(16);
	connect(&frame, &QTimer::timeout, this, &SpectraOverlay::Tick);

	static bool registered = false;
	if (!registered) {
		proc_handler_add(obs_get_proc_handler(),
				 "void spectra_notify(in string kind, in string title, in string text, in string key)",
				 ProcNotify, nullptr);
		registered = true;
	}
	instance = this;

	LoadSettings();
}

SpectraOverlay::~SpectraOverlay()
{
	if (instance == this) {
		instance = nullptr;
	}
}

SpectraOverlay::Kind SpectraOverlay::KindFromName(const QString &name)
{
	static const std::pair<const char *, Kind> names[] = {
		{"loop", Kind::Loop},       {"loop_stop", Kind::LoopStop},
		{"clip", Kind::Clip},       {"screenshot", Kind::Screenshot},
		{"obscura", Kind::Obscura}, {"upload", Kind::Upload},
		{"warning", Kind::Warning}, {"error", Kind::Error},
	};
	for (const auto &[n, kind] : names) {
		if (name.compare(QLatin1String(n), Qt::CaseInsensitive) == 0) {
			return kind;
		}
	}
	return Kind::Info;
}

const QList<SpectraOverlay::Category> &SpectraOverlay::Categories()
{
	static const QList<Category> categories = {
		{"ShowLoop", "Spectra.Overlay.Show.Loop"},
		{"ShowClips", "Spectra.Overlay.Show.Clips"},
		{"ShowScreenshots", "Spectra.Overlay.Show.Screenshots"},
		{"ShowObscura", "Spectra.Overlay.Show.Obscura"},
		{"ShowUploads", "Spectra.Overlay.Show.Uploads"},
		{"ShowCapture", "Spectra.Overlay.Show.Capture"},
		{"ShowErrors", "Spectra.Overlay.Show.Errors"},
	};
	return categories;
}

const char *SpectraOverlay::CategoryKey(Kind kind)
{
	switch (kind) {
	case Kind::Loop:
	case Kind::LoopStop:
		return "ShowLoop";
	case Kind::Clip:
		return "ShowClips";
	case Kind::Screenshot:
		return "ShowScreenshots";
	case Kind::Obscura:
		return "ShowObscura";
	case Kind::Upload:
		return "ShowUploads";
	case Kind::Info:
	case Kind::Warning:
		return "ShowCapture";
	case Kind::Error:
		return "ShowErrors";
	}
	return "ShowCapture";
}

void SpectraOverlay::LoadSettings()
{
	OBSBasic *main = OBSBasic::Get();
	config_t *config = main ? main->Config() : nullptr;
	if (!config) {
		return;
	}
	hiddenCategories.clear();
	for (const Category &category : Categories()) {
		if (!config_get_bool(config, OVERLAY_SECTION, category.configKey)) {
			hiddenCategories << QString::fromLatin1(category.configKey);
		}
	}
	SetStyle(config_get_bool(config, OVERLAY_SECTION, "Enabled"),
		 (Corner)config_get_int(config, OVERLAY_SECTION, "Corner"),
		 (int)config_get_int(config, OVERLAY_SECTION, "DurationSec"));
}

void SpectraOverlay::SetStyle(bool enabled_, Corner corner_, int durationSec)
{
	enabled = enabled_;
	corner = (Corner)std::clamp((int)corner_, 0, (int)Corner::TopCenter);
	durationMs = std::clamp(durationSec, 1, 30) * 1000;
	if (!enabled) {
		toasts.clear();
	}
}

void SpectraOverlay::Notify(Kind kind, const QString &title, const QString &text, const QString &key)
{
	QMetaObject::invokeMethod(
		this, [this, kind, title, text, key]() { Show(kind, title, text, key); }, Qt::QueuedConnection);
}

void SpectraOverlay::Show(Kind kind, const QString &title, const QString &text, const QString &key)
{
	if (!enabled || hiddenCategories.contains(QLatin1String(CategoryKey(kind)))) {
		return;
	}
	const qint64 now = clock.elapsed();

	Toast toast{kind, title, text, key, now, now + durationMs};
	bool replaced = false;
	if (!key.isEmpty()) {
		for (Toast &t : toasts) {
			if (t.key == key && t.leaving < 0) {
				/* keep its slot and entrance, refresh the rest */
				toast.born = t.born;
				toast.y = t.y;
				t = toast;
				replaced = true;
				break;
			}
		}
	}
	if (!replaced) {
		toasts.prepend(toast);
		int shown = 0;
		for (Toast &t : toasts) {
			if (t.leaving < 0 && ++shown > kMaxToasts) {
				t.leaving = now;
			}
		}
	}

	Relayout();
	if (!isVisible()) {
		show();
		ApplyNativeStyle();
	}
#ifdef _WIN32
	/* games come to the front on focus; stay above them */
	SetWindowPos((HWND)winId(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
	raise();
	if (!frame.isActive()) {
		frame.start();
	}
}

void SpectraOverlay::Tick()
{
	const qint64 now = clock.elapsed();
	for (Toast &t : toasts) {
		if (t.leaving < 0 && now >= t.deadline) {
			t.leaving = now;
		}
	}
	toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
				    [now](const Toast &t) { return t.leaving >= 0 && now - t.leaving > kLeaveMs; }),
		     toasts.end());

	if (toasts.isEmpty()) {
		frame.stop();
		hide();
		/* drop the native window too: no idle topmost window is left around */
		destroy();
		return;
	}

	/* slide the remaining toasts into their slots */
	const bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
	int slot = 0;
	for (Toast &t : toasts) {
		float target = (float)(slot * (kCardHeight + kGap));
		if (bottom) {
			target = (float)(height() - 2 * kMargin - kCardHeight) - target;
		}
		t.y = t.y < 0.0f ? target : t.y + (target - t.y) * 0.22f;
		if (t.leaving < 0) {
			slot++;
		}
	}
	update();
}

void SpectraOverlay::Relayout()
{
	anchor = GameArea();
	const QSize size = this->size();
	QPoint pos;
	switch (corner) {
	case Corner::TopRight:
		pos = QPoint(anchor.right() + 1 - size.width(), anchor.top());
		break;
	case Corner::TopLeft:
		pos = anchor.topLeft();
		break;
	case Corner::BottomRight:
		pos = QPoint(anchor.right() + 1 - size.width(), anchor.bottom() + 1 - size.height());
		break;
	case Corner::BottomLeft:
		pos = QPoint(anchor.left(), anchor.bottom() + 1 - size.height());
		break;
	case Corner::TopCenter:
		pos = QPoint(anchor.center().x() - size.width() / 2, anchor.top());
		break;
	}
	if (pos != this->pos()) {
		move(pos);
	}
}

QRect SpectraOverlay::GameArea() const
{
	QScreen *fallbackScreen = QGuiApplication::primaryScreen();
	if (OBSBasic *main = OBSBasic::Get()) {
		if (QScreen *s = main->screen()) {
			fallbackScreen = s;
		}
	}
	const QRect fallback = fallbackScreen ? fallbackScreen->geometry() : QRect(0, 0, 1920, 1080);

#ifdef _WIN32
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (!fg || (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId())) {
		return fallback;
	}

	HMONITOR monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = {sizeof(mi)};
	if (!GetMonitorInfo(monitor, &mi)) {
		return fallback;
	}
	RECT r = mi.rcMonitor;
	RECT win;
	if (GetWindowRect(fg, &win)) {
		RECT clipped;
		/* a windowed game: sit inside its window when it has room */
		if (IntersectRect(&clipped, &win, &mi.rcMonitor) && clipped.right - clipped.left >= 640 &&
		    clipped.bottom - clipped.top >= 400) {
			r = clipped;
		}
	}

	/* native pixels to Qt's logical coordinates (screen origins are native) */
	const QPoint monitorOrigin(mi.rcMonitor.left, mi.rcMonitor.top);
	for (QScreen *s : QGuiApplication::screens()) {
		if (s->geometry().topLeft() != monitorOrigin) {
			continue;
		}
		const qreal dpr = s->devicePixelRatio();
		const QPoint o = s->geometry().topLeft();
		return QRect(o.x() + qRound((r.left - o.x()) / dpr), o.y() + qRound((r.top - o.y()) / dpr),
			     qRound((r.right - r.left) / dpr), qRound((r.bottom - r.top) / dpr));
	}
	if (QScreen *s = QGuiApplication::screenAt(monitorOrigin)) {
		return s->geometry();
	}
#endif
	return fallback;
}

void SpectraOverlay::ApplyNativeStyle()
{
#ifdef _WIN32
	HWND hwnd = (HWND)winId();
	LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
	SetWindowLongPtr(hwnd, GWL_EXSTYLE,
			 ex | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
#endif
}

void SpectraOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const qint64 now = clock.elapsed();
	const bool left = corner == Corner::TopLeft || corner == Corner::BottomLeft;
	const bool center = corner == Corner::TopCenter;

	QFont titleFont = font();
	titleFont.setPointSizeF(11.0);
	titleFont.setWeight(QFont::DemiBold);
	QFont textFont = font();
	textFont.setPointSizeF(9.5);
	QFont brandFont = font();
	brandFont.setPointSizeF(6.5);
	brandFont.setWeight(QFont::Bold);
	brandFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);

	for (auto it = toasts.crbegin(); it != toasts.crend(); ++it) {
		const Toast &t = *it;
		const double in = EaseOut((double)(now - t.born) / kEnterMs);
		const double out = t.leaving < 0 ? 0.0 : EaseOut((double)(now - t.leaving) / kLeaveMs);
		const double opacity = in * (1.0 - out);
		if (opacity <= 0.0) {
			continue;
		}
		const double slide = (1.0 - in) * 36.0 + out * 36.0;
		QPointF offset = center ? QPointF(0, -slide) : QPointF(left ? -slide : slide, 0);

		p.save();
		p.setOpacity(opacity);
		p.translate(QPointF(kMargin, kMargin + std::max(0.0f, t.y)) + offset);
		const QRectF card(0, 0, kCardWidth, kCardHeight);
		const QColor accent = Accent(t.kind);

		/* soft shadow */
		for (int i = 3; i >= 1; i--) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 22));
			p.drawRoundedRect(card.adjusted(-i * 2, -i * 2 + 2, i * 2, i * 2 + 2), 12 + i * 2, 12 + i * 2);
		}

		QPainterPath shape;
		shape.addRoundedRect(card, 12, 12);
		QLinearGradient bg(card.topLeft(), card.bottomLeft());
		bg.setColorAt(0.0, QColor(0x1a, 0x1c, 0x22, 240));
		bg.setColorAt(1.0, QColor(0x10, 0x11, 0x15, 240));
		p.fillPath(shape, bg);

		/* accent wash behind the icon */
		QRadialGradient wash(QPointF(30, kCardHeight / 2.0), 90);
		QColor washColor = accent;
		washColor.setAlpha(46);
		wash.setColorAt(0.0, washColor);
		wash.setColorAt(1.0, QColor(0, 0, 0, 0));
		p.fillPath(shape, wash);

		p.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawPath(shape);

		/* the spectrum edge */
		p.save();
		p.setClipPath(shape);
		QLinearGradient edge(0, 0, 0, kCardHeight);
		SpectrumStops(edge);
		p.fillRect(QRectF(0, 0, 4, kCardHeight), edge);

		/* time left, as a thin spectrum line along the bottom */
		if (t.leaving < 0 && t.deadline > t.born) {
			const double remaining =
				std::clamp((double)(t.deadline - now) / (t.deadline - t.born), 0.0, 1.0);
			QLinearGradient bar(0, 0, kCardWidth, 0);
			SpectrumStops(bar, 200);
			p.fillRect(QRectF(0, kCardHeight - 2, kCardWidth * remaining, 2), bar);
		}
		p.restore();

		/* icon */
		const QPointF iconCenter(34, kCardHeight / 2.0);
		QColor ring = accent;
		ring.setAlpha(40);
		p.setPen(Qt::NoPen);
		p.setBrush(ring);
		p.drawEllipse(iconCenter, 19.0, 19.0);
		DrawGlyph(p, t.kind, iconCenter, accent, now);

		/* text */
		const qreal textLeft = 64;
		const qreal textWidth = kCardWidth - textLeft - 14;
		p.setFont(brandFont);
		p.setPen(QColor(0x8a, 0x8f, 0x99, 170));
		p.drawText(QRectF(textLeft, 0, textWidth, 20), Qt::AlignRight | Qt::AlignVCenter,
			   QStringLiteral("SPECTRA"));

		const bool hasText = !t.text.isEmpty();
		p.setFont(titleFont);
		p.setPen(QColor(0xf2, 0xf2, 0xf2));
		QFontMetrics tm(titleFont);
		const qreal titleTop = hasText ? 12 : (kCardHeight - tm.height()) / 2.0;
		p.drawText(QRectF(textLeft, titleTop, textWidth - 52, tm.height()), Qt::AlignLeft | Qt::AlignVCenter,
			   tm.elidedText(t.title, Qt::ElideRight, (int)textWidth - 52));

		if (hasText) {
			p.setFont(textFont);
			p.setPen(QColor(0xb0, 0xb4, 0xbc));
			QFontMetrics fm(textFont);
			p.drawText(QRectF(textLeft, 34, textWidth, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
				   fm.elidedText(t.text, Qt::ElideMiddle, (int)textWidth));
		}
		p.restore();
	}
}
