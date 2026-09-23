#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class QScrollBar;

/*
 * Timeline for the Spectra clip maker: a time ruler with In/Out markers, a
 * video track showing the whole loop as one continuous video, and one track per
 * censor layer with a bar for the time span it covers. Times are seconds on
 * the joined timeline of all segments.
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

	/* Horizontal scale in pixels per second */
	void SetZoom(double pixelsPerSecond, double anchorTime = -1.0);
	double Zoom() const { return pxPerSec; }
	void ZoomToFit();
	void EnsureVisible(double t);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

	/* Localized text for the video track header and the empty timeline */
	void SetLabels(const QString &videoTrack, const QString &empty);

	static QString FormatTime(double seconds, bool fractions = true);

signals:
	/* The user scrubbed or clicked the timeline */
	void seekRequested(double t);
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
	void wheelEvent(QWheelEvent *event) override;

private:
	enum class Drag { None, Scrub, In, Out, BarMove, BarStart, BarEnd };

	QVector<Segment> segments;
	QVector<Bar> bars;
	QScrollBar *scroll;
	QString videoLabel = QStringLiteral("Loop video");
	QString emptyLabel = QStringLiteral("No loop recordings");

	double duration = 0.0;
	double playhead = 0.0;
	double inPoint = -1.0;
	double outPoint = -1.0;
	double pxPerSec = 1.0;
	int selectedBar = -1;

	Drag drag = Drag::None;
	int dragBar = -1;
	double dragOffset = 0.0;
	double dragStart = 0.0, dragEnd = 0.0;

	int ContentLeft() const;
	int ContentWidth() const;
	double ScrollTime() const;
	double TimeAt(int x) const;
	int XAt(double t) const;
	int BarRowTop(int index) const;
	int BarIndexAt(int y) const;
	double Snap(double t, double exclude = -1.0) const;
	void UpdateScrollRange();
	void Scrub(int x);
};
