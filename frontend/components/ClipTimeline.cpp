#include "ClipTimeline.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

static constexpr int HEADER_W = 110;
static constexpr int RULER_H = 28;
static constexpr int VIDEO_H = 50;
static constexpr int BAR_H = 28;
static constexpr int TRACK_GAP = 2;
static constexpr int EDGE_GRAB = 6;
static constexpr int SNAP_PX = 8;
static constexpr double MAX_PX_PER_SEC = 400.0;

static const QColor playheadColor(230, 60, 60);
static const QColor inOutColor(255, 190, 40);
static const QColor segmentColor(38, 110, 140);
static const QColor sessionColor(255, 140, 0);

ClipTimeline::ClipTimeline(QWidget *parent) : QWidget(parent), scroll(new QScrollBar(Qt::Horizontal, this))
{
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);
	connect(scroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
}

QString ClipTimeline::FormatTime(double seconds, bool fractions)
{
	seconds = std::max(seconds, 0.0);
	qint64 cs = (qint64)std::llround(seconds * 100.0);
	qint64 h = cs / 360000, m = (cs / 6000) % 60, s = (cs / 100) % 60;
	QString text =
		QStringLiteral("%1:%2:%3").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
	if (fractions) {
		text += QStringLiteral(".%1").arg(cs % 100, 2, 10, QChar('0'));
	}
	return text;
}

void ClipTimeline::SetLabels(const QString &videoTrack, const QString &empty)
{
	videoLabel = videoTrack;
	emptyLabel = empty;
	update();
}

/* ------------------------------------------------------------------------- */
/* Data                                                                      */

void ClipTimeline::SetSegments(const QVector<Segment> &segments_)
{
	bool wasEmpty = segments.isEmpty();
	segments = segments_;
	duration = segments.isEmpty() ? 0.0 : segments.last().start + segments.last().duration;
	if (wasEmpty) {
		ZoomToFit();
	} else {
		SetZoom(pxPerSec);
	}
	update();
}

void ClipTimeline::SetBars(const QVector<Bar> &bars_)
{
	bool heightChanged = bars.size() != bars_.size();
	bars = bars_;
	if (heightChanged) {
		updateGeometry();
		UpdateScrollRange();
	}
	update();
}

void ClipTimeline::SetSelectedBar(int id)
{
	selectedBar = id;
	update();
}

void ClipTimeline::SetPlayhead(double t)
{
	playhead = std::clamp(t, 0.0, std::max(duration, 0.0));
	update();
}

void ClipTimeline::SetInOut(double in, double out)
{
	inPoint = in;
	outPoint = out;
	update();
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */

QSize ClipTimeline::sizeHint() const
{
	return QSize(800, minimumSizeHint().height());
}

QSize ClipTimeline::minimumSizeHint() const
{
	int h = RULER_H + VIDEO_H + TRACK_GAP + (int)std::max<qsizetype>(bars.size(), 2) * (BAR_H + TRACK_GAP) +
		scroll->sizeHint().height() + 4;
	return QSize(HEADER_W + 200, h);
}

int ClipTimeline::ContentLeft() const
{
	return HEADER_W;
}

int ClipTimeline::ContentWidth() const
{
	return std::max(width() - HEADER_W, 1);
}

double ClipTimeline::ScrollTime() const
{
	return scroll->value() / pxPerSec;
}

double ClipTimeline::TimeAt(int x) const
{
	return std::clamp(ScrollTime() + (x - ContentLeft()) / pxPerSec, 0.0, std::max(duration, 0.0));
}

int ClipTimeline::XAt(double t) const
{
	return ContentLeft() + (int)std::lround(t * pxPerSec) - scroll->value();
}

int ClipTimeline::BarRowTop(int index) const
{
	return RULER_H + VIDEO_H + TRACK_GAP + index * (BAR_H + TRACK_GAP);
}

int ClipTimeline::BarIndexAt(int y) const
{
	int top = BarRowTop(0);
	if (y < top) {
		return -1;
	}
	int index = (y - top) / (BAR_H + TRACK_GAP);
	return index < bars.size() ? index : -1;
}

void ClipTimeline::UpdateScrollRange()
{
	int total = (int)std::min(std::ceil(duration * pxPerSec), 2.0e9);
	scroll->setRange(0, std::max(total - ContentWidth(), 0));
	scroll->setPageStep(ContentWidth());
	scroll->setSingleStep(std::max(ContentWidth() / 20, 1));
	int sh = scroll->sizeHint().height();
	scroll->setGeometry(HEADER_W, height() - sh, ContentWidth(), sh);
}

void ClipTimeline::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	UpdateScrollRange();
}

void ClipTimeline::SetZoom(double pixelsPerSecond, double anchorTime)
{
	double minZoom = duration > 0.0 ? ContentWidth() / duration : 1.0;
	double maxZoom = duration > 0.0 ? std::min(MAX_PX_PER_SEC, 1.9e9 / duration) : MAX_PX_PER_SEC;
	minZoom = std::min(minZoom, maxZoom);
	pixelsPerSecond = std::clamp(pixelsPerSecond, minZoom, maxZoom);

	if (anchorTime < 0.0) {
		anchorTime = playhead;
	}
	/* Keep the anchor at the same place on screen */
	int anchorX = std::clamp(XAt(anchorTime), ContentLeft(), ContentLeft() + ContentWidth());
	bool changed = pixelsPerSecond != pxPerSec;
	pxPerSec = pixelsPerSecond;
	UpdateScrollRange();
	scroll->setValue((int)std::lround(anchorTime * pxPerSec) - (anchorX - ContentLeft()));
	update();
	if (changed) {
		emit zoomChanged(pxPerSec);
	}
}

void ClipTimeline::ZoomToFit()
{
	SetZoom(0.0);
	scroll->setValue(0);
}

void ClipTimeline::EnsureVisible(double t)
{
	int x = XAt(t);
	int margin = ContentWidth() / 10;
	if (x < ContentLeft() || x > ContentLeft() + ContentWidth()) {
		scroll->setValue((int)std::lround(t * pxPerSec) - margin);
	}
}

/* ------------------------------------------------------------------------- */
/* Painting                                                                  */

static double RulerStep(double pxPerSec)
{
	static const double steps[] = {0.1, 0.5, 1,   2,    5,    10,   15,    30,    60,    120,
				       300, 600, 900, 1800, 3600, 7200, 14400, 21600, 43200, 86400};
	for (double step : steps) {
		if (step * pxPerSec >= 90.0) {
			return step;
		}
	}
	return 172800.0;
}

void ClipTimeline::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const QPalette &pal = palette();
	const QColor bg = pal.color(QPalette::Window).darker(115);
	const QColor trackBg = pal.color(QPalette::Base);
	const QColor text = pal.color(QPalette::WindowText);
	const QColor dim = pal.color(QPalette::PlaceholderText);
	const QColor accent = pal.color(QPalette::Highlight);
	const int contentBottom = height() - scroll->height();

	p.fillRect(rect(), bg);

	/* Track headers */
	p.setPen(text);
	QRect videoHeader(0, RULER_H, HEADER_W, VIDEO_H);
	p.fillRect(videoHeader.adjusted(2, 0, -2, 0), trackBg);
	p.drawText(videoHeader.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, videoLabel);
	for (int i = 0; i < bars.size(); i++) {
		QRect header(0, BarRowTop(i), HEADER_W, BAR_H);
		p.fillRect(header.adjusted(2, 0, -2, 0), bars[i].id == selectedBar ? accent.darker(160) : trackBg);
		p.fillRect(QRect(header.left() + 2, header.top(), 4, header.height()), bars[i].color);
		p.setPen(bars[i].visible ? text : dim);
		p.drawText(header.adjusted(12, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
			   p.fontMetrics().elidedText(bars[i].name, Qt::ElideRight, HEADER_W - 16));
	}

	p.setClipRect(QRect(ContentLeft(), 0, ContentWidth(), contentBottom));

	const double visibleStart = ScrollTime();
	const double visibleEnd = visibleStart + ContentWidth() / pxPerSec;

	/* Track backgrounds */
	p.fillRect(QRect(ContentLeft(), RULER_H, ContentWidth(), VIDEO_H), trackBg);
	for (int i = 0; i < bars.size(); i++) {
		p.fillRect(QRect(ContentLeft(), BarRowTop(i), ContentWidth(), BAR_H), trackBg);
	}

	/* The loop plays as one continuous video; segment files are an
	 * implementation detail and aren't shown. Only gaps in recording
	 * (a new session) are marked, with the time that session began. */
	QFont small = font();
	small.setPointSizeF(std::max(small.pointSizeF() * 0.85, 6.0));
	p.setFont(small);
	if (duration > 0.0) {
		int x0 = XAt(0.0), x1 = XAt(duration);
		p.fillRect(QRect(x0, RULER_H + 3, std::max(x1 - x0, 1), VIDEO_H - 6), segmentColor);
	}
	for (int i = 0; i < segments.size(); i++) {
		const Segment &s = segments[i];
		if (!s.sessionStart || s.start > visibleEnd) {
			continue;
		}
		/* A session's label stays readable until the next session */
		double next = duration;
		for (int j = i + 1; j < segments.size(); j++) {
			if (segments[j].sessionStart) {
				next = segments[j].start;
				break;
			}
		}
		if (next < visibleStart) {
			continue;
		}
		int x0 = XAt(s.start), x1 = XAt(next);
		if (i > 0) {
			p.setPen(QPen(sessionColor, 2, Qt::DashLine));
			p.drawLine(x0, RULER_H, x0, contentBottom);
		}
		int labelX = std::max(x0, ContentLeft());
		if (x1 - labelX > 40) {
			p.setPen(Qt::white);
			p.drawText(QRect(labelX + 5, RULER_H + 6, x1 - labelX - 8, VIDEO_H - 12),
				   Qt::AlignTop | Qt::AlignLeft,
				   p.fontMetrics().elidedText(s.label, Qt::ElideRight, x1 - labelX - 8));
		}
	}

	/* Layer bars */
	for (int i = 0; i < bars.size(); i++) {
		const Bar &b = bars[i];
		int x0 = XAt(b.start), x1 = XAt(b.end);
		QRect r(x0, BarRowTop(i) + 3, std::max(x1 - x0, 2), BAR_H - 6);
		QColor c = b.color;
		if (!b.visible) {
			c.setAlpha(80);
		}
		QPainterPath path;
		path.addRoundedRect(r, 4, 4);
		p.fillPath(path, c);
		if (b.id == selectedBar) {
			p.setPen(QPen(Qt::white, 2));
			p.drawPath(path);
		}
		if (r.width() > 30) {
			p.setPen(c.lightness() > 150 ? Qt::black : Qt::white);
			p.drawText(r.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(b.name, Qt::ElideRight, r.width() - 10));
		}
	}

	/* Ruler */
	p.fillRect(QRect(ContentLeft(), 0, ContentWidth(), RULER_H - 1), bg.lighter(110));
	const double step = RulerStep(pxPerSec);
	const double minor = step / 5.0;
	p.setPen(dim);
	for (double t = std::floor(visibleStart / minor) * minor; t <= visibleEnd; t += minor) {
		int x = XAt(t);
		p.drawLine(x, RULER_H - 6, x, RULER_H - 1);
	}
	p.setPen(text);
	for (double t = std::floor(visibleStart / step) * step; t <= visibleEnd; t += step) {
		int x = XAt(t);
		p.drawLine(x, RULER_H - 14, x, RULER_H - 1);
		p.drawText(QRect(x + 3, 0, 120, RULER_H - 8), Qt::AlignVCenter | Qt::AlignLeft,
			   FormatTime(t, step < 1.0));
	}

	/* In/Out range */
	if (inPoint >= 0.0 || outPoint >= 0.0) {
		double a = inPoint >= 0.0 ? inPoint : 0.0;
		double b = outPoint >= 0.0 ? outPoint : duration;
		int x0 = XAt(a), x1 = XAt(b);
		QColor shade = inOutColor;
		shade.setAlpha(40);
		p.fillRect(QRect(x0, 0, x1 - x0, contentBottom), shade);
		p.setPen(QPen(inOutColor, 2));
		if (inPoint >= 0.0) {
			p.drawLine(x0, 0, x0, contentBottom);
			QPolygon flag({QPoint(x0, 0), QPoint(x0 + 9, 0), QPoint(x0, 12)});
			p.setBrush(inOutColor);
			p.drawPolygon(flag);
		}
		if (outPoint >= 0.0) {
			p.drawLine(x1, 0, x1, contentBottom);
			QPolygon flag({QPoint(x1, 0), QPoint(x1 - 9, 0), QPoint(x1, 12)});
			p.setBrush(inOutColor);
			p.drawPolygon(flag);
		}
		p.setBrush(Qt::NoBrush);
	}

	/* Playhead */
	int px = XAt(playhead);
	p.setPen(QPen(playheadColor, 2));
	p.drawLine(px, 0, px, contentBottom);
	p.setBrush(playheadColor);
	p.drawPolygon(
		QPolygon({QPoint(px - 6, 0), QPoint(px + 6, 0), QPoint(px + 6, 8), QPoint(px, 14), QPoint(px - 6, 8)}));

	if (segments.isEmpty()) {
		p.setPen(dim);
		p.drawText(QRect(ContentLeft(), RULER_H, ContentWidth(), VIDEO_H), Qt::AlignCenter, emptyLabel);
	}
}

/* ------------------------------------------------------------------------- */
/* Interaction                                                               */

double ClipTimeline::Snap(double t, double exclude) const
{
	double best = t;
	double bestDist = SNAP_PX / pxPerSec;
	auto consider = [&](double candidate) {
		if (candidate < 0.0 || candidate == exclude) {
			return;
		}
		double d = std::abs(candidate - t);
		if (d < bestDist) {
			bestDist = d;
			best = candidate;
		}
	};
	consider(playhead);
	consider(inPoint);
	consider(outPoint);
	consider(0.0);
	consider(duration);
	for (const Bar &b : bars) {
		if (b.id != dragBar) {
			consider(b.start);
			consider(b.end);
		}
	}
	return best;
}

void ClipTimeline::Scrub(int x)
{
	double t = TimeAt(x);
	playhead = t;
	update();
	emit seekRequested(t);
}

void ClipTimeline::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || duration <= 0.0) {
		return;
	}
	const QPoint pos = event->position().toPoint();

	if (pos.x() < ContentLeft()) {
		int index = BarIndexAt(pos.y());
		if (index >= 0) {
			selectedBar = bars[index].id;
			emit barSelected(selectedBar);
			update();
		}
		return;
	}

	if (pos.y() < RULER_H) {
		if (inPoint >= 0.0 && std::abs(pos.x() - XAt(inPoint)) <= EDGE_GRAB) {
			drag = Drag::In;
			return;
		}
		if (outPoint >= 0.0 && std::abs(pos.x() - XAt(outPoint)) <= EDGE_GRAB) {
			drag = Drag::Out;
			return;
		}
	}

	int index = BarIndexAt(pos.y());
	if (index >= 0) {
		const Bar &b = bars[index];
		int x0 = XAt(b.start), x1 = XAt(b.end);
		if (pos.x() >= x0 - EDGE_GRAB && pos.x() <= x1 + EDGE_GRAB) {
			dragBar = b.id;
			dragStart = b.start;
			dragEnd = b.end;
			if (std::abs(pos.x() - x0) <= EDGE_GRAB) {
				drag = Drag::BarStart;
			} else if (std::abs(pos.x() - x1) <= EDGE_GRAB) {
				drag = Drag::BarEnd;
			} else {
				drag = Drag::BarMove;
				dragOffset = TimeAt(pos.x()) - b.start;
			}
		}
		selectedBar = b.id;
		emit barSelected(selectedBar);
		update();
		return;
	}

	drag = Drag::Scrub;
	Scrub(pos.x());
}

void ClipTimeline::mouseMoveEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();

	if (drag == Drag::None) {
		/* Cursor hints */
		Qt::CursorShape shape = Qt::ArrowCursor;
		int index = BarIndexAt(pos.y());
		if (pos.y() < RULER_H && ((inPoint >= 0.0 && std::abs(pos.x() - XAt(inPoint)) <= EDGE_GRAB) ||
					  (outPoint >= 0.0 && std::abs(pos.x() - XAt(outPoint)) <= EDGE_GRAB))) {
			shape = Qt::SizeHorCursor;
		} else if (index >= 0 && pos.x() >= ContentLeft()) {
			int x0 = XAt(bars[index].start), x1 = XAt(bars[index].end);
			if (std::abs(pos.x() - x0) <= EDGE_GRAB || std::abs(pos.x() - x1) <= EDGE_GRAB) {
				shape = Qt::SizeHorCursor;
			} else if (pos.x() > x0 && pos.x() < x1) {
				shape = Qt::OpenHandCursor;
			}
		}
		setCursor(shape);
		return;
	}

	/* Scroll while dragging past either edge */
	if (pos.x() < ContentLeft()) {
		scroll->setValue(scroll->value() - (ContentLeft() - pos.x()));
	} else if (pos.x() > ContentLeft() + ContentWidth()) {
		scroll->setValue(scroll->value() + (pos.x() - ContentLeft() - ContentWidth()));
	}

	double t = TimeAt(pos.x());
	switch (drag) {
	case Drag::Scrub:
		Scrub(pos.x());
		return;
	case Drag::In:
		inPoint = Snap(outPoint >= 0.0 ? std::min(t, outPoint) : t, inPoint);
		emit inOutChanged(inPoint, outPoint);
		break;
	case Drag::Out:
		outPoint = Snap(std::max(t, inPoint), outPoint);
		emit inOutChanged(inPoint, outPoint);
		break;
	case Drag::BarMove:
	case Drag::BarStart:
	case Drag::BarEnd: {
		double start = dragStart, end = dragEnd;
		const double minLength = 0.1;
		if (drag == Drag::BarMove) {
			double length = dragEnd - dragStart;
			start = std::clamp(t - dragOffset, 0.0, std::max(duration - length, 0.0));
			double snapped = Snap(start);
			if (snapped == start) {
				snapped = Snap(start + length) - length;
			}
			start = std::clamp(snapped, 0.0, std::max(duration - length, 0.0));
			end = start + length;
		} else if (drag == Drag::BarStart) {
			start = std::min(Snap(t), dragEnd - minLength);
		} else {
			end = std::max(Snap(t), dragStart + minLength);
		}
		for (Bar &b : bars) {
			if (b.id == dragBar) {
				b.start = start;
				b.end = end;
			}
		}
		emit barChanged(dragBar, start, end);
		break;
	}
	case Drag::None:
		break;
	}
	update();
}

void ClipTimeline::mouseReleaseEvent(QMouseEvent *)
{
	drag = Drag::None;
	dragBar = -1;
}

void ClipTimeline::wheelEvent(QWheelEvent *event)
{
	const QPoint delta = event->angleDelta();
	if (event->modifiers() & Qt::ControlModifier) {
		double factor = std::pow(1.0015, delta.y());
		SetZoom(pxPerSec * factor, TimeAt((int)event->position().x()));
	} else {
		int d = delta.x() != 0 ? delta.x() : delta.y();
		scroll->setValue(scroll->value() - d * ContentWidth() / 1200);
	}
	event->accept();
}
