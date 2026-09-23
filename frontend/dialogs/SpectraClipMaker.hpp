#pragma once

#include <utility/ClipRender.hpp>

#include <obs.hpp>

#include <QDateTime>
#include <QDialog>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <memory>
#include <optional>
#include <mutex>
#include <vector>

class ClipTimeline;
class LoopRecorder;
class OBSBasic;
class OBSQTDisplay;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressDialog;
class QPushButton;
class QSlider;
class QSpinBox;
class QLineEdit;

/*
 * Spectra Clip Maker: an editor over the whole loop recording. The loop's
 * segment files play as one continuous video on one timeline; the user marks
 * an In and Out point anywhere on it, adds shape layers that censor parts of
 * the picture for part or all of the clip, and exports the range as one file.
 * Saved clips can be opened the same way to trim them.
 */
class SpectraClipMaker : public QDialog {
	Q_OBJECT

public:
	SpectraClipMaker(OBSBasic *main, LoopRecorder *recorder);
	~SpectraClipMaker();

	/* Opens the loop recording at `offset` seconds into the segment file
	 * `segment`, with In and Out `before` / `after` seconds either side.
	 * Waits for the segment being recorded to be finalized if needed. */
	void ShowMoment(const QString &segment, double offset, double before, double after);

protected:
	void closeEvent(QCloseEvent *event) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	struct Segment {
		QString path;
		double start = 0.0;
		double duration = 0.0;
		QDateTime recorded;
		qint64 size = 0;
		bool sessionStart = false;
	};

	struct CensorLayer {
		int id = 0;
		QString name;
		bool visible = true;
		ClipRender::Layer layer;
	};

	/* Editing the loop recording, or trimming one saved clip */
	enum class Mode { Loop, Clips };

	struct ScannedFile {
		QString path;
		qint64 size = 0;
		QDateTime modified;
		double duration = -1.0;
	};

	enum class Handle { None, Move, N, S, E, W, NE, NW, SE, SW };

	struct ExportState {
		std::atomic<float> progress{0.0f};
		std::atomic_bool cancel{false};
	};

	OBSBasic *main;
	QPointer<LoopRecorder> recorder;

	/* Library */
	QComboBox *modeCombo;
	QPushButton *browseButton;
	QLabel *folderLabel;
	QLabel *scanLabel;
	QLabel *statusLabel;
	QListWidget *sessionList;

	/* Preview */
	OBSQTDisplay *preview;
	QPushButton *playButton;
	QLabel *timeLabel;
	QLabel *recordedLabel;
	QSlider *volumeSlider;

	/* Timeline */
	ClipTimeline *timeline;
	QLabel *rangeLabel;
	QCheckBox *reencodeCheck;
	QPushButton *exportButton;

	/* Layers */
	QListWidget *layerList;
	QWidget *layerProps;
	QLineEdit *nameEdit;
	QComboBox *shapeCombo;
	QComboBox *fillCombo;
	QPushButton *colorButton;
	QSlider *strengthSlider;
	QDoubleSpinBox *startEdit;
	QDoubleSpinBox *endEdit;
	QSpinBox *xEdit, *yEdit, *wEdit, *hEdit;

	/* A moment asked for with ShowMoment, placed once its segment is scanned */
	struct Moment {
		QString segment;
		double offset = 0.0;
		double before = 0.0;
		double after = 0.0;
		bool splitRequested = false;
	};
	std::optional<Moment> pendingMoment;

	/* Data */
	Mode mode = Mode::Loop;
	QString folder;
	QString clipFile;
	std::vector<Segment> segments;
	std::vector<CensorLayer> layers;
	int nextLayerId = 1;
	int selectedLayer = -1;
	double playhead = 0.0;
	double inPoint = -1.0;
	double outPoint = -1.0;
	bool updatingUI = false;

	/* Scanning */
	int scanGeneration = 0;
	bool scanning = false;
	bool rescanQueued = false;
	std::shared_ptr<std::atomic_bool> scanCancel;

	/* Playback: two players, so the next segment file can be opened ahead
	 * of time and the loop plays through segment boundaries seamlessly. */
	struct Player {
		OBSSourceAutoRelease source;
		int segment = -1;
		qint64 pendingSeekMs = -1;
		QDateTime loadRequested;
	};
	Player players[2];
	std::atomic_int activePlayer{0};
	QTimer pollTimer;
	bool playing = false;

	/* Overlay drawn on the preview (graphics thread reads it) */
	std::mutex overlayMutex;
	std::vector<ClipRender::Layer> overlay;
	int overlaySelected = -1;

	/* Shape dragging on the preview */
	Handle dragHandle = Handle::None;
	QPointF dragOrigin;
	ClipRender::Layer dragLayer;

	/* Export */
	std::shared_ptr<ExportState> exportState;
	QPointer<QProgressDialog> progressDialog;
	QTimer progressTimer;

	QWidget *BuildLibrary();
	QWidget *BuildPreview();
	QWidget *BuildTimeline();
	QWidget *BuildLayerPanel();
	void BuildShortcuts();

	/* Library */
	void SetMode(Mode newMode);
	void Browse();
	void OpenClip(const QString &path);
	void ResetEditing();
	void Rescan();
	void ScanFinished(int generation, const std::vector<ScannedFile> &files);
	void UpdateLibrary();
	void UpdateStatus();
	/* Sets In/Out around the pending moment once its segment is scanned */
	void PlaceMoment();

	/* Playback */
	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);
	int SegmentAt(double t) const;
	double Duration() const;
	QDateTime RecordedAt(double t) const;
	Player &Active() { return players[activePlayer]; }
	Player &Standby() { return players[1 - activePlayer]; }
	obs_source_t *ActiveSource() const { return players[activePlayer].source; }
	void LoadSegment(Player &player, int index, qint64 localMs);
	void UnloadSegment(Player &player);
	void SwapToStandby();
	void Seek(double t);
	void SetPlaying(bool play);
	void Step(double seconds);
	void Poll();
	void PlayheadChanged();

	/* In / Out */
	void SetIn(double t);
	void SetOut(double t);
	void InOutChanged();

	/* Layers */
	void AddLayer(ClipRender::Shape shape);
	void DeleteLayer();
	void MoveLayer(int direction);
	CensorLayer *Selected();
	void SelectLayer(int id);
	void LayersChanged();
	void UpdateLayerList();
	void UpdateLayerProps();
	void ApplyLayerProps();
	void UpdateOverlay();

	/* Preview interaction */
	bool MapToFrame(const QPointF &widgetPos, QPointF &norm, double &handleSize) const;
	Handle HandleAt(const ClipRender::Layer &layer, const QPointF &norm, double handleSize) const;
	void PreviewMousePress(QMouseEvent *event);
	void PreviewMouseMove(QMouseEvent *event);
	void PreviewMouseRelease(QMouseEvent *event);

	/* Export */
	void Export();
	void ExportFinished(bool ok, const QString &path, const QString &error);
};
