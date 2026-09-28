#include "SpectraClipMaker.hpp"

#include <components/ClipTimeline.hpp>
#include <dialogs/SpectraYouTubeUpload.hpp>
#include <utility/ClipExport.hpp>
#include <utility/LoopRecorder.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <thread>

using ClipRender::Fill;
using ClipRender::Shape;

/* Recording gaps longer than this start a new session on the timeline */
static constexpr double SESSION_GAP_SEC = 10.0;
static constexpr int POLL_MS = 33;
/* A source update is applied on the next video tick; wait for it before
 * seeking the newly loaded file. */
static constexpr int LOAD_SETTLE_MS = 120;
/* Open the next segment this long before the current one ends */
static constexpr double PRELOAD_SEC = 3.0;
static constexpr double STEP_SEC = 1.0 / 60.0;
static constexpr double JUMP_SEC = 5.0;
static constexpr double MIN_SHAPE = 0.01;
/* Zoom slider resolution */
static constexpr int ZOOM_STEPS = 1000;
static constexpr double PI = 3.14159265358979323846;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */

namespace {

/* Durations are expensive to probe for long segments; keep them for the
 * session so reopening the editor or refreshing is instant. */
struct DurationCacheEntry {
	qint64 size;
	QDateTime modified;
	double duration;
};
std::mutex durationCacheMutex;
QHash<QString, DurationCacheEntry> durationCache;

/* Spin box that shows and parses HH:MM:SS.cc */
class TimeSpinBox : public QDoubleSpinBox {
public:
	explicit TimeSpinBox(QWidget *parent = nullptr) : QDoubleSpinBox(parent)
	{
		setDecimals(2);
		setSingleStep(0.1);
		setKeyboardTracking(false);
		setAccelerated(true);
	}

protected:
	QString textFromValue(double value) const override { return ClipTimeline::FormatTime(value); }

	double valueFromText(const QString &text) const override
	{
		double total = 0.0;
		for (const QString &part : text.trimmed().split(':')) {
			total = total * 60.0 + part.toDouble();
		}
		return total;
	}

	QValidator::State validate(QString &text, int &) const override
	{
		static const QRegularExpression re("^\\s*(\\d+:){0,2}\\d*(\\.\\d*)?\\s*$");
		return re.match(text).hasMatch() ? QValidator::Acceptable : QValidator::Invalid;
	}
};

QColor BarColor(const ClipRender::Layer &layer)
{
	switch (layer.fill) {
	case Fill::Pixelate:
		return QColor(150, 100, 220);
	case Fill::Blur:
		return QColor(80, 150, 230);
	case Fill::Solid:
		break;
	}
	QColor c(layer.r, layer.g, layer.b);
	/* Keep black bars visible on the dark timeline */
	return c.lightness() < 60 ? QColor(95, 95, 100) : c;
}

QString FormatDuration(double seconds)
{
	qint64 s = (qint64)std::llround(seconds);
	if (s >= 3600) {
		return QStringLiteral("%1h %2m").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0'));
	}
	return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QPushButton *ToolButton(const QString &text, const QString &tip = QString())
{
	QPushButton *button = new QPushButton(text);
	button->setFocusPolicy(Qt::NoFocus);
	if (!tip.isEmpty()) {
		button->setToolTip(tip);
	}
	return button;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Construction                                                              */

SpectraClipMaker::SpectraClipMaker(OBSBasic *main_, LoopRecorder *recorder_)
	: QDialog(main_),
	  main(main_),
	  recorder(recorder_)
{
	setWindowTitle(QTStr("Spectra.ClipMaker.Title"));
	setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1400, 860);

	folder = recorder ? recorder->LoopDirectory() : QString();

	QSplitter *top = new QSplitter(Qt::Horizontal);
	top->addWidget(BuildLibrary());
	top->addWidget(BuildPreview());
	top->addWidget(BuildSidePanel());
	top->setStretchFactor(0, 0);
	top->setStretchFactor(1, 1);
	top->setStretchFactor(2, 0);
	top->setSizes({260, 820, 320});

	QSplitter *vertical = new QSplitter(Qt::Vertical);
	vertical->addWidget(top);
	vertical->addWidget(BuildTimeline());
	vertical->setStretchFactor(0, 1);
	vertical->setStretchFactor(1, 0);
	vertical->setSizes({560, 300});

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->addWidget(vertical);

	BuildShortcuts();

	/* Preview players: each plays one segment at a time, audio to the monitoring
	 * device only so it never ends up in a recording or stream. */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_bool(settings, "hw_decode", true);
	for (int i = 0; i < 2; i++) {
		QString name = QStringLiteral("Spectra Clip Maker Preview %1").arg(i + 1);
		players[i].source = obs_source_create_private("ffmpeg_source", QT_TO_UTF8(name), settings);
		obs_source_t *src = players[i].source;
		if (src) {
			obs_source_set_monitoring_enabled(src, true);
			obs_source_set_audio_mixers(src, 0);
			obs_source_set_volume(src, volumeSlider->value() / 100.0f);
			obs_source_set_muted(src, true);
			obs_source_inc_showing(src);
		}
	}

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(preview->GetDisplay(), SpectraClipMaker::DrawPreview, this);
	};
	connect(preview, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
	preview->installEventFilter(this);
	preview->setMouseTracking(true);

	pollTimer.setInterval(POLL_MS);
	connect(&pollTimer, &QTimer::timeout, this, &SpectraClipMaker::Poll);
	pollTimer.start();

	if (recorder) {
		connect(recorder, &LoopRecorder::segmentsChanged, this, &SpectraClipMaker::Rescan);
		connect(recorder, &LoopRecorder::activeChanged, this, &SpectraClipMaker::UpdateStatus);
		connect(recorder, &LoopRecorder::armedChanged, this, &SpectraClipMaker::UpdateStatus);
		connect(recorder, &LoopRecorder::recordingStatusChanged, this, &SpectraClipMaker::UpdateStatus);
	}

	UpdateStatus();
	UpdateLayerProps();
	InOutChanged();
	PlayheadChanged();
	Rescan();
}

SpectraClipMaker::~SpectraClipMaker()
{
	pollTimer.stop();
	progressTimer.stop();
	if (scanCancel) {
		*scanCancel = true;
	}
	if (exportState) {
		exportState->cancel = true;
	}
	obs_display_remove_draw_callback(preview->GetDisplay(), SpectraClipMaker::DrawPreview, this);
	for (Player &player : players) {
		if (player.source) {
			obs_source_dec_showing(player.source);
		}
	}
	if (captionTexture) {
		obs_enter_graphics();
		gs_texture_destroy(captionTexture);
		obs_leave_graphics();
	}
}

QWidget *SpectraClipMaker::BuildLibrary()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	modeCombo = new QComboBox();
	modeCombo->addItem(QTStr("Spectra.ClipMaker.Mode.Loop"), (int)Mode::Loop);
	modeCombo->addItem(QTStr("Spectra.ClipMaker.Mode.Clips"), (int)Mode::Clips);
	modeCombo->setToolTip(QTStr("Spectra.ClipMaker.ModeTip"));
	layout->addWidget(modeCombo);
	connect(modeCombo, &QComboBox::currentIndexChanged, this,
		[this]() { SetMode((Mode)modeCombo->currentData().toInt()); });

	folderLabel = new QLabel();
	folderLabel->setWordWrap(true);
	folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(folderLabel);

	QHBoxLayout *buttons = new QHBoxLayout();
	QPushButton *refresh = ToolButton(QTStr("Spectra.ClipMaker.Refresh"));
	QPushButton *open = ToolButton(QTStr("Spectra.ClipMaker.OpenFolder"));
	browseButton = ToolButton(QTStr("Browse"));
	buttons->addWidget(refresh);
	buttons->addWidget(open);
	buttons->addWidget(browseButton);
	layout->addLayout(buttons);
	connect(refresh, &QPushButton::clicked, this, &SpectraClipMaker::Rescan);
	connect(open, &QPushButton::clicked, this, [this]() {
		QDir().mkpath(folder);
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
	});
	connect(browseButton, &QPushButton::clicked, this, &SpectraClipMaker::Browse);

	scanLabel = new QLabel();
	scanLabel->setWordWrap(true);
	layout->addWidget(scanLabel);

	sessionList = new QListWidget();
	sessionList->setToolTip(QTStr("Spectra.ClipMaker.SessionsTip"));
	layout->addWidget(sessionList, 1);
	connect(sessionList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		if (mode == Mode::Clips) {
			OpenClip(item->data(Qt::UserRole).toString());
			return;
		}
		double t = item->data(Qt::UserRole).toDouble();
		Seek(t);
		timeline->EnsureVisible(t);
	});

	statusLabel = new QLabel();
	statusLabel->setWordWrap(true);
	statusLabel->setFrameShape(QFrame::StyledPanel);
	statusLabel->setMargin(6);
	layout->addWidget(statusLabel);

	return panel;
}

QWidget *SpectraClipMaker::BuildPreview()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	preview = new OBSQTDisplay();
	preview->setMinimumSize(320, 180);
	preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	layout->addWidget(preview, 1);

	QHBoxLayout *transport = new QHBoxLayout();
	QPushButton *toIn = ToolButton(QStringLiteral("⏮"), QTStr("Spectra.ClipMaker.GoToIn"));
	QPushButton *back = ToolButton(QStringLiteral("⏪"), QTStr("Spectra.ClipMaker.Back"));
	playButton = ToolButton(QStringLiteral("▶"), QTStr("Spectra.ClipMaker.PlayTip"));
	playButton->setMinimumWidth(56);
	QPushButton *forward = ToolButton(QStringLiteral("⏩"), QTStr("Spectra.ClipMaker.Forward"));
	QPushButton *toOut = ToolButton(QStringLiteral("⏭"), QTStr("Spectra.ClipMaker.GoToOut"));
	for (QPushButton *b : {toIn, back, playButton, forward, toOut}) {
		transport->addWidget(b);
	}
	connect(toIn, &QPushButton::clicked, this, [this]() { Seek(inPoint >= 0.0 ? inPoint : 0.0); });
	connect(toOut, &QPushButton::clicked, this, [this]() { Seek(outPoint >= 0.0 ? outPoint : Duration()); });
	connect(back, &QPushButton::clicked, this, [this]() { Step(-JUMP_SEC); });
	connect(forward, &QPushButton::clicked, this, [this]() { Step(JUMP_SEC); });
	connect(playButton, &QPushButton::clicked, this, [this]() { SetPlaying(!playing); });

	timeLabel = new QLabel();
	timeLabel->setStyleSheet("font-family: monospace; font-size: 14px;");
	transport->addSpacing(12);
	transport->addWidget(timeLabel);
	transport->addStretch();

	recordedLabel = new QLabel();
	recordedLabel->setToolTip(QTStr("Spectra.ClipMaker.RecordedTip"));
	transport->addWidget(recordedLabel);
	transport->addSpacing(12);

	transport->addWidget(new QLabel(QStringLiteral("🔊")));
	volumeSlider = new QSlider(Qt::Horizontal);
	volumeSlider->setRange(0, 100);
	volumeSlider->setValue(80);
	volumeSlider->setMaximumWidth(110);
	volumeSlider->setFocusPolicy(Qt::NoFocus);
	transport->addWidget(volumeSlider);
	connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) {
		for (Player &player : players) {
			if (player.source) {
				obs_source_set_volume(player.source, value / 100.0f);
			}
		}
	});

	layout->addLayout(transport);
	return panel;
}

QWidget *SpectraClipMaker::BuildTimeline()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	QHBoxLayout *tools = new QHBoxLayout();
	QPushButton *setIn = ToolButton(QTStr("Spectra.ClipMaker.SetIn"), QTStr("Spectra.ClipMaker.SetInTip"));
	QPushButton *setOut = ToolButton(QTStr("Spectra.ClipMaker.SetOut"), QTStr("Spectra.ClipMaker.SetOutTip"));
	QPushButton *clear = ToolButton(QTStr("Spectra.ClipMaker.ClearInOut"));
	QPushButton *addRect = ToolButton(QTStr("Spectra.ClipMaker.AddRectangle"));
	QPushButton *addEllipse = ToolButton(QTStr("Spectra.ClipMaker.AddEllipse"));
	QPushButton *fit = ToolButton(QTStr("Spectra.ClipMaker.Fit"), QTStr("Spectra.ClipMaker.FitTip"));
	QPushButton *fitRange = ToolButton(QTStr("Spectra.ClipMaker.FitRange"), QTStr("Spectra.ClipMaker.FitRangeTip"));
	QPushButton *zoomIn = ToolButton(QStringLiteral("+"), QTStr("Spectra.ClipMaker.ZoomIn"));
	QPushButton *zoomOut = ToolButton(QStringLiteral("−"), QTStr("Spectra.ClipMaker.ZoomOut"));
	zoomSlider = new QSlider(Qt::Horizontal);
	zoomSlider->setRange(0, ZOOM_STEPS);
	zoomSlider->setFixedWidth(120);
	zoomSlider->setFocusPolicy(Qt::NoFocus);
	zoomSlider->setToolTip(QTStr("Spectra.ClipMaker.ZoomSliderTip"));

	tools->addWidget(setIn);
	tools->addWidget(setOut);
	tools->addWidget(clear);
	tools->addSpacing(16);
	tools->addWidget(addRect);
	tools->addWidget(addEllipse);
	tools->addSpacing(16);
	tools->addWidget(zoomOut);
	tools->addWidget(zoomSlider);
	tools->addWidget(zoomIn);
	tools->addWidget(fit);
	tools->addWidget(fitRange);
	tools->addSpacing(16);
	rangeLabel = new QLabel();
	tools->addWidget(rangeLabel);
	tools->addStretch();

	config_t *config = main->Config();
	tools->addWidget(new QLabel(QTStr("Spectra.ClipMaker.Quality")));
	qualityCombo = new QComboBox();
	qualityCombo->setFocusPolicy(Qt::NoFocus);
	qualityCombo->setToolTip(QTStr("Spectra.ClipMaker.QualityTip"));
	FillQualityCombo(qualityCombo);
	int qualityIndex = qualityCombo->findData(
		QString::fromUtf8(config_get_string(config, "SpectraClipMaker", "ExportQuality")));
	qualityCombo->setCurrentIndex(std::max(qualityIndex, 0));
	tools->addWidget(qualityCombo);

	reencodeCheck = new QCheckBox(QTStr("Spectra.ClipMaker.Reencode"));
	reencodeCheck->setToolTip(QTStr("Spectra.ClipMaker.ReencodeTip"));
	reencodeCheck->setFocusPolicy(Qt::NoFocus);
	reencodeCheck->setChecked(config_get_bool(config, "SpectraClipMaker", "FrameAccurate"));
	/* Every quality but the original re-encodes, which is frame-accurate */
	reencodeCheck->setEnabled(qualityCombo->currentIndex() == 0);
	tools->addWidget(reencodeCheck);
	connect(qualityCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
		reencodeCheck->setEnabled(index == 0);
		SaveExportSettings();
	});
	connect(reencodeCheck, &QCheckBox::toggled, this, &SpectraClipMaker::SaveExportSettings);
	exportButton = new QPushButton(QTStr("Spectra.ClipMaker.Export"));
	exportButton->setToolTip(QTStr("Spectra.ClipMaker.ExportTip"));
	exportButton->setFocusPolicy(Qt::NoFocus);
	exportButton->setDefault(false);
	exportButton->setAutoDefault(false);
	exportButton->setStyleSheet("font-weight: bold; padding: 4px 18px;");
	tools->addWidget(exportButton);
	uploadButton = new QPushButton(QTStr("Spectra.ClipMaker.UploadToYouTube"));
	uploadButton->setToolTip(QTStr("Spectra.ClipMaker.UploadToYouTubeTip"));
	uploadButton->setFocusPolicy(Qt::NoFocus);
	uploadButton->setDefault(false);
	uploadButton->setAutoDefault(false);
	tools->addWidget(uploadButton);
	layout->addLayout(tools);

	connect(setIn, &QPushButton::clicked, this, [this]() { SetIn(playhead); });
	connect(setOut, &QPushButton::clicked, this, [this]() { SetOut(playhead); });
	connect(clear, &QPushButton::clicked, this, [this]() {
		inPoint = outPoint = -1.0;
		InOutChanged();
	});
	connect(addRect, &QPushButton::clicked, this, [this]() { AddLayer(Shape::Rectangle); });
	connect(addEllipse, &QPushButton::clicked, this, [this]() { AddLayer(Shape::Ellipse); });
	connect(exportButton, &QPushButton::clicked, this, &SpectraClipMaker::Export);
	connect(uploadButton, &QPushButton::clicked, this, &SpectraClipMaker::UploadToYouTube);

	timeline = new ClipTimeline();
	timeline->SetLabels(QTStr("Spectra.ClipMaker.VideoTrack"), QTStr("Spectra.ClipMaker.NoRecordings"));
	QScrollArea *scroll = new QScrollArea();
	scroll->setWidget(timeline);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setFrameShape(QFrame::NoFrame);
	layout->addWidget(scroll, 1);

	timeline->SetRangeLabels(QTStr("Spectra.ClipMaker.RangeTrack"), QTStr("Spectra.ClipMaker.RangeHint"));
	connect(fit, &QPushButton::clicked, timeline, &ClipTimeline::ZoomToFit);
	connect(fitRange, &QPushButton::clicked, timeline, &ClipTimeline::ZoomToRange);
	connect(zoomIn, &QPushButton::clicked, this, [this]() { timeline->ZoomBy(2.0); });
	connect(zoomOut, &QPushButton::clicked, this, [this]() { timeline->ZoomBy(0.5); });
	connect(zoomSlider, &QSlider::valueChanged, this, [this](int value) {
		if (updatingZoom) {
			return;
		}
		const double lo = timeline->MinZoom(), hi = timeline->MaxZoom();
		updatingZoom = true;
		timeline->SetZoom(lo * std::pow(hi / lo, value / (double)ZOOM_STEPS));
		updatingZoom = false;
	});
	connect(timeline, &ClipTimeline::zoomChanged, this, &SpectraClipMaker::UpdateZoomSlider);
	UpdateZoomSlider();
	connect(timeline, &ClipTimeline::seekRequested, this, &SpectraClipMaker::Seek);
	connect(timeline, &ClipTimeline::scrubStarted, this, [this]() {
		resumeAfterScrub = playing;
		if (playing) {
			SetPlaying(false);
		}
	});
	connect(timeline, &ClipTimeline::scrubFinished, this, [this]() {
		if (resumeAfterScrub) {
			SetPlaying(true);
		}
		resumeAfterScrub = false;
	});
	connect(timeline, &ClipTimeline::playFromRequested, this, [this](double t) {
		Seek(t);
		SetPlaying(true);
	});
	connect(timeline, &ClipTimeline::inOutChanged, this, [this](double in, double out) {
		inPoint = in;
		outPoint = out;
		InOutChanged();
	});
	connect(timeline, &ClipTimeline::barSelected, this, &SpectraClipMaker::SelectLayer);
	connect(timeline, &ClipTimeline::barChanged, this, [this](int id, double start, double end) {
		for (CensorLayer &l : layers) {
			if (l.id == id) {
				l.layer.start = start;
				l.layer.end = end;
			}
		}
		UpdateLayerProps();
		UpdateOverlay();
	});

	return panel;
}

QWidget *SpectraClipMaker::BuildLayerPanel()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	QLabel *title = new QLabel(QTStr("Spectra.ClipMaker.Layers"));
	title->setStyleSheet("font-weight: bold;");
	layout->addWidget(title);

	layerList = new QListWidget();
	layerList->setToolTip(QTStr("Spectra.ClipMaker.LayersTip"));
	layerList->setMaximumHeight(170);
	layout->addWidget(layerList);
	connect(layerList, &QListWidget::currentRowChanged, this, [this](int row) {
		if (updatingUI) {
			return;
		}
		SelectLayer(row >= 0 && row < (int)layers.size() ? layers[layers.size() - 1 - row].id : -1);
	});
	connect(layerList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
		if (updatingUI) {
			return;
		}
		int id = item->data(Qt::UserRole).toInt();
		for (CensorLayer &l : layers) {
			if (l.id == id) {
				l.visible = item->checkState() == Qt::Checked;
			}
		}
		LayersChanged();
	});

	QHBoxLayout *order = new QHBoxLayout();
	QPushButton *up = ToolButton(QStringLiteral("▲"), QTStr("Spectra.ClipMaker.MoveUp"));
	QPushButton *down = ToolButton(QStringLiteral("▼"), QTStr("Spectra.ClipMaker.MoveDown"));
	QPushButton *remove = ToolButton(QTStr("Remove"), QTStr("Spectra.ClipMaker.DeleteTip"));
	order->addWidget(up);
	order->addWidget(down);
	order->addStretch();
	order->addWidget(remove);
	layout->addLayout(order);
	connect(up, &QPushButton::clicked, this, [this]() { MoveLayer(1); });
	connect(down, &QPushButton::clicked, this, [this]() { MoveLayer(-1); });
	connect(remove, &QPushButton::clicked, this, &SpectraClipMaker::DeleteLayer);

	layerProps = new QGroupBox(QTStr("Spectra.ClipMaker.LayerProps"));
	QFormLayout *form = new QFormLayout(layerProps);

	nameEdit = new QLineEdit();
	form->addRow(QTStr("Name"), nameEdit);

	shapeCombo = new QComboBox();
	shapeCombo->addItem(QTStr("Spectra.ClipMaker.Rectangle"), (int)Shape::Rectangle);
	shapeCombo->addItem(QTStr("Spectra.ClipMaker.Ellipse"), (int)Shape::Ellipse);
	form->addRow(QTStr("Spectra.ClipMaker.Shape"), shapeCombo);

	fillCombo = new QComboBox();
	fillCombo->addItem(QTStr("Spectra.ClipMaker.Solid"), (int)Fill::Solid);
	fillCombo->addItem(QTStr("Spectra.ClipMaker.Pixelate"), (int)Fill::Pixelate);
	fillCombo->addItem(QTStr("Spectra.ClipMaker.Blur"), (int)Fill::Blur);
	form->addRow(QTStr("Spectra.ClipMaker.Fill"), fillCombo);

	colorButton = new QPushButton();
	form->addRow(QTStr("Spectra.ClipMaker.Color"), colorButton);

	strengthSlider = new QSlider(Qt::Horizontal);
	strengthSlider->setRange(1, 100);
	form->addRow(QTStr("Spectra.ClipMaker.Strength"), strengthSlider);

	startEdit = new TimeSpinBox();
	endEdit = new TimeSpinBox();
	QPushButton *startHere = ToolButton(QStringLiteral("⌖"), QTStr("Spectra.ClipMaker.StartAtPlayhead"));
	QPushButton *endHere = ToolButton(QStringLiteral("⌖"), QTStr("Spectra.ClipMaker.EndAtPlayhead"));
	auto timeRow = [](QDoubleSpinBox *edit, QPushButton *button) {
		QWidget *row = new QWidget();
		QHBoxLayout *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		h->addWidget(edit, 1);
		h->addWidget(button);
		return row;
	};
	form->addRow(QTStr("Spectra.ClipMaker.Start"), timeRow(startEdit, startHere));
	form->addRow(QTStr("Spectra.ClipMaker.End"), timeRow(endEdit, endHere));

	QHBoxLayout *span = new QHBoxLayout();
	QPushButton *spanClip = ToolButton(QTStr("Spectra.ClipMaker.SpanClip"), QTStr("Spectra.ClipMaker.SpanClipTip"));
	QPushButton *spanAll = ToolButton(QTStr("Spectra.ClipMaker.SpanAll"), QTStr("Spectra.ClipMaker.SpanAllTip"));
	span->addWidget(spanClip);
	span->addWidget(spanAll);
	form->addRow(QString(), span);

	auto percent = []() {
		QSpinBox *box = new QSpinBox();
		box->setRange(0, 100);
		box->setSuffix(QStringLiteral(" %"));
		box->setKeyboardTracking(false);
		return box;
	};
	xEdit = percent();
	yEdit = percent();
	wEdit = percent();
	hEdit = percent();
	wEdit->setMinimum(1);
	hEdit->setMinimum(1);
	QHBoxLayout *pos = new QHBoxLayout();
	pos->addWidget(new QLabel(QStringLiteral("X")));
	pos->addWidget(xEdit);
	pos->addWidget(new QLabel(QStringLiteral("Y")));
	pos->addWidget(yEdit);
	form->addRow(QTStr("Spectra.ClipMaker.Position"), pos);
	QHBoxLayout *size = new QHBoxLayout();
	size->addWidget(new QLabel(QStringLiteral("W")));
	size->addWidget(wEdit);
	size->addWidget(new QLabel(QStringLiteral("H")));
	size->addWidget(hEdit);
	form->addRow(QTStr("Spectra.ClipMaker.Size"), size);

	QPushButton *fullFrame =
		ToolButton(QTStr("Spectra.ClipMaker.FullFrame"), QTStr("Spectra.ClipMaker.FullFrameTip"));
	form->addRow(QString(), fullFrame);

	layout->addWidget(layerProps);

	QLabel *hint = new QLabel(QTStr("Spectra.ClipMaker.LayerHint"));
	hint->setWordWrap(true);
	layout->addWidget(hint);
	layout->addStretch();

	/* Property edits */
	connect(nameEdit, &QLineEdit::textEdited, this, &SpectraClipMaker::ApplyLayerProps);
	connect(shapeCombo, &QComboBox::currentIndexChanged, this, &SpectraClipMaker::ApplyLayerProps);
	connect(fillCombo, &QComboBox::currentIndexChanged, this, &SpectraClipMaker::ApplyLayerProps);
	connect(strengthSlider, &QSlider::valueChanged, this, &SpectraClipMaker::ApplyLayerProps);
	connect(startEdit, &QDoubleSpinBox::valueChanged, this, &SpectraClipMaker::ApplyLayerProps);
	connect(endEdit, &QDoubleSpinBox::valueChanged, this, &SpectraClipMaker::ApplyLayerProps);
	for (QSpinBox *box : {xEdit, yEdit, wEdit, hEdit}) {
		connect(box, &QSpinBox::valueChanged, this, &SpectraClipMaker::ApplyLayerProps);
	}

	connect(colorButton, &QPushButton::clicked, this, [this]() {
		CensorLayer *l = Selected();
		if (!l) {
			return;
		}
		QColor color = QColorDialog::getColor(QColor(l->layer.r, l->layer.g, l->layer.b), this);
		if (color.isValid()) {
			l->layer.r = (uint8_t)color.red();
			l->layer.g = (uint8_t)color.green();
			l->layer.b = (uint8_t)color.blue();
			LayersChanged();
		}
	});
	connect(startHere, &QPushButton::clicked, this, [this]() {
		if (CensorLayer *l = Selected()) {
			l->layer.start = std::min(playhead, l->layer.end - 0.1);
			LayersChanged();
		}
	});
	connect(endHere, &QPushButton::clicked, this, [this]() {
		if (CensorLayer *l = Selected()) {
			l->layer.end = std::max(playhead, l->layer.start + 0.1);
			LayersChanged();
		}
	});
	connect(spanClip, &QPushButton::clicked, this, [this]() {
		CensorLayer *l = Selected();
		if (!l || inPoint < 0.0 || outPoint <= inPoint) {
			return;
		}
		l->layer.start = inPoint;
		l->layer.end = outPoint;
		LayersChanged();
	});
	connect(spanAll, &QPushButton::clicked, this, [this]() {
		if (CensorLayer *l = Selected()) {
			l->layer.start = 0.0;
			l->layer.end = std::max(Duration(), 0.1);
			LayersChanged();
		}
	});
	connect(fullFrame, &QPushButton::clicked, this, [this]() {
		if (CensorLayer *l = Selected()) {
			l->layer.x = l->layer.y = 0.0;
			l->layer.w = l->layer.h = 1.0;
			l->layer.shape = Shape::Rectangle;
			LayersChanged();
		}
	});

	return panel;
}

QWidget *SpectraClipMaker::BuildSidePanel()
{
	QTabWidget *tabs = new QTabWidget();
	tabs->addTab(BuildLayerPanel(), QTStr("Spectra.ClipMaker.CensorTab"));
	tabs->addTab(BuildCaptionPanel(), QTStr("Spectra.ClipMaker.CaptionsTab"));
	return tabs;
}

enum CaptionColumn { kCaptionTime, kCaptionWho, kCaptionText };

QWidget *SpectraClipMaker::BuildCaptionPanel()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	transcribeButton = new QPushButton(QTStr("Spectra.ClipMaker.Captions.Transcribe"));
	transcribeButton->setToolTip(QTStr("Spectra.ClipMaker.Captions.TranscribeTip"));
	connect(transcribeButton, &QPushButton::clicked, this, &SpectraClipMaker::Transcribe);
	layout->addWidget(transcribeButton);

	captionInfo = new QLabel(QTStr("Spectra.ClipMaker.Captions.Info"));
	captionInfo->setWordWrap(true);
	layout->addWidget(captionInfo);

	captionTable = new QTableWidget(0, 3);
	captionTable->setHorizontalHeaderLabels({QTStr("Spectra.ClipMaker.Captions.Time"),
						 QTStr("Spectra.ClipMaker.Captions.Who"),
						 QTStr("Spectra.ClipMaker.Captions.Text")});
	captionTable->horizontalHeader()->setSectionResizeMode(kCaptionTime, QHeaderView::ResizeToContents);
	captionTable->horizontalHeader()->setSectionResizeMode(kCaptionWho, QHeaderView::ResizeToContents);
	captionTable->horizontalHeader()->setStretchLastSection(true);
	captionTable->verticalHeader()->setVisible(false);
	captionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	captionTable->setSelectionMode(QAbstractItemView::SingleSelection);
	captionTable->setWordWrap(true);
	layout->addWidget(captionTable, 1);

	connect(captionTable, &QTableWidget::currentCellChanged, this, [this](int row) {
		if (updatingUI) {
			return;
		}
		if (row >= 0 && row < (int)captions.size()) {
			Seek(captions[row].start);
		}
		UpdateCaptionProps();
	});
	connect(captionTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
		if (updatingUI || item->column() != kCaptionText || item->row() >= (int)captions.size()) {
			return;
		}
		captions[item->row()].text = item->text().simplified();
		shownCaption = -2;
		UpdateCaptionPreview();
	});

	QHBoxLayout *edit = new QHBoxLayout();
	QPushButton *add =
		ToolButton(QTStr("Spectra.ClipMaker.Captions.Add"), QTStr("Spectra.ClipMaker.Captions.AddTip"));
	QPushButton *remove = ToolButton(QTStr("Remove"), QTStr("Spectra.ClipMaker.Captions.DeleteTip"));
	edit->addWidget(add);
	edit->addWidget(remove);
	edit->addStretch();
	layout->addLayout(edit);

	captionStart = new QDoubleSpinBox();
	captionEnd = new QDoubleSpinBox();
	for (QDoubleSpinBox *box : {captionStart, captionEnd}) {
		box->setDecimals(2);
		box->setSingleStep(0.1);
		box->setRange(0.0, 1e7);
		box->setSuffix(QStringLiteral(" s"));
	}
	QPushButton *startHere = ToolButton(QTStr("Spectra.ClipMaker.Captions.StartHere"),
					    QTStr("Spectra.ClipMaker.Captions.StartHereTip"));
	QPushButton *endHere =
		ToolButton(QTStr("Spectra.ClipMaker.Captions.EndHere"), QTStr("Spectra.ClipMaker.Captions.EndHereTip"));
	QFormLayout *times = new QFormLayout();
	QHBoxLayout *startRow = new QHBoxLayout();
	startRow->addWidget(captionStart, 1);
	startRow->addWidget(startHere);
	QHBoxLayout *endRow = new QHBoxLayout();
	endRow->addWidget(captionEnd, 1);
	endRow->addWidget(endHere);
	times->addRow(QTStr("Spectra.ClipMaker.Captions.Start"), startRow);
	times->addRow(QTStr("Spectra.ClipMaker.Captions.End"), endRow);
	layout->addLayout(times);

	speakerCheck = new QCheckBox(QTStr("Spectra.ClipMaker.Captions.ShowSpeaker"));
	burnCaptionsCheck = new QCheckBox(QTStr("Spectra.ClipMaker.Captions.Burn"));
	burnCaptionsCheck->setChecked(true);
	srtCheck = new QCheckBox(QTStr("Spectra.ClipMaker.Captions.Srt"));
	srtCheck->setChecked(true);
	layout->addWidget(speakerCheck);
	layout->addWidget(burnCaptionsCheck);
	layout->addWidget(srtCheck);

	connect(speakerCheck, &QCheckBox::toggled, this, [this]() {
		shownCaption = -2;
		UpdateCaptionPreview();
	});
	connect(add, &QPushButton::clicked, this, [this]() {
		ClipCaptions::Cue cue;
		cue.start = playhead;
		cue.end = std::min(playhead + 3.0, std::max(Duration(), playhead + 0.1));
		cue.text = QTStr("Spectra.ClipMaker.Captions.NewText");
		auto at = std::upper_bound(captions.begin(), captions.end(), cue.start,
					   [](double t, const ClipCaptions::Cue &c) { return t < c.start; });
		const int row = (int)(at - captions.begin());
		captions.insert(at, cue);
		CaptionsChanged();
		captionTable->setCurrentCell(row, kCaptionText);
		captionTable->editItem(captionTable->item(row, kCaptionText));
	});
	connect(remove, &QPushButton::clicked, this, [this]() {
		const int row = SelectedCaption();
		if (row >= 0) {
			captions.erase(captions.begin() + row);
			CaptionsChanged();
		}
	});
	auto setTimes = [this](double start, double end) {
		const int row = SelectedCaption();
		if (row < 0) {
			return;
		}
		ClipCaptions::Cue &cue = captions[row];
		cue.start = std::max(0.0, start);
		cue.end = std::max(cue.start + 0.1, end);
		CaptionsChanged();
	};
	connect(captionStart, &QDoubleSpinBox::valueChanged, this, [this, setTimes](double v) {
		const int row = SelectedCaption();
		if (!updatingUI && row >= 0) {
			setTimes(v, captions[row].end);
		}
	});
	connect(captionEnd, &QDoubleSpinBox::valueChanged, this, [this, setTimes](double v) {
		const int row = SelectedCaption();
		if (!updatingUI && row >= 0) {
			setTimes(captions[row].start, v);
		}
	});
	connect(startHere, &QPushButton::clicked, this, [this, setTimes]() {
		const int row = SelectedCaption();
		if (row >= 0) {
			setTimes(playhead, captions[row].end);
		}
	});
	connect(endHere, &QPushButton::clicked, this, [this, setTimes]() {
		const int row = SelectedCaption();
		if (row >= 0) {
			setTimes(captions[row].start, playhead);
		}
	});

	UpdateCaptionProps();
	return panel;
}

void SpectraClipMaker::BuildShortcuts()
{
	auto add = [this](const QKeySequence &key, auto fn) {
		QShortcut *shortcut = new QShortcut(key, this);
		connect(shortcut, &QShortcut::activated, this, fn);
	};
	add(Qt::Key_Space, [this]() { SetPlaying(!playing); });
	add(Qt::Key_K, [this]() { SetPlaying(!playing); });
	add(Qt::Key_I, [this]() { SetIn(playhead); });
	add(Qt::Key_O, [this]() { SetOut(playhead); });
	add(Qt::Key_Left, [this]() { Step(-STEP_SEC); });
	add(Qt::Key_Right, [this]() { Step(STEP_SEC); });
	add(Qt::Key_J, [this]() { Step(-JUMP_SEC); });
	add(Qt::Key_L, [this]() { Step(JUMP_SEC); });
	add(QKeySequence(Qt::SHIFT | Qt::Key_Left), [this]() { Step(-JUMP_SEC); });
	add(QKeySequence(Qt::SHIFT | Qt::Key_Right), [this]() { Step(JUMP_SEC); });
	add(Qt::Key_Home, [this]() { Seek(inPoint >= 0.0 ? inPoint : 0.0); });
	add(Qt::Key_End, [this]() { Seek(outPoint >= 0.0 ? outPoint : Duration()); });
	add(Qt::Key_Delete, [this]() { DeleteLayer(); });
	add(QKeySequence(Qt::CTRL | Qt::Key_E), [this]() { Export(); });
	add(QKeySequence(Qt::CTRL | Qt::Key_0), [this]() { timeline->ZoomToFit(); });
	add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_0), [this]() { timeline->ZoomToRange(); });
	add(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this]() { timeline->ZoomBy(2.0); });
	add(QKeySequence(Qt::CTRL | Qt::Key_Minus), [this]() { timeline->ZoomBy(0.5); });
}

/* ------------------------------------------------------------------------- */
/* Library                                                                   */

void SpectraClipMaker::ResetEditing()
{
	SetPlaying(false);
	UnloadSegment(players[0]);
	UnloadSegment(players[1]);
	segments.clear();
	layers.clear();
	captions.clear();
	selectedLayer = -1;
	inPoint = outPoint = -1.0;
	playhead = 0.0;
	timeline->SetSegments({});
	LayersChanged();
	CaptionsChanged();
	InOutChanged();
	PlayheadChanged();
}

void SpectraClipMaker::SetMode(Mode newMode)
{
	if (mode == newMode) {
		return;
	}
	if (!layers.empty() || !captions.empty() || inPoint >= 0.0) {
		QMessageBox::StandardButton answer = OBSMessageBox::question(this, QTStr("Spectra.ClipMaker.Title"),
									     QTStr("Spectra.ClipMaker.DiscardEdits"));
		if (answer != QMessageBox::Yes) {
			QSignalBlocker block(modeCombo);
			modeCombo->setCurrentIndex(modeCombo->findData((int)mode));
			return;
		}
	}

	mode = newMode;
	clipFile.clear();
	if (recorder) {
		folder = QDir::cleanPath(mode == Mode::Loop ? recorder->LoopDirectory() : recorder->ClipsDirectory());
	}
	timeline->SetLabels(QTStr(mode == Mode::Loop ? "Spectra.ClipMaker.VideoTrack" : "Spectra.ClipMaker.ClipTrack"),
			    QTStr(mode == Mode::Loop ? "Spectra.ClipMaker.NoRecordings"
						     : "Spectra.ClipMaker.PickClip"));
	browseButton->setToolTip(
		QTStr(mode == Mode::Loop ? "Spectra.ClipMaker.BrowseFolderTip" : "Spectra.ClipMaker.OpenFileTip"));
	sessionList->setToolTip(
		QTStr(mode == Mode::Loop ? "Spectra.ClipMaker.SessionsTip" : "Spectra.ClipMaker.ClipsTip"));
	statusLabel->setVisible(mode == Mode::Loop);
	ResetEditing();
	Rescan();
}

void SpectraClipMaker::Browse()
{
	if (mode == Mode::Clips) {
		QString path = QFileDialog::getOpenFileName(this, QTStr("Spectra.ClipMaker.OpenFileTip"), folder,
							    QStringLiteral("Video (*.mp4 *.mkv *.mov)"));
		if (!path.isEmpty()) {
			folder = QDir::cleanPath(QFileInfo(path).absolutePath());
			OpenClip(path);
		}
		return;
	}

	QString dir = QFileDialog::getExistingDirectory(this, QTStr("Browse"), folder);
	if (dir.isEmpty()) {
		return;
	}
	folder = QDir::cleanPath(dir);
	ResetEditing();
	Rescan();
}

void SpectraClipMaker::OpenClip(const QString &path)
{
	QString cleaned = QDir::cleanPath(path);
	if (cleaned == clipFile) {
		return;
	}
	if (!layers.empty() || !captions.empty()) {
		QMessageBox::StandardButton answer = OBSMessageBox::question(this, QTStr("Spectra.ClipMaker.Title"),
									     QTStr("Spectra.ClipMaker.DiscardEdits"));
		if (answer != QMessageBox::Yes) {
			return;
		}
	}
	ResetEditing();
	clipFile = cleaned;
	Rescan();
}

void SpectraClipMaker::Rescan()
{
	if (scanning) {
		rescanQueued = true;
		return;
	}

	folderLabel->setText(QDir::toNativeSeparators(folder));

	std::vector<ScannedFile> files;
	if (mode == Mode::Clips) {
		/* Only the clip being trimmed is read; the list shows the folder */
		QFileInfo fi(clipFile);
		if (!clipFile.isEmpty() && fi.exists()) {
			files.push_back({clipFile, fi.size(), fi.lastModified(), -1.0});
		}
	}

	const QString current = recorder ? QDir::cleanPath(recorder->CurrentSegment()) : QString();
	const QStringList loopFiles = mode == Mode::Loop ? QStringList{"*.mkv"} : QStringList();
	for (const QFileInfo &fi :
	     loopFiles.isEmpty() ? QFileInfoList() : QDir(folder).entryInfoList(loopFiles, QDir::Files, QDir::Name)) {
		QString path = QDir::cleanPath(fi.absoluteFilePath());
		/* The segment being written grows and has no index yet */
		if (!current.isEmpty() && path.compare(current, Qt::CaseInsensitive) == 0) {
			continue;
		}
		files.push_back({path, fi.size(), fi.lastModified(), -1.0});
	}

	scanning = true;
	rescanQueued = false;
	const int generation = ++scanGeneration;
	scanCancel = std::make_shared<std::atomic_bool>(false);
	scanLabel->setText(QTStr("Spectra.ClipMaker.Scanning").arg(0).arg(files.size()));

	QPointer<SpectraClipMaker> self(this);
	std::shared_ptr<std::atomic_bool> cancel = scanCancel;
	std::thread([self, cancel, generation, files]() mutable {
		for (size_t i = 0; i < files.size() && !*cancel; i++) {
			ScannedFile &f = files[i];
			{
				std::lock_guard lock(durationCacheMutex);
				auto it = durationCache.constFind(f.path);
				if (it != durationCache.constEnd() && it->size == f.size &&
				    it->modified == f.modified) {
					f.duration = it->duration;
					continue;
				}
			}
			f.duration = ClipExport::ProbeDuration(f.path.toStdString());
			{
				std::lock_guard lock(durationCacheMutex);
				durationCache.insert(f.path, {f.size, f.modified, f.duration});
			}
			size_t done = i + 1, total = files.size();
			QMetaObject::invokeMethod(
				qApp,
				[self, generation, done, total]() {
					if (self && self->scanGeneration == generation) {
						self->scanLabel->setText(
							QTStr("Spectra.ClipMaker.Scanning").arg(done).arg(total));
					}
				},
				Qt::QueuedConnection);
		}
		if (*cancel) {
			return;
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, generation, files]() {
				if (self) {
					self->ScanFinished(generation, files);
				}
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraClipMaker::ScanFinished(int generation, const std::vector<ScannedFile> &files)
{
	if (generation != scanGeneration) {
		return;
	}
	scanning = false;

	std::vector<Segment> updated;
	double offset = 0.0;
	for (const ScannedFile &f : files) {
		if (f.duration <= 0.0) {
			continue;
		}
		Segment s;
		s.path = f.path;
		s.start = offset;
		s.duration = f.duration;
		s.size = f.size;
		/* Segment (and clip) names carry the time their recording started */
		static const QRegularExpression stamp("\\d{4}-\\d{2}-\\d{2} \\d{2}-\\d{2}-\\d{2}");
		QRegularExpressionMatch match = stamp.match(QFileInfo(f.path).completeBaseName());
		s.recorded = match.hasMatch()
				     ? QDateTime::fromString(match.captured(), QStringLiteral("yyyy-MM-dd HH-mm-ss"))
				     : QDateTime();
		if (!s.recorded.isValid()) {
			s.recorded = f.modified.addMSecs(-(qint64)(f.duration * 1000.0));
		}
		if (updated.empty()) {
			s.sessionStart = true;
		} else {
			const Segment &prev = updated.back();
			qint64 prevEnd = (qint64)(prev.duration * 1000.0);
			s.sessionStart = prev.recorded.addMSecs(prevEnd).msecsTo(s.recorded) > SESSION_GAP_SEC * 1000.0;
		}
		offset += f.duration;
		updated.push_back(s);
	}

	/* Old segments leave from the front (disk quota) and new ones arrive at
	 * the back, so everything placed on the timeline shifts by however much
	 * the first surviving segment moved. */
	const bool firstScan = segments.empty();
	double shift = 0.0;
	bool anchored = firstScan;
	for (const Segment &old : segments) {
		auto it = std::find_if(updated.begin(), updated.end(),
				       [&](const Segment &s) { return s.path == old.path; });
		if (it != updated.end()) {
			shift = it->start - old.start;
			anchored = true;
			break;
		}
	}
	if (!anchored) {
		/* A different folder or everything was replaced */
		inPoint = outPoint = -1.0;
		playhead = 0.0;
	}

	QString loadedPaths[2];
	for (int p = 0; p < 2; p++) {
		int index = players[p].segment;
		if (index >= 0 && index < (int)segments.size()) {
			loadedPaths[p] = segments[index].path;
		}
	}
	segments = std::move(updated);

	const double duration = Duration();
	auto move = [&](double t) {
		return t < 0.0 ? t : std::clamp(t + shift, 0.0, duration);
	};
	playhead = std::clamp(playhead + shift, 0.0, duration);
	inPoint = move(inPoint);
	outPoint = move(outPoint);
	for (CensorLayer &l : layers) {
		l.layer.start = std::clamp(l.layer.start + shift, 0.0, duration);
		l.layer.end = std::clamp(l.layer.end + shift, 0.0, duration);
	}
	if (shift != 0.0 && !captions.empty()) {
		for (ClipCaptions::Cue &cue : captions) {
			cue.start = std::clamp(cue.start + shift, 0.0, duration);
			cue.end = std::clamp(cue.end + shift, 0.0, duration);
		}
		CaptionsChanged();
	}
	if (firstScan && mode == Mode::Clips && !segments.empty()) {
		/* Trimming: the handles start at the ends of the clip */
		playhead = 0.0;
		inPoint = 0.0;
		outPoint = duration;
	} else if (firstScan) {
		/* Start at the most recent session */
		for (const Segment &s : segments) {
			if (s.sessionStart) {
				playhead = s.start;
			}
		}
	}

	for (int p = 0; p < 2; p++) {
		players[p].segment = -1;
		for (size_t i = 0; i < segments.size(); i++) {
			if (segments[i].path == loadedPaths[p]) {
				players[p].segment = (int)i;
			}
		}
		if (players[p].segment < 0 && !loadedPaths[p].isEmpty()) {
			UnloadSegment(players[p]);
		}
	}

	QVector<ClipTimeline::Segment> timelineSegments;
	for (const Segment &s : segments) {
		timelineSegments.push_back({s.start, s.duration,
					    s.recorded.toString(QStringLiteral("ddd d MMM, HH:mm:ss")),
					    s.sessionStart});
	}
	timeline->SetSegments(timelineSegments);

	UpdateLibrary();
	LayersChanged();
	InOutChanged();
	if (Active().segment < 0 && !segments.empty()) {
		Seek(playhead);
	} else {
		PlayheadChanged();
	}

	if (rescanQueued) {
		Rescan();
	} else if (pendingMoment) {
		PlaceMoment();
	}
}

void SpectraClipMaker::ShowMoment(const QString &segment, double offset, double before, double after)
{
	const QString path = QDir::cleanPath(segment);
	const QString dir = QDir::cleanPath(QFileInfo(path).absolutePath());
	if (mode != Mode::Loop || dir.compare(folder, Qt::CaseInsensitive) != 0) {
		if (!layers.empty() || !captions.empty() || inPoint >= 0.0) {
			QMessageBox::StandardButton answer = OBSMessageBox::question(
				this, QTStr("Spectra.ClipMaker.Title"), QTStr("Spectra.ClipMaker.DiscardEdits"));
			if (answer != QMessageBox::Yes) {
				return;
			}
			layers.clear();
			captions.clear();
			CaptionsChanged();
			inPoint = outPoint = -1.0;
		}
		if (mode != Mode::Loop) {
			QSignalBlocker block(modeCombo);
			modeCombo->setCurrentIndex(modeCombo->findData((int)Mode::Loop));
			SetMode(Mode::Loop);
		}
		if (dir.compare(folder, Qt::CaseInsensitive) != 0) {
			folder = dir;
			ResetEditing();
		}
	}

	pendingMoment = Moment{path, offset, before, after};
	/* Placed when the scan finishes, so a segment finalized just now is in it */
	Rescan();
}

void SpectraClipMaker::PlaceMoment()
{
	Moment &m = *pendingMoment;
	const QString current = recorder ? QDir::cleanPath(recorder->CurrentSegment()) : QString();
	auto it = std::find_if(segments.begin(), segments.end(),
			       [&](const Segment &s) { return s.path.compare(m.segment, Qt::CaseInsensitive) == 0; });

	if (it == segments.end()) {
		if (!current.isEmpty() && current.compare(m.segment, Qt::CaseInsensitive) == 0) {
			/* Still being written: finish it now, or wait for it to end */
			if (!m.splitRequested) {
				m.splitRequested = true;
				main->SplitLoopRecording();
			}
			scanLabel->setText(QTStr("Spectra.ClipMaker.MomentWaiting"));
			return;
		}
		pendingMoment.reset();
		OBSMessageBox::warning(this, QTStr("Spectra.ClipMaker.Title"), QTStr("Spectra.ClipMaker.MomentGone"));
		return;
	}

	const double t = it->start + std::clamp(m.offset, 0.0, it->duration);
	if (t + m.after > Duration() && !current.isEmpty() && !m.splitRequested) {
		/* The Out point is in the segment being written */
		m.splitRequested = true;
		if (main->SplitLoopRecording()) {
			scanLabel->setText(QTStr("Spectra.ClipMaker.MomentWaiting"));
			return;
		}
	}

	const double before = m.before, after = m.after;
	pendingMoment.reset();
	SetPlaying(false);
	inPoint = std::max(0.0, t - before);
	outPoint = std::min(Duration(), t + after);
	InOutChanged();
	Seek(inPoint);
	if (outPoint > inPoint) {
		timeline->SetZoom(timeline->width() / ((outPoint - inPoint) * 3.0), t);
	}
	timeline->EnsureVisible(outPoint);
	timeline->EnsureVisible(inPoint);
}

void SpectraClipMaker::UpdateLibrary()
{
	sessionList->clear();

	if (mode == Mode::Clips) {
		QFileInfoList clips = QDir(folder).entryInfoList({"*.mp4", "*.mkv", "*.mov"}, QDir::Files, QDir::Time);
		for (const QFileInfo &fi : clips) {
			QString path = QDir::cleanPath(fi.absoluteFilePath());
			QString text = QStringLiteral("%1\n%2 · %3 MB")
					       .arg(fi.completeBaseName(),
						    fi.lastModified().toString(QStringLiteral("ddd d MMM, HH:mm")))
					       .arg(fi.size() / (1024 * 1024));
			QListWidgetItem *item = new QListWidgetItem(text, sessionList);
			item->setData(Qt::UserRole, path);
			if (path == clipFile) {
				sessionList->setCurrentItem(item);
			}
		}
		scanLabel->setText(clips.isEmpty() ? QTStr("Spectra.ClipMaker.NoClips")
						   : QTStr("Spectra.ClipMaker.ClipsHint"));
		return;
	}

	qint64 bytes = 0;
	for (const Segment &s : segments) {
		bytes += s.size;
	}

	for (size_t i = 0; i < segments.size(); i++) {
		if (!segments[i].sessionStart) {
			continue;
		}
		size_t last = i;
		while (last + 1 < segments.size() && !segments[last + 1].sessionStart) {
			last++;
		}
		const Segment &first = segments[i];
		const Segment &end = segments[last];
		double length = end.start + end.duration - first.start;
		QDateTime finished = end.recorded.addMSecs((qint64)(end.duration * 1000.0));

		QString text = QStringLiteral("%1 – %2\n%3")
				       .arg(first.recorded.toString(QStringLiteral("ddd d MMM, HH:mm")),
					    finished.toString(QStringLiteral("HH:mm")), FormatDuration(length));
		QListWidgetItem *item = new QListWidgetItem(text, sessionList);
		item->setData(Qt::UserRole, first.start);
	}

	if (segments.empty()) {
		scanLabel->setText(QTStr("Spectra.ClipMaker.Empty"));
	} else {
		scanLabel->setText(QTStr("Spectra.ClipMaker.Summary")
					   .arg(FormatDuration(Duration()))
					   .arg((double)bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1));
	}
	UpdateStatus();
}

void SpectraClipMaker::UpdateStatus()
{
	if (!recorder) {
		statusLabel->setText(QString());
		return;
	}

	QString text;
	if (recorder->Active()) {
		text = QTStr("Spectra.ClipMaker.Status.Recording").arg(std::max(recorder->SegmentSeconds() / 60, 1));
		const QString status = recorder->RecordingStatusText();
		if (!status.isEmpty()) {
			text += "\n\n" + status;
		}
	} else if (recorder->Armed()) {
		QStringList patterns = recorder->ProcessPatterns();
		text = patterns.isEmpty() ? QTStr("Spectra.ClipMaker.Status.NoGames")
					  : QTStr("Spectra.ClipMaker.Status.Armed").arg(patterns.join(", "));
	} else {
		text = QTStr("Spectra.ClipMaker.Status.Off");
	}

	if (!recorder->LastStartError().isEmpty()) {
		text += "\n\n" + QTStr("Spectra.ClipMaker.Status.StartFailed").arg(recorder->LastStartError());
	}

	QString configured = QDir::cleanPath(recorder->LoopDirectory());
	if (configured.compare(folder, Qt::CaseInsensitive) != 0) {
		text += "\n\n" +
			QTStr("Spectra.ClipMaker.Status.OtherFolder").arg(QDir::toNativeSeparators(configured));
	}
	statusLabel->setText(text);
}

/* ------------------------------------------------------------------------- */
/* Playback                                                                  */

double SpectraClipMaker::Duration() const
{
	return segments.empty() ? 0.0 : segments.back().start + segments.back().duration;
}

int SpectraClipMaker::SegmentAt(double t) const
{
	if (segments.empty()) {
		return -1;
	}
	auto it = std::upper_bound(segments.begin(), segments.end(), t,
				   [](double value, const Segment &s) { return value < s.start; });
	int index = (int)(it - segments.begin()) - 1;
	return std::clamp(index, 0, (int)segments.size() - 1);
}

QDateTime SpectraClipMaker::RecordedAt(double t) const
{
	int i = SegmentAt(t);
	if (i < 0) {
		return QDateTime();
	}
	return segments[i].recorded.addMSecs((qint64)std::llround((t - segments[i].start) * 1000.0));
}

void SpectraClipMaker::LoadSegment(Player &player, int index, qint64 localMs)
{
	if (!player.source || index < 0 || index >= (int)segments.size()) {
		return;
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "local_file", QT_TO_UTF8(segments[index].path));
	obs_source_update(player.source, settings);
	/* The file starts playing from the top once opened; keep that quiet
	 * until it has been moved to the right place. */
	obs_source_set_muted(player.source, true);
	player.segment = index;
	player.pendingSeekMs = std::max<qint64>(localMs, 0);
	player.loadRequested = QDateTime::currentDateTime();
}

void SpectraClipMaker::UnloadSegment(Player &player)
{
	if (player.source) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "local_file", "");
		obs_source_update(player.source, settings);
		obs_source_set_muted(player.source, true);
	}
	player.segment = -1;
	player.pendingSeekMs = -1;
}

void SpectraClipMaker::SwapToStandby()
{
	Player &old = Active();
	Player &next = Standby();
	if (old.source) {
		obs_source_media_play_pause(old.source, true);
		obs_source_set_muted(old.source, true);
	}
	activePlayer = 1 - activePlayer;
	/* A player still opening its file is started by Poll */
	if (next.source && next.pendingSeekMs < 0) {
		obs_source_set_muted(next.source, false);
		obs_source_media_play_pause(next.source, !playing);
	}
}

void SpectraClipMaker::Seek(double t)
{
	playhead = std::clamp(t, 0.0, Duration());
	int index = SegmentAt(playhead);
	if (index >= 0) {
		qint64 localMs = (qint64)std::llround((playhead - segments[index].start) * 1000.0);
		Player &active = Active();
		Player &standby = Standby();
		Player *target = nullptr;
		if (index == active.segment) {
			target = &active;
		} else if (index == standby.segment) {
			SwapToStandby();
			target = &standby;
		}

		if (!target) {
			LoadSegment(active, index, localMs);
		} else if (target->pendingSeekMs >= 0) {
			target->pendingSeekMs = localMs;
		} else if (target->source) {
			obs_source_media_set_time(target->source, localMs);
		}
	}
	PlayheadChanged();
}

void SpectraClipMaker::SetPlaying(bool play)
{
	if (segments.empty()) {
		play = false;
	}
	if (play && playhead >= Duration() - 0.05) {
		Seek(inPoint >= 0.0 ? inPoint : 0.0);
	}
	playing = play;
	playButton->setText(playing ? QStringLiteral("⏸") : QStringLiteral("▶"));

	Player &active = Active();
	if (!active.source || active.segment < 0) {
		if (playing) {
			Seek(playhead);
		}
		return;
	}
	if (active.pendingSeekMs >= 0) {
		return; /* Poll starts or pauses once the file is ready */
	}
	obs_media_state state = obs_source_media_get_state(active.source);
	bool finished = state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED;
	if (playing && (finished || SegmentAt(playhead) != active.segment)) {
		/* A file that played to its end has to be reopened */
		int index = SegmentAt(playhead);
		LoadSegment(active, index, (qint64)std::llround((playhead - segments[index].start) * 1000.0));
		return;
	}
	obs_source_media_play_pause(active.source, !playing);
}

void SpectraClipMaker::Step(double seconds)
{
	if (playing) {
		SetPlaying(false);
	}
	Seek(playhead + seconds);
}

void SpectraClipMaker::Poll()
{
	/* Finish loads once the file is open: park it at its position */
	for (int i = 0; i < 2; i++) {
		Player &player = players[i];
		if (!player.source || player.segment < 0 || player.pendingSeekMs < 0) {
			continue;
		}
		if (player.loadRequested.msecsTo(QDateTime::currentDateTime()) < LOAD_SETTLE_MS ||
		    obs_source_media_get_duration(player.source) <= 0) {
			continue;
		}
		bool isActive = i == activePlayer;
		obs_source_media_play_pause(player.source, !(isActive && playing));
		obs_source_media_set_time(player.source, player.pendingSeekMs);
		obs_source_set_muted(player.source, !isActive);
		player.pendingSeekMs = -1;
	}

	Player &active = Active();
	if (!playing || !active.source || active.segment < 0 || active.pendingSeekMs >= 0) {
		return;
	}

	const int next = active.segment + 1;
	const bool hasNext = next < (int)segments.size();
	obs_media_state state = obs_source_media_get_state(active.source);
	if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED) {
		if (hasNext) {
			/* Continue straight into the next segment */
			playhead = segments[next].start;
			if (Standby().segment == next) {
				SwapToStandby();
			} else {
				LoadSegment(active, next, 0);
			}
		} else {
			playhead = Duration();
			SetPlaying(false);
		}
		PlayheadChanged();
		return;
	}
	if (state != OBS_MEDIA_STATE_PLAYING) {
		return;
	}

	const Segment &seg = segments[active.segment];
	double local = obs_source_media_get_time(active.source) / 1000.0;
	/* The view follows the playhead while it's on screen; once the user
	 * has scrolled elsewhere it stays there */
	const bool follow = timeline->IsVisible(playhead);
	playhead = std::clamp(seg.start + local, seg.start, seg.start + seg.duration);
	if (hasNext && seg.duration - local < PRELOAD_SEC && Standby().segment != next) {
		LoadSegment(Standby(), next, 0);
	}
	if (follow) {
		timeline->EnsureVisible(playhead);
	}
	PlayheadChanged();
}

void SpectraClipMaker::PlayheadChanged()
{
	timeline->SetPlayhead(playhead);
	timeLabel->setText(QStringLiteral("%1 / %2").arg(ClipTimeline::FormatTime(playhead),
							 ClipTimeline::FormatTime(Duration(), false)));
	QDateTime recorded = RecordedAt(playhead);
	recordedLabel->setText(recorded.isValid()
				       ? QTStr("Spectra.ClipMaker.RecordedAt")
						 .arg(recorded.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
				       : QString());
	UpdateOverlay();
	UpdateCaptionPreview();
}

void SpectraClipMaker::UpdateZoomSlider()
{
	if (updatingZoom) {
		return;
	}
	const double lo = timeline->MinZoom(), hi = timeline->MaxZoom();
	int value = 0;
	if (hi > lo) {
		value = (int)std::lround(ZOOM_STEPS * std::log(timeline->Zoom() / lo) / std::log(hi / lo));
	}
	updatingZoom = true;
	zoomSlider->setValue(std::clamp(value, 0, ZOOM_STEPS));
	zoomSlider->setEnabled(hi > lo);
	updatingZoom = false;
}

/* ------------------------------------------------------------------------- */
/* In / Out                                                                  */

void SpectraClipMaker::SetIn(double t)
{
	inPoint = t;
	if (outPoint >= 0.0 && outPoint <= inPoint) {
		outPoint = -1.0;
	}
	InOutChanged();
}

void SpectraClipMaker::SetOut(double t)
{
	outPoint = t;
	if (inPoint >= 0.0 && inPoint >= outPoint) {
		inPoint = -1.0;
	}
	InOutChanged();
}

void SpectraClipMaker::InOutChanged()
{
	timeline->SetInOut(inPoint, outPoint);
	QString in = inPoint >= 0.0 ? ClipTimeline::FormatTime(inPoint) : QStringLiteral("--:--:--");
	QString out = outPoint >= 0.0 ? ClipTimeline::FormatTime(outPoint) : QStringLiteral("--:--:--");
	QString text = QTStr("Spectra.ClipMaker.Range").arg(in, out);
	if (inPoint >= 0.0 && outPoint > inPoint) {
		text += QStringLiteral("  (%1)").arg(ClipTimeline::FormatTime(outPoint - inPoint));
	}
	rangeLabel->setText(text);
	exportButton->setEnabled(inPoint >= 0.0 && outPoint > inPoint && !exportState);
	uploadButton->setEnabled(inPoint >= 0.0 && outPoint > inPoint && !exportState);
}

/* ------------------------------------------------------------------------- */
/* Layers                                                                    */

SpectraClipMaker::CensorLayer *SpectraClipMaker::Selected()
{
	for (CensorLayer &l : layers) {
		if (l.id == selectedLayer) {
			return &l;
		}
	}
	return nullptr;
}

void SpectraClipMaker::AddLayer(Shape shape)
{
	if (segments.empty()) {
		return;
	}

	int number = 1;
	for (const CensorLayer &l : layers) {
		if (l.layer.shape == shape) {
			number++;
		}
	}

	CensorLayer l;
	l.id = nextLayerId++;
	l.name = QTStr(shape == Shape::Rectangle ? "Spectra.ClipMaker.Rectangle" : "Spectra.ClipMaker.Ellipse") +
		 QStringLiteral(" %1").arg(number);
	l.layer.shape = shape;
	l.layer.fill = Fill::Solid;
	l.layer.x = 0.35;
	l.layer.y = 0.4;
	l.layer.w = 0.3;
	l.layer.h = 0.2;
	/* Cover the marked clip, or the next ten seconds */
	if (inPoint >= 0.0 && outPoint > inPoint) {
		l.layer.start = inPoint;
		l.layer.end = outPoint;
	} else {
		l.layer.start = playhead;
		l.layer.end = std::min(playhead + 10.0, Duration());
		if (l.layer.end - l.layer.start < 0.1) {
			l.layer.start = std::max(l.layer.end - 10.0, 0.0);
		}
	}
	layers.push_back(l);
	selectedLayer = l.id;
	LayersChanged();
}

void SpectraClipMaker::DeleteLayer()
{
	auto it = std::find_if(layers.begin(), layers.end(),
			       [this](const CensorLayer &l) { return l.id == selectedLayer; });
	if (it == layers.end()) {
		return;
	}
	layers.erase(it);
	selectedLayer = layers.empty() ? -1 : layers.back().id;
	LayersChanged();
}

void SpectraClipMaker::MoveLayer(int direction)
{
	/* Later layers draw on top; "up" moves toward the top */
	auto it = std::find_if(layers.begin(), layers.end(),
			       [this](const CensorLayer &l) { return l.id == selectedLayer; });
	if (it == layers.end()) {
		return;
	}
	size_t index = (size_t)(it - layers.begin());
	size_t target = direction > 0 ? index + 1 : index - 1;
	if (direction < 0 && index == 0) {
		return;
	}
	if (target >= layers.size()) {
		return;
	}
	std::swap(layers[index], layers[target]);
	LayersChanged();
}

void SpectraClipMaker::SelectLayer(int id)
{
	selectedLayer = id;
	UpdateLayerList();
	UpdateLayerProps();
	timeline->SetSelectedBar(id);
	UpdateOverlay();
}

void SpectraClipMaker::LayersChanged()
{
	QVector<ClipTimeline::Bar> bars;
	/* Top layer first, like a layer list in an editor */
	for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
		bars.push_back({it->id, it->name, BarColor(it->layer), it->layer.start, it->layer.end, it->visible});
	}
	timeline->SetBars(bars);
	timeline->SetSelectedBar(selectedLayer);
	UpdateLayerList();
	UpdateLayerProps();
	UpdateOverlay();
}

void SpectraClipMaker::UpdateLayerList()
{
	updatingUI = true;
	layerList->clear();
	for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
		QListWidgetItem *item = new QListWidgetItem(it->name, layerList);
		item->setData(Qt::UserRole, it->id);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(it->visible ? Qt::Checked : Qt::Unchecked);
		QPixmap swatch(12, 12);
		swatch.fill(BarColor(it->layer));
		item->setIcon(QIcon(swatch));
		if (it->id == selectedLayer) {
			layerList->setCurrentItem(item);
		}
	}
	updatingUI = false;
}

void SpectraClipMaker::UpdateLayerProps()
{
	CensorLayer *l = Selected();
	layerProps->setEnabled(l != nullptr);
	if (!l) {
		return;
	}

	updatingUI = true;
	const ClipRender::Layer &c = l->layer;
	if (nameEdit->text() != l->name) {
		nameEdit->setText(l->name);
	}
	shapeCombo->setCurrentIndex(shapeCombo->findData((int)c.shape));
	fillCombo->setCurrentIndex(fillCombo->findData((int)c.fill));
	colorButton->setEnabled(c.fill == Fill::Solid);
	colorButton->setStyleSheet(
		QStringLiteral("background-color: rgb(%1, %2, %3); min-height: 18px;").arg(c.r).arg(c.g).arg(c.b));
	strengthSlider->setEnabled(c.fill != Fill::Solid);
	strengthSlider->setValue(c.strength);

	const double duration = std::max(Duration(), 0.1);
	startEdit->setRange(0.0, duration);
	endEdit->setRange(0.0, duration);
	startEdit->setValue(c.start);
	endEdit->setValue(c.end);

	xEdit->setValue((int)std::lround(c.x * 100.0));
	yEdit->setValue((int)std::lround(c.y * 100.0));
	wEdit->setValue((int)std::lround(c.w * 100.0));
	hEdit->setValue((int)std::lround(c.h * 100.0));
	updatingUI = false;
}

void SpectraClipMaker::ApplyLayerProps()
{
	CensorLayer *l = Selected();
	if (updatingUI || !l) {
		return;
	}

	ClipRender::Layer &c = l->layer;
	l->name = nameEdit->text();
	c.shape = (Shape)shapeCombo->currentData().toInt();
	c.fill = (Fill)fillCombo->currentData().toInt();
	c.strength = strengthSlider->value();

	double start = startEdit->value(), end = endEdit->value();
	if (end - start < 0.1) {
		if (sender() == startEdit) {
			start = std::max(end - 0.1, 0.0);
		} else {
			end = start + 0.1;
		}
	}
	c.start = start;
	c.end = end;

	/* The spin boxes are whole percents; only take values the user changed
	 * so dragged positions keep their precision. */
	auto take = [](QSpinBox *box, double current) {
		return std::lround(current * 100.0) == box->value() ? current : box->value() / 100.0;
	};
	c.w = std::clamp(take(wEdit, c.w), MIN_SHAPE, 1.0);
	c.h = std::clamp(take(hEdit, c.h), MIN_SHAPE, 1.0);
	c.x = std::clamp(take(xEdit, c.x), 0.0, 1.0 - c.w);
	c.y = std::clamp(take(yEdit, c.y), 0.0, 1.0 - c.h);

	LayersChanged();
}

void SpectraClipMaker::UpdateOverlay()
{
	std::vector<ClipRender::Layer> shapes;
	int selected = -1;
	for (const CensorLayer &l : layers) {
		if (!l.visible || !l.layer.ActiveAt(playhead)) {
			continue;
		}
		if (l.id == selectedLayer) {
			selected = (int)shapes.size();
		}
		shapes.push_back(l.layer);
	}
	std::lock_guard lock(overlayMutex);
	overlay = std::move(shapes);
	overlaySelected = selected;
}

/* ------------------------------------------------------------------------- */
/* Preview drawing (graphics thread)                                         */

static void DrawRect(float x0, float y0, float x1, float y1)
{
	gs_render_start(true);
	gs_vertex2f(x0, y0);
	gs_vertex2f(x1, y0);
	gs_vertex2f(x0, y1);
	gs_vertex2f(x1, y0);
	gs_vertex2f(x1, y1);
	gs_vertex2f(x0, y1);
	gs_render_stop(GS_TRIS);
}

static void DrawShape(const ClipRender::Layer &l, float cx, float cy, bool outline)
{
	const float x0 = (float)l.x * cx, y0 = (float)l.y * cy;
	const float x1 = (float)(l.x + l.w) * cx, y1 = (float)(l.y + l.h) * cy;

	if (l.shape == Shape::Rectangle) {
		if (outline) {
			gs_render_start(true);
			gs_vertex2f(x0, y0);
			gs_vertex2f(x1, y0);
			gs_vertex2f(x1, y1);
			gs_vertex2f(x0, y1);
			gs_vertex2f(x0, y0);
			gs_render_stop(GS_LINESTRIP);
		} else {
			DrawRect(x0, y0, x1, y1);
		}
		return;
	}

	const int steps = 64;
	const float ex = (x0 + x1) / 2.0f, ey = (y0 + y1) / 2.0f;
	const float rx = (x1 - x0) / 2.0f, ry = (y1 - y0) / 2.0f;
	gs_render_start(true);
	for (int i = 0; i < steps; i++) {
		float a0 = (float)(2.0 * PI * i / steps), a1 = (float)(2.0 * PI * (i + 1) / steps);
		if (outline) {
			gs_vertex2f(ex + rx * std::cos(a0), ey + ry * std::sin(a0));
		} else {
			gs_vertex2f(ex, ey);
			gs_vertex2f(ex + rx * std::cos(a0), ey + ry * std::sin(a0));
			gs_vertex2f(ex + rx * std::cos(a1), ey + ry * std::sin(a1));
		}
	}
	if (outline) {
		gs_vertex2f(ex + rx, ey);
		gs_render_stop(GS_LINESTRIP);
	} else {
		gs_render_stop(GS_TRIS);
	}
}

void SpectraClipMaker::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	SpectraClipMaker *window = static_cast<SpectraClipMaker *>(data);
	obs_source_t *src = window->ActiveSource();
	if (!src) {
		return;
	}

	uint32_t sourceCX = obs_source_get_width(src);
	uint32_t sourceCY = obs_source_get_height(src);
	if (!sourceCX || !sourceCY) {
		return;
	}

	int x, y;
	float scale;
	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);
	int newCX = int(scale * float(sourceCX));
	int newCY = int(scale * float(sourceCY));

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	obs_source_video_render(src);

	gs_set_linear_srgb(previous);

	std::vector<ClipRender::Layer> shapes;
	int selected;
	{
		std::lock_guard lock(window->overlayMutex);
		shapes = window->overlay;
		selected = window->overlaySelected;
	}

	if (!shapes.empty()) {
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
		gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		const float w = float(sourceCX), h = float(sourceCY);
		struct vec4 c;
		for (size_t i = 0; i < shapes.size(); i++) {
			const ClipRender::Layer &l = shapes[i];
			/* Solid shapes look as exported. Pixelate and blur are
			 * applied on export; show where they go. */
			if (l.fill == Fill::Solid) {
				vec4_set(&c, l.r / 255.0f, l.g / 255.0f, l.b / 255.0f, 1.0f);
			} else if (l.fill == Fill::Pixelate) {
				vec4_set(&c, 0.55f, 0.4f, 0.85f, 0.55f);
			} else {
				vec4_set(&c, 0.35f, 0.6f, 0.9f, 0.55f);
			}
			gs_effect_set_vec4(color, &c);
			DrawShape(l, w, h, false);

			bool isSelected = (int)i == selected;
			vec4_set(&c, isSelected ? 1.0f : 1.0f, isSelected ? 0.75f : 1.0f, isSelected ? 0.15f : 1.0f,
				 isSelected ? 1.0f : 0.5f);
			gs_effect_set_vec4(color, &c);
			DrawShape(l, w, h, true);
		}

		if (selected >= 0) {
			const ClipRender::Layer &l = shapes[selected];
			const float hs = 5.0f / scale;
			const float xs[3] = {(float)l.x * w, (float)(l.x + l.w / 2) * w, (float)(l.x + l.w) * w};
			const float ys[3] = {(float)l.y * h, (float)(l.y + l.h / 2) * h, (float)(l.y + l.h) * h};
			for (int iy = 0; iy < 3; iy++) {
				for (int ix = 0; ix < 3; ix++) {
					if (ix == 1 && iy == 1) {
						continue;
					}
					vec4_set(&c, 0.0f, 0.0f, 0.0f, 1.0f);
					gs_effect_set_vec4(color, &c);
					DrawRect(xs[ix] - hs - 1.0f / scale, ys[iy] - hs - 1.0f / scale,
						 xs[ix] + hs + 1.0f / scale, ys[iy] + hs + 1.0f / scale);
					vec4_set(&c, 1.0f, 0.75f, 0.15f, 1.0f);
					gs_effect_set_vec4(color, &c);
					DrawRect(xs[ix] - hs, ys[iy] - hs, xs[ix] + hs, ys[iy] + hs);
				}
			}
		}

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
		gs_blend_state_pop();
	}

	/* The caption at the playhead, as it will be burned in */
	QImage caption;
	bool captionChanged = false;
	{
		std::lock_guard lock(window->overlayMutex);
		if (window->captionImageChanged) {
			caption = window->captionImage;
			window->captionImageChanged = false;
			captionChanged = true;
		}
	}
	if (captionChanged) {
		if (window->captionTexture) {
			gs_texture_destroy(window->captionTexture);
			window->captionTexture = nullptr;
		}
		if (!caption.isNull()) {
			const QImage argb = caption.convertToFormat(QImage::Format_ARGB32);
			const uint8_t *bits = argb.constBits();
			window->captionTexture = gs_texture_create(argb.width(), argb.height(), GS_BGRA, 1, &bits, 0);
		}
	}
	if (window->captionTexture) {
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), window->captionTexture);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(window->captionTexture, 0, sourceCX, sourceCY);
		}
		gs_blend_state_pop();
	}

	gs_projection_pop();
	gs_viewport_pop();
}

/* ------------------------------------------------------------------------- */
/* Preview interaction                                                       */

bool SpectraClipMaker::MapToFrame(const QPointF &widgetPos, QPointF &norm, double &handleSize) const
{
	obs_source_t *source = ActiveSource();
	if (!source) {
		return false;
	}
	uint32_t sourceCX = obs_source_get_width(source);
	uint32_t sourceCY = obs_source_get_height(source);
	if (!sourceCX || !sourceCY) {
		return false;
	}

	const double ratio = preview->devicePixelRatioF();
	QSize size = GetPixelSize(preview);
	int x, y;
	float scale;
	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x, y, scale);

	norm.setX((widgetPos.x() * ratio - x) / scale / sourceCX);
	norm.setY((widgetPos.y() * ratio - y) / scale / sourceCY);
	/* Handles are ~6 screen pixels from their center */
	handleSize = 7.0 * ratio / scale / sourceCX;
	return true;
}

SpectraClipMaker::Handle SpectraClipMaker::HandleAt(const ClipRender::Layer &l, const QPointF &p,
						    double handleSize) const
{
	uint32_t sourceCX = std::max(obs_source_get_width(ActiveSource()), 1u);
	uint32_t sourceCY = std::max(obs_source_get_height(ActiveSource()), 1u);
	/* Same size on screen in both directions */
	const double hx = handleSize, hy = handleSize * sourceCX / sourceCY;

	const double left = l.x, right = l.x + l.w, top = l.y, bottom = l.y + l.h;
	const double midX = (left + right) / 2.0, midY = (top + bottom) / 2.0;
	auto isNear = [&](double px, double py) {
		return std::abs(p.x() - px) <= hx && std::abs(p.y() - py) <= hy;
	};

	if (isNear(left, top)) {
		return Handle::NW;
	}
	if (isNear(right, top)) {
		return Handle::NE;
	}
	if (isNear(left, bottom)) {
		return Handle::SW;
	}
	if (isNear(right, bottom)) {
		return Handle::SE;
	}
	if (isNear(midX, top)) {
		return Handle::N;
	}
	if (isNear(midX, bottom)) {
		return Handle::S;
	}
	if (isNear(left, midY)) {
		return Handle::W;
	}
	if (isNear(right, midY)) {
		return Handle::E;
	}

	if (p.x() < left || p.x() > right || p.y() < top || p.y() > bottom) {
		return Handle::None;
	}
	if (l.shape == Shape::Ellipse) {
		double dx = (p.x() - midX) / (l.w / 2.0), dy = (p.y() - midY) / (l.h / 2.0);
		if (dx * dx + dy * dy > 1.0) {
			return Handle::None;
		}
	}
	return Handle::Move;
}

static Qt::CursorShape CursorFor(int handle)
{
	switch (handle) {
	case 1:
		return Qt::SizeAllCursor;
	case 2:
	case 3:
		return Qt::SizeVerCursor;
	case 4:
	case 5:
		return Qt::SizeHorCursor;
	case 6:
	case 9:
		return Qt::SizeBDiagCursor;
	case 7:
	case 8:
		return Qt::SizeFDiagCursor;
	default:
		return Qt::ArrowCursor;
	}
}

void SpectraClipMaker::PreviewMousePress(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		return;
	}
	QPointF p;
	double hs;
	if (!MapToFrame(event->position(), p, hs)) {
		return;
	}

	/* The selected shape's handles first, then the topmost shape hit */
	CensorLayer *sel = Selected();
	if (sel && sel->visible && sel->layer.ActiveAt(playhead)) {
		Handle h = HandleAt(sel->layer, p, hs);
		if (h != Handle::None) {
			dragHandle = h;
			dragOrigin = p;
			dragLayer = sel->layer;
			return;
		}
	}
	for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
		if (!it->visible || !it->layer.ActiveAt(playhead)) {
			continue;
		}
		if (HandleAt(it->layer, p, 0.0) == Handle::Move) {
			SelectLayer(it->id);
			dragHandle = Handle::Move;
			dragOrigin = p;
			dragLayer = it->layer;
			return;
		}
	}
	SelectLayer(-1);
}

void SpectraClipMaker::PreviewMouseMove(QMouseEvent *event)
{
	QPointF p;
	double hs;
	if (!MapToFrame(event->position(), p, hs)) {
		return;
	}

	CensorLayer *sel = Selected();
	if (dragHandle == Handle::None || !sel) {
		Handle hover = Handle::None;
		if (sel && sel->visible && sel->layer.ActiveAt(playhead)) {
			hover = HandleAt(sel->layer, p, hs);
		}
		preview->setCursor(CursorFor((int)hover));
		return;
	}

	const double dx = p.x() - dragOrigin.x(), dy = p.y() - dragOrigin.y();
	const ClipRender::Layer &o = dragLayer;
	double left = o.x, top = o.y, right = o.x + o.w, bottom = o.y + o.h;

	switch (dragHandle) {
	case Handle::Move:
		left = std::clamp(o.x + dx, 0.0, 1.0 - o.w);
		top = std::clamp(o.y + dy, 0.0, 1.0 - o.h);
		right = left + o.w;
		bottom = top + o.h;
		break;
	case Handle::N:
	case Handle::NE:
	case Handle::NW:
		top = std::clamp(o.y + dy, 0.0, bottom - MIN_SHAPE);
		break;
	case Handle::S:
	case Handle::SE:
	case Handle::SW:
		bottom = std::clamp(o.y + o.h + dy, top + MIN_SHAPE, 1.0);
		break;
	default:
		break;
	}
	switch (dragHandle) {
	case Handle::W:
	case Handle::NW:
	case Handle::SW:
		left = std::clamp(o.x + dx, 0.0, right - MIN_SHAPE);
		break;
	case Handle::E:
	case Handle::NE:
	case Handle::SE:
		right = std::clamp(o.x + o.w + dx, left + MIN_SHAPE, 1.0);
		break;
	default:
		break;
	}

	sel->layer.x = left;
	sel->layer.y = top;
	sel->layer.w = right - left;
	sel->layer.h = bottom - top;
	UpdateLayerProps();
	UpdateOverlay();
}

void SpectraClipMaker::PreviewMouseRelease(QMouseEvent *)
{
	dragHandle = Handle::None;
}

bool SpectraClipMaker::eventFilter(QObject *object, QEvent *event)
{
	if (object == preview) {
		switch (event->type()) {
		case QEvent::MouseButtonPress:
			PreviewMousePress(static_cast<QMouseEvent *>(event));
			return true;
		case QEvent::MouseMove:
			PreviewMouseMove(static_cast<QMouseEvent *>(event));
			return true;
		case QEvent::MouseButtonRelease:
			PreviewMouseRelease(static_cast<QMouseEvent *>(event));
			return true;
		default:
			break;
		}
	}
	return QDialog::eventFilter(object, event);
}

/* ------------------------------------------------------------------------- */
/* Export                                                                    */

/* ------------------------------------------------------------------------- */
/* Captions                                                                  */

QSize SpectraClipMaker::VideoSize() const
{
	obs_source_t *src = ActiveSource();
	const uint32_t cx = src ? obs_source_get_width(src) : 0;
	const uint32_t cy = src ? obs_source_get_height(src) : 0;
	return cx && cy ? QSize((int)cx, (int)cy) : QSize(1920, 1080);
}

void SpectraClipMaker::Transcribe()
{
	if (exportState || segments.empty()) {
		return;
	}
	if (inPoint < 0.0 || outPoint <= inPoint) {
		OBSMessageBox::information(this, QTStr("Spectra.ClipMaker.Title"),
					   QTStr("Spectra.ClipMaker.NeedRange"));
		return;
	}
	ClipCaptions::Settings settings;
	QString error;
	if (!ClipCaptions::LoadSettings(main->Config(), settings, error)) {
		OBSMessageBox::warning(this, QTStr("Spectra.ClipMaker.Title"), error);
		return;
	}
	if (!captions.empty() &&
	    OBSMessageBox::question(this, QTStr("Spectra.ClipMaker.Title"),
				    QTStr("Spectra.ClipMaker.Captions.Replace")) != QMessageBox::Yes) {
		return;
	}
	if (playing) {
		SetPlaying(false);
	}

	const int first = SegmentAt(inPoint);
	const int last = SegmentAt(std::max(outPoint - 0.001, inPoint));
	std::vector<ClipCaptions::Source> sources;
	QStringList paths;
	for (int i = first; i <= last; i++) {
		const Segment &s = segments[i];
		ClipCaptions::Source source;
		source.path = s.path;
		source.from = std::max(inPoint - s.start, 0.0);
		source.to = std::min(outPoint - s.start, s.duration);
		source.at = s.start + source.from;
		if (source.to > source.from) {
			sources.push_back(source);
			paths << s.path;
		}
	}
	blog(LOG_INFO, "[Spectra] Clip maker: transcribing %.2f s from %d segment(s)", outPoint - inPoint,
	     (int)sources.size());

	if (recorder) {
		recorder->Lock(paths);
	}
	std::shared_ptr<ExportState> state = StartJob(QTStr("Spectra.ClipMaker.Captions.Transcribing"));

	QPointer<SpectraClipMaker> self(this);
	QPointer<LoopRecorder> rec = recorder;
	std::thread([self, rec, state, sources, settings, paths]() {
		std::vector<ClipCaptions::Cue> cues;
		QString error;
		const bool ok = ClipCaptions::Transcribe(
			sources, settings, cues,
			[state](float p) {
				state->progress = p;
				return !state->cancel;
			},
			error);
		QMetaObject::invokeMethod(
			qApp,
			[self, rec, paths, ok, cues, error]() {
				if (rec) {
					rec->Unlock(paths);
				}
				if (self) {
					self->TranscribeFinished(ok, cues, error);
				}
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraClipMaker::TranscribeFinished(bool ok, const std::vector<ClipCaptions::Cue> &cues, const QString &error)
{
	const bool cancelled = EndJob();
	if (!ok) {
		if (!cancelled) {
			blog(LOG_WARNING, "[Spectra] Clip maker: transcription failed: %s", QT_TO_UTF8(error));
			OBSMessageBox::warning(this, QTStr("Spectra.ClipMaker.Title"),
					       QTStr("Spectra.ClipMaker.Captions.Failed").arg(error));
		}
		return;
	}
	blog(LOG_INFO, "[Spectra] Clip maker: %zu caption(s)", cues.size());
	captions = cues;
	CaptionsChanged();
	captionInfo->setText(cues.empty() ? QTStr("Spectra.ClipMaker.Captions.None")
					  : QTStr("Spectra.ClipMaker.Captions.Count").arg(cues.size()));
}

int SpectraClipMaker::SelectedCaption() const
{
	const int row = captionTable->currentRow();
	return row >= 0 && row < (int)captions.size() ? row : -1;
}

void SpectraClipMaker::CaptionsChanged()
{
	UpdateCaptionTable();
	UpdateCaptionProps();
	shownCaption = -2;
	UpdateCaptionPreview();
}

void SpectraClipMaker::UpdateCaptionTable()
{
	updatingUI = true;
	const int current = captionTable->currentRow();
	captionTable->setRowCount((int)captions.size());
	for (int row = 0; row < (int)captions.size(); row++) {
		const ClipCaptions::Cue &cue = captions[row];
		auto readOnly = [](const QString &text) {
			QTableWidgetItem *item = new QTableWidgetItem(text);
			item->setFlags(item->flags() & ~Qt::ItemIsEditable);
			return item;
		};
		captionTable->setItem(row, kCaptionTime, readOnly(ClipTimeline::FormatTime(cue.start)));
		captionTable->setItem(row, kCaptionWho, readOnly(ClipCaptions::SpeakerName(cue.speaker)));
		captionTable->setItem(row, kCaptionText, new QTableWidgetItem(cue.text));
	}
	if (current >= 0 && current < (int)captions.size()) {
		captionTable->setCurrentCell(current, kCaptionText);
	}
	captionTable->resizeRowsToContents();
	updatingUI = false;
}

void SpectraClipMaker::UpdateCaptionProps()
{
	updatingUI = true;
	const int row = SelectedCaption();
	captionStart->setEnabled(row >= 0);
	captionEnd->setEnabled(row >= 0);
	if (row >= 0) {
		captionStart->setValue(captions[row].start);
		captionEnd->setValue(captions[row].end);
	}
	updatingUI = false;
}

void SpectraClipMaker::UpdateCaptionPreview()
{
	int active = -1;
	for (int i = 0; i < (int)captions.size(); i++) {
		if (playhead >= captions[i].start && playhead < captions[i].end) {
			active = i;
		}
	}
	if (active == shownCaption) {
		return;
	}
	shownCaption = active;

	QImage image;
	if (active >= 0) {
		const QSize size = VideoSize();
		image = ClipCaptions::Render(ClipCaptions::Display(captions[active], speakerCheck->isChecked()),
					     size.width(), size.height());
	}
	std::lock_guard lock(overlayMutex);
	captionImage = image;
	captionImageChanged = true;
}

void SpectraClipMaker::FillQualityCombo(QComboBox *combo)
{
	combo->addItem(QTStr("Spectra.ClipMaker.Quality.Original"), QStringLiteral("Original"));
	combo->addItem(QTStr("Spectra.ClipMaker.Quality.High"), QStringLiteral("High"));
	combo->addItem(QTStr("Spectra.ClipMaker.Quality.Medium"), QStringLiteral("Medium"));
	combo->addItem(QTStr("Spectra.ClipMaker.Quality.Small"), QStringLiteral("Small"));
	combo->addItem(QTStr("Spectra.ClipMaker.Quality.YouTube"), QStringLiteral("YouTube"));
}

bool SpectraClipMaker::QualityFromKey(const QString &key, ClipRender::Quality &quality)
{
	if (key == QStringLiteral("High")) {
		quality = ClipRender::Quality::High;
	} else if (key == QStringLiteral("Medium")) {
		quality = ClipRender::Quality::Medium;
	} else if (key == QStringLiteral("Small")) {
		quality = ClipRender::Quality::Small;
	} else if (key == QStringLiteral("YouTube")) {
		quality = ClipRender::Quality::YouTube1080p;
	} else {
		return false;
	}
	return true;
}

void SpectraClipMaker::SaveExportSettings()
{
	config_t *config = main->Config();
	config_set_string(config, "SpectraClipMaker", "ExportQuality",
			  QT_TO_UTF8(qualityCombo->currentData().toString()));
	config_set_bool(config, "SpectraClipMaker", "FrameAccurate", reencodeCheck->isChecked());
	config_save_safe(config, "tmp", nullptr);
}

bool SpectraClipMaker::CheckExportable()
{
	if (exportState || segments.empty()) {
		return false;
	}
	if (inPoint < 0.0 || outPoint <= inPoint) {
		OBSMessageBox::information(this, QTStr("Spectra.ClipMaker.Title"),
					   QTStr("Spectra.ClipMaker.NeedRange"));
		return false;
	}
	return true;
}

QString SpectraClipMaker::DefaultExportName(QString &dir) const
{
	dir = recorder ? recorder->ClipsDirectory() : folder;
	QString name = QStringLiteral("Clip %1").arg(RecordedAt(inPoint).toString("yyyy-MM-dd HH-mm-ss"));
	if (mode == Mode::Clips) {
		dir = QFileInfo(clipFile).absolutePath();
		name = QTStr("Spectra.ClipMaker.TrimmedName").arg(QFileInfo(clipFile).completeBaseName());
	}
	return name;
}

void SpectraClipMaker::Export()
{
	if (!CheckExportable()) {
		return;
	}
	if (playing) {
		SetPlaying(false);
	}
	QString clipsDir;
	const QString name = DefaultExportName(clipsDir) + QStringLiteral(".mp4");
	QDir().mkpath(clipsDir);
	QString path = QFileDialog::getSaveFileName(this, QTStr("Spectra.ClipMaker.Export"),
						    QDir(clipsDir).filePath(name),
						    QStringLiteral("MP4 (*.mp4);;Matroska (*.mkv)"));
	if (path.isEmpty()) {
		return;
	}
	if (QFileInfo(path).suffix().isEmpty()) {
		path += QStringLiteral(".mp4");
	}
	StartExport(path, qualityCombo->currentData().toString(), reencodeCheck->isChecked());
}

void SpectraClipMaker::UploadToYouTube()
{
	if (!CheckExportable()) {
		return;
	}
	if (playing) {
		SetPlaying(false);
	}
	if (!YouTubeUpload::Available(main->Config())) {
		OBSMessageBox::information(this, QTStr("Spectra.YouTube.Title"), QTStr("Spectra.YouTube.Unavailable"));
		return;
	}
	QString clipsDir;
	const QString name = DefaultExportName(clipsDir);
	SpectraYouTubeUpload dialog(main->Config(), name, this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}
	pendingUpload = dialog.Video();

	/* The clip is kept in the clips folder as well */
	QDir().mkpath(clipsDir);
	QString path = QDir(clipsDir).filePath(name + QStringLiteral(".mp4"));
	for (int i = 2; QFileInfo::exists(path); i++) {
		path = QDir(clipsDir).filePath(QStringLiteral("%1 (%2).mp4").arg(name).arg(i));
	}
	StartExport(path, dialog.QualityKey(), false);
}

void SpectraClipMaker::StartExport(const QString &path, const QString &qualityKey, bool frameAccurate)
{
	if (exportState || segments.empty() || inPoint < 0.0 || outPoint <= inPoint) {
		pendingUpload.reset();
		return;
	}

	const int first = SegmentAt(inPoint);
	const int last = SegmentAt(std::max(outPoint - 0.001, inPoint));
	const double base = segments[first].start;

	QStringList paths;
	std::vector<std::string> inputs;
	for (int i = first; i <= last; i++) {
		paths << segments[i].path;
		inputs.push_back(segments[i].path.toStdString());
	}
	/* The output can't replace a file it is being made from */
	for (const QString &input : paths) {
		if (QFileInfo(input) == QFileInfo(path)) {
			pendingUpload.reset();
			OBSMessageBox::warning(this, QTStr("Spectra.ClipMaker.Title"),
					       QTStr("Spectra.ClipMaker.SameFile"));
			return;
		}
	}

	std::vector<ClipRender::Layer> burn;
	for (const CensorLayer &l : layers) {
		if (l.visible && l.layer.end > inPoint && l.layer.start < outPoint) {
			ClipRender::Layer shifted = l.layer;
			shifted.start -= base;
			shifted.end -= base;
			burn.push_back(shifted);
		}
	}

	std::vector<ClipCaptions::Cue> clipCaptions;
	for (const ClipCaptions::Cue &cue : captions) {
		if (cue.end > inPoint && cue.start < outPoint && !cue.text.trimmed().isEmpty()) {
			clipCaptions.push_back(cue);
		}
	}
	const bool showSpeaker = speakerCheck->isChecked();
	const bool saveSrt = !clipCaptions.empty() && srtCheck->isChecked();
	std::vector<ClipRender::Overlay> overlays;
	if (burnCaptionsCheck->isChecked()) {
		const QSize size = VideoSize();
		for (const ClipCaptions::Cue &cue : clipCaptions) {
			overlays.push_back(
				ClipCaptions::ToOverlay(ClipCaptions::Render(ClipCaptions::Display(cue, showSpeaker),
									     size.width(), size.height()),
							cue.start - base, cue.end - base));
		}
	}

	/* Captions are timed from the In point, which only a re-encode starts on */
	ClipRender::Quality quality = ClipRender::Quality::High;
	const bool render = QualityFromKey(qualityKey, quality) || frameAccurate || !burn.empty() ||
			    !clipCaptions.empty();

	const double start = inPoint - base, end = outPoint - base;
	blog(LOG_INFO,
	     "[Spectra] Clip maker: exporting %.2f s from %d segment(s) to '%s' (%s, %zu censor layer(s), %zu caption(s)%s%s)",
	     outPoint - inPoint, (int)inputs.size(), QT_TO_UTF8(path), render ? QT_TO_UTF8(qualityKey) : "lossless",
	     burn.size(), clipCaptions.size(), overlays.empty() ? "" : " burned in",
	     pendingUpload ? ", for YouTube" : "");

	if (recorder) {
		recorder->Lock(paths);
	}

	std::shared_ptr<ExportState> state = StartJob(QTStr("Spectra.ClipMaker.Exporting"));
	const double clipStart = inPoint, clipLength = outPoint - inPoint;

	QPointer<SpectraClipMaker> self(this);
	QPointer<LoopRecorder> rec = recorder;
	std::thread([self, rec, state, inputs, paths, start, end, burn, overlays, render, quality, path, saveSrt,
		     clipCaptions, clipStart, clipLength, showSpeaker]() {
		auto progress = [state](float p) {
			state->progress = p;
			return !state->cancel;
		};

		std::string error;
		QString outPath = path;
		auto run = [&](const QString &target) {
			error.clear();
			return render ? ClipRender::Export(inputs, start, end, target.toStdString(), burn, error,
							   progress, overlays, quality)
				      : ClipExport::Export(inputs, start, end, target.toStdString(), error, progress);
		};
		bool ok = run(outPath);
		/* MP4 can't carry every audio codec; Matroska can */
		if (!ok && !state->cancel && error.find("not supported by the output container") != std::string::npos &&
		    QFileInfo(outPath).suffix().compare("mkv", Qt::CaseInsensitive) != 0) {
			outPath = QFileInfo(outPath).dir().filePath(QFileInfo(outPath).completeBaseName() + ".mkv");
			ok = run(outPath);
		}

		QString qerror = QString::fromStdString(error);
		if (ok && saveSrt) {
			/* Next to the clip with the same name, so players pick it up */
			const QString srt =
				QFileInfo(outPath).dir().filePath(QFileInfo(outPath).completeBaseName() + ".srt");
			QString srtError;
			if (!ClipCaptions::WriteSrt(srt, clipCaptions, clipStart, clipLength, showSpeaker, srtError)) {
				blog(LOG_WARNING, "[Spectra] Clip maker: could not save '%s': %s", QT_TO_UTF8(srt),
				     QT_TO_UTF8(srtError));
			}
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, rec, paths, ok, outPath, qerror]() {
				if (rec) {
					rec->Unlock(paths);
				}
				if (ok) {
					blog(LOG_INFO, "[Spectra] Clip maker: saved '%s'", QT_TO_UTF8(outPath));
				} else {
					blog(LOG_WARNING, "[Spectra] Clip maker: export failed: %s",
					     QT_TO_UTF8(qerror));
				}
				if (self) {
					self->ExportFinished(ok, outPath, qerror);
				}
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraClipMaker::StartUpload(const QString &path)
{
	YouTubeUpload::Session session;
	if (!pendingUpload || !YouTubeUpload::LoadSession(main->Config(), session)) {
		UploadFinished(false, path, QString(), QTStr("Spectra.YouTube.NotSignedIn"));
		return;
	}
	const YouTubeUpload::Video video = *pendingUpload;
	std::shared_ptr<ExportState> state = StartJob(QTStr("Spectra.YouTube.Uploading"));

	QPointer<SpectraClipMaker> self(this);
	std::thread([self, state, session, path, video]() mutable {
		auto progress = [state](float p) {
			state->progress = p;
			return !state->cancel;
		};
		QString videoId, error;
		const bool ok = YouTubeUpload::Upload(session, path, video, videoId, error, progress);
		QMetaObject::invokeMethod(
			qApp,
			[self, session, ok, path, videoId, error]() {
				if (!self) {
					return;
				}
				/* Keep a refreshed access token */
				YouTubeUpload::SaveSession(self->main->Config(), session);
				self->UploadFinished(ok, path, videoId, error);
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraClipMaker::UploadFinished(bool ok, const QString &path, const QString &videoId, const QString &error)
{
	const bool cancelled = EndJob();
	pendingUpload.reset();
	const QString shownPath = QDir::toNativeSeparators(path);

	if (ok) {
		const QString url = YouTubeUpload::VideoUrl(videoId);
		QMessageBox box(QMessageBox::Information, QTStr("Spectra.YouTube.Title"),
				QTStr("Spectra.YouTube.Done").arg(url, shownPath), QMessageBox::Ok, this);
		QPushButton *open = box.addButton(QTStr("Spectra.YouTube.OpenVideo"), QMessageBox::ActionRole);
		QPushButton *copy = box.addButton(QTStr("Spectra.YouTube.CopyLink"), QMessageBox::ActionRole);
		box.exec();
		if (box.clickedButton() == open) {
			QDesktopServices::openUrl(QUrl(url));
		} else if (box.clickedButton() == copy) {
			QApplication::clipboard()->setText(url);
		}
	} else if (!cancelled) {
		blog(LOG_WARNING, "[Spectra] YouTube: upload failed: %s", QT_TO_UTF8(error));
		OBSMessageBox::warning(this, QTStr("Spectra.YouTube.Title"),
				       QTStr("Spectra.YouTube.Failed").arg(error, shownPath));
	}
}

std::shared_ptr<SpectraClipMaker::ExportState> SpectraClipMaker::StartJob(const QString &label)
{
	exportState = std::make_shared<ExportState>();
	exportButton->setEnabled(false);
	uploadButton->setEnabled(false);
	transcribeButton->setEnabled(false);

	progressDialog = new QProgressDialog(label, QTStr("Cancel"), 0, 1000, this);
	progressDialog->setWindowTitle(QTStr("Spectra.ClipMaker.Title"));
	progressDialog->setWindowModality(Qt::WindowModal);
	progressDialog->setMinimumDuration(0);
	progressDialog->setAutoClose(false);
	progressDialog->setAutoReset(false);
	progressDialog->setValue(0);
	std::shared_ptr<ExportState> state = exportState;
	connect(progressDialog, &QProgressDialog::canceled, this, [state]() { state->cancel = true; });

	progressTimer.setInterval(100);
	disconnect(&progressTimer, nullptr, nullptr, nullptr);
	connect(&progressTimer, &QTimer::timeout, this, [this]() {
		if (exportState && progressDialog) {
			progressDialog->setValue((int)(exportState->progress * 1000.0f));
		}
	});
	progressTimer.start();
	return state;
}

bool SpectraClipMaker::EndJob()
{
	const bool cancelled = exportState && exportState->cancel;
	progressTimer.stop();
	exportState.reset();
	if (progressDialog) {
		progressDialog->close();
		progressDialog->deleteLater();
	}
	transcribeButton->setEnabled(true);
	InOutChanged();
	return cancelled;
}

void SpectraClipMaker::ExportFinished(bool ok, const QString &path, const QString &error)
{
	const bool cancelled = EndJob();

	if (ok && pendingUpload) {
		StartUpload(path);
		return;
	}
	pendingUpload.reset();
	if (ok) {
		QMessageBox box(QMessageBox::Information, QTStr("Spectra.ClipMaker.Title"),
				QTStr("Spectra.ClipMaker.Saved").arg(QDir::toNativeSeparators(path)), QMessageBox::Ok,
				this);
		QPushButton *show = box.addButton(QTStr("Spectra.ClipMaker.ShowFile"), QMessageBox::ActionRole);
		box.exec();
		if (box.clickedButton() == show) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
		}
	} else if (!cancelled) {
		OBSMessageBox::warning(this, QTStr("Spectra.ClipMaker.Title"),
				       QTStr("Spectra.ClipMaker.Failed").arg(error));
	}
}

void SpectraClipMaker::closeEvent(QCloseEvent *event)
{
	if (exportState) {
		QMessageBox::StandardButton answer = OBSMessageBox::question(this, QTStr("Spectra.ClipMaker.Title"),
									     QTStr("Spectra.ClipMaker.CancelExport"));
		if (answer != QMessageBox::Yes) {
			event->ignore();
			return;
		}
		exportState->cancel = true;
	}
	SetPlaying(false);
	QDialog::closeEvent(event);
}
