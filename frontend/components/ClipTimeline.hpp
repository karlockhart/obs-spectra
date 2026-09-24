#pragma once

#include <QColor>
#include <QPoint>
#include <QVector>
#include <QWidget>

class QScrollBar;

/*
 * Timeline for the Spectra clip maker: a time ruler, a clip strip where the
 * In/Out range is marked and trimmed with handles, a video track showing the
 * whole loop as one continuous video, and one track per censor layer with a
 * bar for the time span it covers. Times are seconds on the joined timeline
 * of all segments.
 *
 * Mouse: click the ruler or a track to play from there, drag to scrub. Drag
 * in the clip strip to mark a range, its handles to trim it and its bar to
 * move it. Ctrl+wheel zooms at the pointer, wheel scrolls, and dragging with
 * the middle button pans.
 */
class ClipTimeline : public QWidget {
	Q_OBJECT

public:
	struct Segment {
		double start = 0.0;
		double duration = 0.0;
		/* Shown where a session starts */
		QString label;
		/* First segment after a gap in recording */
		bool sessionStart = false;
	};

	struct Bar {
		int id = 0;
		QString name;
		QColor color;
		double start = 0.0;
		double end = 0.0;
		bool visible = true;
	};

	explicit ClipTimeline(QWidget *parent = nullptr);

	void SetSegments(const QVector<Segment> &segments);
	void SetBars(const QVector<Bar> &bars);
	void SetSelectedBar(int id);
	int SelectedBar() const { return selectedBar; }

	double Duration() const { return duration; }

	void SetPlayhead(double t);
	double Playhead() const { return playhead; }

	/* Negative values mean "not set" */
	void SetInOut(double in, double out);
	double In() const { return inPoint; }
	double Out() const { return outPoint; }

	/* Horizontal scale in pixels per second. Without an anchor the
	 * playhead stays put on screen if it's visible, else the view's
	 * center does. */
	void SetZoom(double pixelsPerSecond, double anchorTime = -1.0);
	void ZoomBy(double factor);
	double Zoom() const { return pxPerSec; }
	/* The zoom that fits the whole loop, and the closest zoom allowed */
	double MinZoom() const;
	double MaxZoom() const;
	void ZoomToFit();
	/* Fills the view with the In/Out range; fits the loop without one */
	void ZoomToRange();
	void EnsureVisible(double t);
	void CenterOn(double t);
	bool IsVisible(double t) const;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

	/* Localized text for the video track header and the empty timeline */
	void SetLabels(const QString &videoTrack, const QString &empty);
	/* Localized text for the clip strip header and its hint when no
	 * range is marked */
	void SetRangeLabels(const QString &header, const QString &hint);

	static QString FormatTime(double seconds, bool fractions = true);

signals:
	/* The playhead was moved by clicking or scrubbing */
	void seekRequested(double t);
	/* The user started / stopped dragging the playhead along the
	 * timeline; playback pauses in between */
	void scrubStarted();
	void scrubFinished();
	/* The timeline was clicked without dragging: play from there */
	void playFromRequested(double t);
	void inOutChanged(double in, double out);
	void barSelected(int id);
	void barChanged(int id, double start, double end);
	void zoomChanged(double pixelsPerSecond);

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	enum class Drag { None, Scrub, Pan, In, Out, Range, Select, BarMove, BarStart, BarEnd };
	/* Part of the In/Out range under the pointer */
	enum class Hit { None, In, Out, Range };

	QVector<Segment> segments;
	QVector<Bar> bars;
	QScrollBar *scroll;
	QString videoLabel = QStringLiteral("Loop video");
	QString emptyLabel = QStringLiteral("No loop recordings");
	QString rangeLabel = QStringLiteral("Clip");
	QString rangeHint;

	double duration = 0.0;
	double playhead = 0.0;
	double inPoint = -1.0;
	double outPoint = -1.0;
	double pxPerSec = 1.0;
	int selectedBar = -1;

	Drag drag = Drag::None;
	Hit hover = Hit::None;
	QPoint pressPos;
	bool dragMoved = false;
	int panScroll = 0;
	int dragBar = -1;
	double dragOffset = 0.0;
	double dragStart = 0.0, dragEnd = 0.0;

	int ContentLeft() const;
	int ContentWidth() const;
	int ContentBottom() const;
	int StripTop() const;
	int VideoTop() const;
	double ScrollTime() const;
	double TimeAt(int x) const;
	int XAt(double t) const;
	int BarRowTop(int index) const;
	int BarIndexAt(int y) const;
	/* The marked range, with an unset In as 0 and an unset Out as the
	 * end; false when neither is set */
	bool RangeBounds(double &a, double &b) const;
	Hit HitAt(const QPoint &pos) const;
	/* Pulls `t` onto a nearby point of interest. `exclude` is a value
	 * not to snap to (the one being dragged); `ignoreRange` leaves the
	 * In/Out points out when they are what's being moved. */
	double Snap(double t, double exclude = -1.0, bool ignoreRange = false) const;
	void UpdateScrollRange();
	void UpdateHover(const QPoint &pos);
	void Scrub(int x);
};
