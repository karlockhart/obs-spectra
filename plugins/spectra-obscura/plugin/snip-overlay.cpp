#include "snip-overlay.hpp"

#include <obs-module.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace obscura {

namespace {
constexpr int kMinSide = 4; /* anything smaller is a stray click */
const QColor kDim(0, 0, 0, 130);
const QColor kEdge(255, 210, 60);
const QColor kGuide(255, 210, 60, 70);
const QColor kLabelBg(20, 20, 24, 220);
} // namespace

SnipOverlay::SnipOverlay(const QImage &desktop)
	: QDialog(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
	  pixmap(QPixmap::fromImage(desktop))
{
	setCursor(Qt::CrossCursor);
	setMouseTracking(true);
	setWindowTitle(obs_module_text("Obscura.Snip.Title"));
	QRect bounds;
	for (QScreen *screen : QGuiApplication::screens()) {
		bounds = bounds.united(screen->geometry());
	}
	setGeometry(bounds);
}

std::optional<spectra::Rect> SnipOverlay::SelectRegion(const QImage &desktop)
{
	SnipOverlay overlay(desktop);
	overlay.exec();
	return overlay.result;
}

QPointF SnipOverlay::Scale() const
{
	/* widget (logical) pixels -> captured (physical) pixels */
	return {pixmap.width() / (double)std::max(1, width()), pixmap.height() / (double)std::max(1, height())};
}

std::optional<QRectF> SnipOverlay::Selection() const
{
	if (!origin) {
		return std::nullopt;
	}
	return QRectF(*origin, cursor).normalized();
}

spectra::Rect SnipOverlay::ToImage(const QRectF &sel) const
{
	const QPointF s = Scale();
	auto clamp = [](double v, int hi) {
		return std::clamp((int)std::lround(v), 0, hi);
	};
	return {clamp(sel.left() * s.x(), pixmap.width()), clamp(sel.top() * s.y(), pixmap.height()),
		clamp(sel.right() * s.x(), pixmap.width()), clamp(sel.bottom() * s.y(), pixmap.height())};
}

void SnipOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.drawPixmap(QRectF(rect()), pixmap, QRectF(pixmap.rect()));
	p.fillRect(rect(), kDim);
	std::optional<QRectF> sel = Selection();
	if (!sel) {
		QScreen *screen = QGuiApplication::screenAt(QPoint((int)cursor.x() + x(), (int)cursor.y() + y()));
		QRect area = screen ? screen->geometry().translated(-x(), -y()) : rect();
		QRectF box(area.center().x() - 220, area.center().y() - 22, 440, 44);
		p.setPen(Qt::NoPen);
		p.setBrush(kLabelBg);
		p.drawRoundedRect(box, 8, 8);
		p.setPen(QColor(235, 235, 235));
		p.drawText(box, Qt::AlignCenter, obs_module_text("Obscura.Snip.Hint"));
		return;
	}
	const QPointF s = Scale();
	p.drawPixmap(*sel, pixmap,
		     QRectF(sel->left() * s.x(), sel->top() * s.y(), sel->width() * s.x(), sel->height() * s.y()));
	QPen pen(kEdge, 1);
	pen.setCosmetic(true);
	p.setPen(pen);
	p.drawRect(*sel);
	p.setPen(QPen(kGuide, 1));
	p.drawLine(QPointF(0, sel->top()), QPointF(width(), sel->top()));
	p.drawLine(QPointF(0, sel->bottom()), QPointF(width(), sel->bottom()));
	p.drawLine(QPointF(sel->left(), 0), QPointF(sel->left(), height()));
	p.drawLine(QPointF(sel->right(), 0), QPointF(sel->right(), height()));

	const spectra::Rect r = ToImage(*sel);
	const QString text = QStringLiteral("%1 × %2").arg(r.x1 - r.x0).arg(r.y1 - r.y0);
	QRectF box(sel->left(), sel->top() - 24, p.fontMetrics().horizontalAdvance(text) + 16, 20);
	if (box.top() < 0) {
		box.moveTop(sel->bottom() + 24 <= height() ? sel->bottom() + 4 : sel->top() + 4);
	}
	p.setPen(Qt::NoPen);
	p.setBrush(kLabelBg);
	p.drawRoundedRect(box, 4, 4);
	p.setPen(kEdge);
	p.drawText(box, Qt::AlignCenter, text);
}

void SnipOverlay::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
	raise();
	activateWindow();
#ifdef _WIN32
	/* the game may be fullscreen and in front; ask Windows to bring us forward */
	SetForegroundWindow((HWND)winId());
#endif
	grabKeyboard();
}

void SnipOverlay::hideEvent(QHideEvent *event)
{
	releaseKeyboard();
	QDialog::hideEvent(event);
}

void SnipOverlay::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		origin = cursor = event->position();
		update();
	} else {
		reject();
	}
}

void SnipOverlay::mouseMoveEvent(QMouseEvent *event)
{
	cursor = event->position();
	update();
}

void SnipOverlay::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || !origin) {
		return;
	}
	cursor = event->position();
	const spectra::Rect r = ToImage(*Selection());
	if (r.x1 - r.x0 < kMinSide || r.y1 - r.y0 < kMinSide) {
		origin.reset(); /* a click, not a drag: let them try again */
		update();
		return;
	}
	result = r;
	accept();
}

void SnipOverlay::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape) {
		reject();
	}
}

} // namespace obscura
