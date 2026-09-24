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
static constexpr int STRIP_H = 24;
static constexpr int VIDEO_H = 50;
static constexpr int BAR_H = 28;
static constexpr int TRACK_GAP = 2;
/* How close to a line the pointer has to be to grab it */
static constexpr int EDGE_GRAB = 7;
/* Width of the In/Out handles on the clip strip */
static constexpr int HANDLE_W = 10;
static constexpr int SNAP_PX = 8;
/* Movement before a press counts as a drag rather than a click */
static constexpr int DRAG_THRESHOLD = 4;
static constexpr double MIN_RANGE_SEC = 0.05;
static constexpr double MAX_PX_PER_SEC = 400.0;

static const QColor playheadColor(230, 60, 60);
static const QColor inOutColor(255, 190, 40);
static const QColor inOutActiveColor(255, 225, 130);
static const QColor segmentColor(38, 110, 140);
static const QColor sessionColor(255, 140, 0);
static const QColor outsideColor(0, 0, 0, 110);
static const QColor badgeColor(24, 24, 24, 230);

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

void ClipTimeline::SetRangeLabels(const QString &header, const QString &hint)
{
	rangeLabel = header;
	rangeHint = hint;
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
		SetZoom(pxPerSec, ScrollTime());
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

bool ClipTimeline::RangeBounds(double &a, double &b) const
{
	if (inPoint < 0.0 && outPoint < 0.0) {
		return false;
	}
	a = inPoint >= 0.0 ? inPoint : 0.0;
	b = outPoint >= 0.0 ? outPoint : duration;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */

QSize ClipTimeline::sizeHint() const
{
	return QSize(800, minimumSizeHint().height());
}

QSize ClipTimeline::minimumSizeHint() const
{
	int h = RULER_H + STRIP_H + VIDEO_H + TRACK_GAP +
		(int)std::max<qsizetype>(bars.size(), 2) * (BAR_H + TRACK_GAP) + scroll->sizeHint().height() + 4;
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

int ClipTimeline::ContentBottom() const
{
	return height() - scroll->height();
}

int ClipTimeline::StripTop() const
{
	return RULER_H;
}

int ClipTimeline::VideoTop() const
{
	return RULER_H + STRIP_H;
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

bool ClipTimeline::IsVisible(double t) const
{
	int x = XAt(t);
	return x >= ContentLeft() && x <= ContentLeft() + ContentWidth();
}

int ClipTimeline::BarRowTop(int index) const
{
	return VideoTop() + VIDEO_H + TRACK_GAP + index * (BAR_H + TRACK_GAP);
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
	/* The fit zoom depends on the width; keep the left edge in place */
	SetZoom(pxPerSec, ScrollTime());
}

double ClipTimeline::MaxZoom() const
{
	return duration > 0.0 ? std::min(MAX_PX_PER_SEC, 1.9e9 / duration) : MAX_PX_PER_SEC;
}

double ClipTimeline::MinZoom() const
{
	double fit = duration > 0.0 ? ContentWidth() / duration : 1.0;
	return std::min(fit, MaxZoom());
}

void ClipTimeline::SetZoom(double pixelsPerSecond, double anchorTime)
{
	pixelsPerSecond = std::clamp(pixelsPerSecond, MinZoom(), MaxZoom());

	if (anchorTime < 0.0) {
		anchorTime = IsVisible(playhead) ? playhead : ScrollTime() + ContentWidth() / (2.0 * pxPerSec);
	}
	/* Keep the anchor at the same place on screen */
	int anchorX = std::clamp(XAt(anchorTime), ContentLeft(), ContentLeft() + ContentWidth());
	pxPerSec = pixelsPerSecond;
	UpdateScrollRange();
	scroll->setValue((int)std::lround(anchorTime * pxPerSec) - (anchorX - ContentLeft()));
	update();
	emit zoomChanged(pxPerSec);
}

void ClipTimeline::ZoomBy(double factor)
{
	SetZoom(pxPerSec * factor);
}

void ClipTimeline::ZoomToFit()
{
	SetZoom(0.0);
	scroll->setValue(0);
}

void ClipTimeline::ZoomToRange()
{
	double a, b;
	if (!RangeBounds(a, b) || b - a < MIN_RANGE_SEC) {
		ZoomToFit();
		return;
	}
	const double mid = (a + b) / 2.0;
	SetZoom(ContentWidth() / ((b - a) * 1.25), mid);
	CenterOn(mid);
}

void ClipTimeline::CenterOn(double t)
{
	scroll->setValue((int)std::lround(t * pxPerSec) - ContentWidth() / 2);
	update();
}

void ClipTimeline::EnsureVisible(double t)
{
	if (!IsVisible(t)) {
		scroll->setValue((int)std::lround(t * pxPerSec) - ContentWidth() / 10);
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
	const int contentBottom = ContentBottom();
	const int contentRight = ContentLeft() + ContentWidth();
	const int stripTop = StripTop();
	const int videoTop = VideoTop();

	QFont small = font();
	small.setPointSizeF(std::max(small.pointSizeF() * 0.85, 6.0));

	p.fillRect(rect(), bg);

	/* Track headers */
	p.setPen(dim);
	p.setFont(small);
	QRect stripHeader(0, stripTop, HEADER_W, STRIP_H);
	p.fillRect(stripHeader.adjusted(2, 0, -2, 0), trackBg.darker(120));
	p.drawText(stripHeader.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, rangeLabel);
	p.setFont(font());
	p.setPen(text);
	QRect videoHeader(0, videoTop, HEADER_W, VIDEO_H);
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
	p.fillRect(QRect(ContentLeft(), stripTop, ContentWidth(), STRIP_H), trackBg.darker(120));
	p.fillRect(QRect(ContentLeft(), videoTop, ContentWidth(), VIDEO_H), trackBg);
	for (int i = 0; i < bars.size(); i++) {
		p.fillRect(QRect(ContentLeft(), BarRowTop(i), ContentWidth(), BAR_H), trackBg);
	}

	/* The loop plays as one continuous video; segment files are an
	 * implementation detail and aren't shown. Only gaps in recording
	 * (a new session) are marked, with the time that session began. */
	p.setFont(small);
	if (duration > 0.0) {
		int x0 = XAt(0.0), x1 = XAt(duration);
		p.fillRect(QRect(x0, videoTop + 3, std::max(x1 - x0, 1), VIDEO_H - 6), segmentColor);
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
			p.drawLine(x0, videoTop, x0, contentBottom);
		}
		int labelX = std::max(x0, ContentLeft());
		if (x1 - labelX > 40) {
			p.setPen(Qt::white);
			p.drawText(QRect(labelX + 5, videoTop + 6, x1 - labelX - 8, VIDEO_H - 12),
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

	/* Everything outside the In/Out range is dimmed on the tracks, and
	 * the range's edges run down through them */
	double rangeA = 0.0, rangeB = 0.0;
	const bool hasRange = RangeBounds(rangeA, rangeB);
	if (hasRange) {
		int x0 = XAt(rangeA), x1 = XAt(rangeB);
		if (x0 > ContentLeft()) {
			p.fillRect(QRect(ContentLeft(), videoTop, x0 - ContentLeft(), contentBottom - videoTop),
				   outsideColor);
		}
		if (x1 < contentRight) {
			p.fillRect(QRect(x1, videoTop, contentRight - x1, contentBottom - videoTop), outsideColor);
		}
		QColor edge = inOutColor;
		edge.setAlpha(170);
		p.setPen(QPen(edge, 1));
		p.drawLine(x0, videoTop, x0, contentBottom);
		p.drawLine(x1, videoTop, x1, contentBottom);
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

	/* Clip strip: the range as a bar with a handle at each end */
	if (hasRange) {
		const int x0 = XAt(rangeA), x1 = XAt(rangeB);
		const bool rangeActive = hover == Hit::Range || drag == Drag::Range || drag == Drag::Select;
		QRect bar(x0, stripTop + 5, std::max(x1 - x0, 1), STRIP_H - 10);
		QColor fill = inOutColor;
		fill.setAlpha(rangeActive ? 235 : 175);
		QPainterPath barPath;
		barPath.addRoundedRect(bar, 3, 3);
		p.fillPath(barPath, fill);
		if (bar.width() > 90) {
			p.setPen(Qt::black);
			p.drawText(bar, Qt::AlignCenter, FormatTime(rangeB - rangeA));
		}

		auto handle = [&](int x, bool isIn, bool isSet, bool active) {
			QRect r = isIn ? QRect(x - HANDLE_W, stripTop + 2, HANDLE_W, STRIP_H - 4)
				       : QRect(x, stripTop + 2, HANDLE_W, STRIP_H - 4);
			QPainterPath path;
			path.addRoundedRect(r, 3, 3);
			const QColor c = active ? inOutActiveColor : inOutColor;
			if (isSet) {
				p.fillPath(path, c);
				/* Grip */
				p.setPen(QPen(QColor(0, 0, 0, 150), 1));
				const int cx = r.center().x();
				p.drawLine(cx - 1, r.top() + 6, cx - 1, r.bottom() - 5);
				p.drawLine(cx + 2, r.top() + 6, cx + 2, r.bottom() - 5);
			} else {
				/* An implied end (the loop's start or end) */
				p.setPen(QPen(c, 1, Qt::DashLine));
				p.setBrush(Qt::NoBrush);
				p.drawPath(path);
			}
		};
		handle(x0, true, inPoint >= 0.0, hover == Hit::In || drag == Drag::In);
		handle(x1, false, outPoint >= 0.0, hover == Hit::Out || drag == Drag::Out);

		/* The time of a handle being pointed at or dragged */
		auto badge = [&](int x, bool isIn, double t) {
			const QString label = FormatTime(t);
			const int w = p.fontMetrics().horizontalAdvance(label) + 12;
			QRect r(0, stripTop + 3, w, STRIP_H - 6);
			if (isIn) {
				r.moveRight(x - HANDLE_W - 4);
				if (r.left() < ContentLeft() + 2) {
					r.moveLeft(x + 4);
				}
			} else {
				r.moveLeft(x + HANDLE_W + 4);
				if (r.right() > contentRight - 2) {
					r.moveRight(x - 4);
				}
			}
			QPainterPath path;
			path.addRoundedRect(r, 3, 3);
			p.fillPath(path, badgeColor);
			p.setPen(Qt::white);
			p.drawText(r, Qt::AlignCenter, label);
		};
		const bool showBoth = drag == Drag::Range || drag == Drag::Select;
		if (showBoth || hover == Hit::In || drag == Drag::In) {
			badge(x0, true, rangeA);
		}
		if (showBoth || hover == Hit::Out || drag == Drag::Out) {
			badge(x1, false, rangeB);
		}
		p.setBrush(Qt::NoBrush);
	} else if (duration > 0.0 && !rangeHint.isEmpty()) {
		p.setPen(dim);
		p.drawText(QRect(ContentLeft() + 8, stripTop, ContentWidth() - 16, STRIP_H),
			   Qt::AlignVCenter | Qt::AlignLeft,
			   p.fontMetrics().elidedText(rangeHint, Qt::ElideRight, ContentWidth() - 16));
	}
	p.setFont(font());

	/* Playhead */
	int px = XAt(playhead);
	p.setPen(QPen(playheadColor, 2));
	p.drawLine(px, 0, px, contentBottom);
	p.setBrush(playheadColor);
	p.setPen(Qt::NoPen);
	p.drawPolygon(
		QPolygon({QPoint(px - 6, 0), QPoint(px + 6, 0), QPoint(px + 6, 8), QPoint(px, 14), QPoint(px - 6, 8)}));
	p.setBrush(Qt::NoBrush);

	if (segments.isEmpty()) {
		p.setPen(dim);
		p.drawText(QRect(ContentLeft(), videoTop, ContentWidth(), VIDEO_H), Qt::AlignCenter, emptyLabel);
	}
}

/* ------------------------------------------------------------------------- */
/* Interaction                                                               */

double ClipTimeline::Snap(double t, double exclude, bool ignoreRange) const
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
	if (!ignoreRange) {
		consider(inPoint);
		consider(outPoint);
	}
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

ClipTimeline::Hit ClipTimeline::HitAt(const QPoint &pos) const
{
	double a, b;
	if (duration <= 0.0 || pos.x() < ContentLeft() || !RangeBounds(a, b)) {
		return Hit::None;
	}
	/* The ruler belongs to the playhead alone */
	if (pos.y() < StripTop() || pos.y() >= ContentBottom()) {
		return Hit::None;
	}
	const bool strip = pos.y() < VideoTop();
	const int x0 = XAt(a), x1 = XAt(b);
	/* On the strip the handles sit outside the range; on the tracks its
	 * edge lines can be grabbed */
	const int inCenter = strip ? x0 - HANDLE_W / 2 : x0;
	const int outCenter = strip ? x1 + HANDLE_W / 2 : x1;
	const int reach = strip ? HANDLE_W / 2 + 3 : EDGE_GRAB;
	const int dIn = std::abs(pos.x() - inCenter), dOut = std::abs(pos.x() - outCenter);
	if (dIn <= reach || dOut <= reach) {
		if (dIn == dOut) {
			return pos.x() < (x0 + x1) / 2 ? Hit::In : Hit::Out;
		}
		return dIn < dOut ? Hit::In : Hit::Out;
	}
	if (strip && pos.x() > x0 && pos.x() < x1) {
		return Hit::Range;
	}
	return Hit::None;
}

void ClipTimeline::Scrub(int x)
{
	double t = TimeAt(x);
	playhead = t;
	update();
	emit seekRequested(t);
}

void ClipTimeline::UpdateHover(const QPoint &pos)
{
	Qt::CursorShape shape = Qt::ArrowCursor;
	Hit hit = Hit::None;
	int index = BarIndexAt(pos.y());
	bool overBar = false;
	if (index >= 0 && pos.x() >= ContentLeft()) {
		int x0 = XAt(bars[index].start), x1 = XAt(bars[index].end);
		if (std::abs(pos.x() - x0) <= EDGE_GRAB || std::abs(pos.x() - x1) <= EDGE_GRAB) {
			shape = Qt::SizeHorCursor;
			overBar = true;
		} else if (pos.x() > x0 && pos.x() < x1) {
			shape = Qt::OpenHandCursor;
			overBar = true;
		}
	}
	if (!overBar) {
		hit = HitAt(pos);
		if (hit == Hit::In || hit == Hit::Out) {
			shape = Qt::SizeHorCursor;
		} else if (hit == Hit::Range) {
			shape = Qt::OpenHandCursor;
		} else if (pos.x() >= ContentLeft() && pos.y() < ContentBottom() && duration > 0.0) {
			shape = pos.y() >= StripTop() && pos.y() < VideoTop() ? Qt::CrossCursor
									      : Qt::PointingHandCursor;
		}
	}
	setCursor(shape);
	if (hit != hover) {
		hover = hit;
		update();
	}
}

void ClipTimeline::mousePressEvent(QMouseEvent *event)
{
	if (duration <= 0.0) {
		return;
	}
	const QPoint pos = event->position().toPoint();
	pressPos = pos;
	dragMoved = false;

	if (event->button() == Qt::MiddleButton && pos.x() >= ContentLeft()) {
		drag = Drag::Pan;
		panScroll = scroll->value();
		setCursor(Qt::ClosedHandCursor);
		return;
	}
	if (event->button() != Qt::LeftButton) {
		return;
	}

	if (pos.x() < ContentLeft()) {
		int index = BarIndexAt(pos.y());
		if (index >= 0) {
			selectedBar = bars[index].id;
			emit barSelected(selectedBar);
			update();
		}
		return;
	}

	/* Layer bars come first on their own rows */
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
				setCursor(Qt::ClosedHandCursor);
			}
			selectedBar = b.id;
			emit barSelected(selectedBar);
			update();
			return;
		}
	}

	double a = 0.0, b = 0.0;
	RangeBounds(a, b);
	switch (HitAt(pos)) {
	case Hit::In:
		drag = Drag::In;
		return;
	case Hit::Out:
		drag = Drag::Out;
		return;
	case Hit::Range:
		drag = Drag::Range;
		dragStart = a;
		dragEnd = b;
		dragOffset = TimeAt(pos.x()) - a;
		setCursor(Qt::ClosedHandCursor);
		return;
	case Hit::None:
		break;
	}

	if (pos.y() >= StripTop() && pos.y() < VideoTop()) {
		/* Dragging on the strip marks a new range */
		drag = Drag::Select;
		dragStart = TimeAt(pos.x());
		return;
	}

	drag = Drag::Scrub;
	Scrub(pos.x());
}

void ClipTimeline::mouseMoveEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();

	if (drag == Drag::None) {
		UpdateHover(pos);
		return;
	}

	if (!dragMoved && (pos - pressPos).manhattanLength() >= DRAG_THRESHOLD) {
		dragMoved = true;
		if (drag == Drag::Scrub) {
			emit scrubStarted();
		}
	}

	if (drag == Drag::Pan) {
		scroll->setValue(panScroll - (pos.x() - pressPos.x()));
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
	case Drag::In: {
		const double limit =
			std::max(outPoint >= 0.0 ? outPoint - MIN_RANGE_SEC : duration - MIN_RANGE_SEC, 0.0);
		inPoint = std::min(Snap(std::clamp(t, 0.0, limit), inPoint), limit);
		emit inOutChanged(inPoint, outPoint);
		break;
	}
	case Drag::Out: {
		const double limit = std::min(inPoint >= 0.0 ? inPoint + MIN_RANGE_SEC : MIN_RANGE_SEC, duration);
		outPoint = std::max(Snap(std::clamp(t, limit, duration), outPoint), limit);
		emit inOutChanged(inPoint, outPoint);
		break;
	}
	case Drag::Range: {
		if (!dragMoved) {
			return;
		}
		const double length = dragEnd - dragStart;
		const double maxStart = std::max(duration - length, 0.0);
		double start = std::clamp(t - dragOffset, 0.0, maxStart);
		double snapped = Snap(start, -1.0, true);
		if (snapped == start) {
			snapped = Snap(start + length, -1.0, true) - length;
		}
		start = std::clamp(snapped, 0.0, maxStart);
		inPoint = start;
		outPoint = start + length;
		emit inOutChanged(inPoint, outPoint);
		break;
	}
	case Drag::Select: {
		if (!dragMoved) {
			return;
		}
		double a = std::min(dragStart, t), b = std::max(dragStart, t);
		a = Snap(a, -1.0, true);
		b = Snap(b, -1.0, true);
		if (b - a < MIN_RANGE_SEC) {
			b = std::min(a + MIN_RANGE_SEC, duration);
			a = std::max(b - MIN_RANGE_SEC, 0.0);
		}
		inPoint = a;
		outPoint = b;
		emit inOutChanged(inPoint, outPoint);
		break;
	}
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
	case Drag::Pan:
	case Drag::None:
		break;
	}
	update();
}

void ClipTimeline::mouseReleaseEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();
	const Drag finished = drag;
	drag = Drag::None;
	dragBar = -1;

	if (finished == Drag::Scrub) {
		if (dragMoved) {
			emit scrubFinished();
		} else {
			/* A click: play from here */
			emit playFromRequested(TimeAt(pos.x()));
		}
	}
	UpdateHover(pos);
}

void ClipTimeline::mouseDoubleClickEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();
	if (event->button() == Qt::LeftButton && HitAt(pos) == Hit::Range) {
		ZoomToRange();
		return;
	}
	mousePressEvent(event);
}

void ClipTimeline::leaveEvent(QEvent *event)
{
	QWidget::leaveEvent(event);
	if (drag == Drag::None && hover != Hit::None) {
		hover = Hit::None;
		update();
	}
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
	if (drag == Drag::None) {
		UpdateHover(event->position().toPoint());
	}
	event->accept();
}
