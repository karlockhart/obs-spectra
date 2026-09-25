#include "lucida-viewer.hpp"
#include "lucida-host.hpp"
#include "prisma.hpp"

#include <definitions.hpp>
#include <learner.hpp>
#include <naming.hpp>
#include <obscura-config.hpp>

#include <util/base.h>

#include <spectra-vision/imaging.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <thread>

using namespace spectra::censor;

namespace lucida {

namespace {

constexpr int kLimit = 1000;
constexpr int kGalleryLimit = 300;
constexpr int kThumbWidth = 256;
constexpr double kPlayLeadIn = 5.0;   /* s of context before the line */
constexpr double kClipPadding = 15.0; /* s either side of the line in the clip editor */
constexpr int kRoleLine = Qt::UserRole;
constexpr int kRoleFrame = Qt::UserRole + 1;
constexpr int kRoleSortTs = Qt::UserRole + 2;
constexpr int kRoleSeq = Qt::UserRole + 3;
constexpr int kRoleCloud = Qt::UserRole + 4; /* {source, line_id} of another install's line */
constexpr int kCloudSearchLimit = 100;
constexpr int kCloudTimelineLimit = 200;

enum Column { kTime, kClock, kChannel, kRegion, kTags, kText, kVideo, kSource, kColumns };

/* Rows sort by when the line was said, local and cloud alike */
class LineItem : public QTreeWidgetItem {
public:
	bool operator<(const QTreeWidgetItem &other) const override
	{
		const long long a = data(0, kRoleSortTs).toLongLong(), b = other.data(0, kRoleSortTs).toLongLong();
		return a != b ? a < b : data(0, kRoleSeq).toInt() < other.data(0, kRoleSeq).toInt();
	}
};

QString T(const char *key)
{
	return Text(key);
}

const std::pair<const char *, long long> kPeriods[] = {
	{"Lucida.Viewer.Period.Any", 0},
	{"Lucida.Viewer.Period.Hour", 3600},
	{"Lucida.Viewer.Period.Day", 86400},
	{"Lucida.Viewer.Period.Week", 7 * 86400},
	{"Lucida.Viewer.Period.Month", 30 * 86400},
};

void FillPeriods(QComboBox *combo)
{
	for (const auto &[key, seconds] : kPeriods) {
		combo->addItem(T(key), seconds);
	}
}

std::optional<long long> PeriodStart(const QComboBox *combo)
{
	const long long seconds = combo->currentData().toLongLong();
	if (seconds <= 0) {
		return std::nullopt;
	}
	return QDateTime::currentSecsSinceEpoch() - seconds;
}

/* Keeps the selection when the choices change */
void FillChoices(QComboBox *combo, const char *allKey, const QStringList &values)
{
	const QString current = combo->currentData().toString();
	const bool blocked = combo->blockSignals(true);
	combo->clear();
	combo->addItem(T(allKey), QString());
	for (const QString &v : values) {
		combo->addItem(v, v);
	}
	const int index = combo->findData(current);
	combo->setCurrentIndex(index >= 0 ? index : 0);
	combo->blockSignals(blocked);
}

QString VideoText(const VideoSpot &spot)
{
	return QStringLiteral("%1 @ %2").arg(QFileInfo(spot.path).completeBaseName(), FormatOffset(spot.offset));
}

/* Spectra's Obscura folder, for output when Obscura's config names none */
QString ObscuraOutputFolder(const obscura::Config &cfg)
{
	if (!cfg.outputDir.isEmpty()) {
		return cfg.outputDir;
	}
	const QString path = ProfileString("Spectra", "ObscuraPath");
	if (!path.isEmpty()) {
		return QDir::cleanPath(path);
	}
	return QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("Spectra/Obscura");
}

obscura::Features FeaturesOf(const LogLine &l)
{
	return obscura::Features{l.channel, l.tags, l.body, l.colour};
}

/* A player that can start part way into a file: VLC or mpv */
std::optional<std::pair<QString, QStringList>> PlayerCommand(const QString &file, double start)
{
	const QString seconds = QString::number(std::max(0.0, start), 'f', 1);
	QStringList vlc = {QStandardPaths::findExecutable(QStringLiteral("vlc"))};
#ifdef _WIN32
	for (const char *env : {"ProgramFiles", "ProgramFiles(x86)"}) {
		const QString root = qEnvironmentVariable(env);
		if (!root.isEmpty()) {
			vlc << QDir(root).filePath(QStringLiteral("VideoLAN/VLC/vlc.exe"));
		}
	}
#endif
	for (const QString &exe : vlc) {
		if (!exe.isEmpty() && QFileInfo::exists(exe)) {
			return std::pair{exe, QStringList{QStringLiteral("--start-time=") + seconds,
							  QDir::toNativeSeparators(file)}};
		}
	}
	const QString mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"));
	if (!mpv.isEmpty()) {
		return std::pair{mpv,
				 QStringList{QStringLiteral("--start=") + seconds, QDir::toNativeSeparators(file)}};
	}
	return std::nullopt;
}

} // namespace

Viewer::Viewer(ViewerSource source_, QWidget *parent) : QMainWindow(parent), source(std::move(source_))
{
	setWindowTitle(T("Lucida.Viewer.Title"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1600, 920);

	tabs = new QTabWidget();
	tabs->addTab(BuildLogTab(), T("Lucida.Viewer.LogTab"));
	tabs->addTab(BuildGalleryTab(), T("Lucida.Viewer.GalleryTab"));
	connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
		if (index == 1) {
			ReloadGallery();
		}
	});
	setCentralWidget(tabs);

	status = new QLabel();
	statusBar()->addWidget(status, 1);

	auto shortcut = [this](const QKeySequence &keys, auto slot) {
		QAction *a = new QAction(this);
		a->setShortcut(keys);
		connect(a, &QAction::triggered, this, slot);
		addAction(a);
	};
	shortcut(QKeySequence::Refresh, [this] {
		RefreshFilters();
		Reload();
	});
	shortcut(QKeySequence::Find, [this] {
		tabs->setCurrentIndex(0);
		search->setFocus();
		search->selectAll();
	});
	shortcut(QKeySequence(QStringLiteral("Ctrl+S")), [this] { Save(false); });
	shortcut(QKeySequence(QStringLiteral("Ctrl+U")), [this] { Save(true); });
	shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")), [this] { SaveAs(); });

	RefreshFilters();
	Reload();
	LoadLearner();
}

Viewer::~Viewer()
{
	if (galleryCancel) {
		*galleryCancel = true;
	}
}

Store *Viewer::store() const
{
	return source.reader ? source.reader() : nullptr;
}

/* --- building ------------------------------------------------------------------ */

QWidget *Viewer::BuildLogTab()
{
	/* filters */
	search = new QLineEdit();
	search->setPlaceholderText(T("Lucida.Viewer.Search"));
	search->setToolTip(T("Lucida.Viewer.Search.Tip"));
	search->setClearButtonEnabled(true);
	channel = new QComboBox();
	channel->setToolTip(T("Lucida.Viewer.Channel"));
	label = new QComboBox();
	label->setToolTip(T("Lucida.Viewer.Tag"));
	region = new QComboBox();
	region->setToolTip(T("Lucida.Viewer.Region.Tip"));
	period = new QComboBox();
	FillPeriods(period);
	withShot = new QCheckBox(T("Lucida.Viewer.WithShot"));
	cloud = new QCheckBox(T("Lucida.Viewer.Cloud"));
	cloud->setToolTip(T("Lucida.Viewer.Cloud.Tip"));
	cloud->setVisible(source.cloud != nullptr);
	QPushButton *find = new QPushButton(T("Lucida.Viewer.Find"));
	find->setDefault(true);

	connect(search, &QLineEdit::returnPressed, this, &Viewer::Reload);
	connect(search, &QLineEdit::textChanged, this, [this](const QString &t) {
		if (t.isEmpty()) {
			Reload();
		}
	});
	connect(find, &QPushButton::clicked, this, &Viewer::Reload);
	for (QComboBox *combo : {channel, label, region, period}) {
		connect(combo, &QComboBox::currentIndexChanged, this, &Viewer::Reload);
	}
	connect(withShot, &QCheckBox::toggled, this, &Viewer::Reload);
	connect(cloud, &QCheckBox::toggled, this, &Viewer::Reload);

	QHBoxLayout *filters = new QHBoxLayout();
	filters->addWidget(search, 3);
	filters->addWidget(new QLabel(T("Lucida.Viewer.Channel")));
	filters->addWidget(channel, 1);
	filters->addWidget(new QLabel(T("Lucida.Viewer.Tag")));
	filters->addWidget(label, 1);
	filters->addWidget(new QLabel(T("Lucida.Viewer.Region")));
	filters->addWidget(region, 1);
	filters->addWidget(period);
	filters->addWidget(withShot);
	filters->addWidget(find);
	filters->addWidget(cloud);

	frameFilter = new QLabel();
	clearFrameFilter = new QPushButton(T("Lucida.Viewer.ShowAll"));
	connect(clearFrameFilter, &QPushButton::clicked, this, [this] {
		onlyFrame.reset();
		Reload();
	});
	QHBoxLayout *frameRow = new QHBoxLayout();
	frameRow->addWidget(frameFilter);
	frameRow->addWidget(clearFrameFilter);
	frameRow->addStretch(1);
	frameFilter->hide();
	clearFrameFilter->hide();

	/* results */
	results = new QTreeWidget();
	results->setColumnCount(kColumns);
	results->setHeaderLabels({T("Lucida.Viewer.Col.Time"), T("Lucida.Viewer.Col.Clock"),
				  T("Lucida.Viewer.Col.Channel"), T("Lucida.Viewer.Col.Region"),
				  T("Lucida.Viewer.Col.Tags"), T("Lucida.Viewer.Col.Text"),
				  T("Lucida.Viewer.Col.Video"), T("Lucida.Viewer.Col.Source")});
	results->setColumnHidden(kSource, true);
	results->setRootIsDecorated(false);
	results->setUniformRowHeights(true);
	results->setAlternatingRowColors(true);
	results->setSelectionMode(QAbstractItemView::SingleSelection);
	results->header()->setStretchLastSection(false);
	for (int c : {kTime, kClock, kChannel, kRegion, kTags, kVideo, kSource}) {
		results->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
	}
	results->header()->setSectionResizeMode(kText, QHeaderView::Stretch);
	connect(results, &QTreeWidget::currentItemChanged, this, &Viewer::CurrentChanged);
	connect(results, &QTreeWidget::itemChanged, this, &Viewer::ItemChanged);

	/* censoring, as in Obscura's review window */
	QHBoxLayout *tools = new QHBoxLayout();
	auto button = [&](const char *key, auto slot, bool checkable = false) {
		QPushButton *b = new QPushButton(T(key));
		b->setCheckable(checkable);
		const QString tip = T((QByteArray(key) + ".Tip").constData());
		if (!tip.endsWith(".Tip")) {
			b->setToolTip(tip);
		}
		connect(b, &QPushButton::clicked, this, slot);
		tools->addWidget(b);
		return b;
	};
	button("Lucida.Viewer.CensorAll", [this] { SetAll(true); });
	button("Lucida.Viewer.KeepAll", [this] { SetAll(false); });
	button("Lucida.Viewer.Suggested", [this] { ResetSuggested(); });
	drawButton = button("Lucida.Viewer.Draw", [this](bool checked) { image->drawing = checked; }, true);
	previewButton = button("Lucida.Viewer.Preview", [this] { UpdatePreview(); }, true);
	tools->addStretch(1);
	saveAsButton = button("Lucida.Viewer.SaveAs", [this] { SaveAs(); });
	uploadButton = button("Lucida.Viewer.SaveUpload", [this] { Save(true); });
	saveButton = button("Lucida.Viewer.Save", [this] { Save(false); });

	image = new ImageView();
	connect(image, &ImageView::rectDrawn, this, &Viewer::RectDrawn);

	/* where the line is in the loop recording */
	videoLabel = new QLabel();
	videoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	playButton = new QPushButton(T("Lucida.Viewer.Play"));
	playButton->setToolTip(T("Lucida.Viewer.Play.Tip"));
	editClipButton = new QPushButton(T("Lucida.Viewer.EditClip"));
	editClipButton->setToolTip(T("Lucida.Viewer.EditClip.Tip").arg(kClipPadding));
	showVideoButton = new QPushButton(T("Lucida.Viewer.ShowVideo"));
	copyVideoButton = new QPushButton(T("Lucida.Viewer.CopyVideo"));
	connect(playButton, &QPushButton::clicked, this, &Viewer::PlayVideo);
	connect(editClipButton, &QPushButton::clicked, this, &Viewer::EditClip);
	connect(showVideoButton, &QPushButton::clicked, this, &Viewer::ShowVideoFile);
	connect(copyVideoButton, &QPushButton::clicked, this, &Viewer::CopyVideo);
	QHBoxLayout *videoRow = new QHBoxLayout();
	videoRow->addWidget(videoLabel, 1);
	videoRow->addWidget(playButton);
	videoRow->addWidget(editClipButton);
	videoRow->addWidget(showVideoButton);
	videoRow->addWidget(copyVideoButton);

	QWidget *right = new QWidget();
	QVBoxLayout *rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(0, 0, 0, 0);
	rightLayout->addLayout(tools);
	rightLayout->addWidget(image, 1);
	rightLayout->addLayout(videoRow);

	QSplitter *splitter = new QSplitter();
	splitter->addWidget(results);
	splitter->addWidget(right);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({700, 900});

	QWidget *page = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(page);
	layout->addLayout(filters);
	layout->addLayout(frameRow);
	layout->addWidget(splitter, 1);
	ShowVideo(std::nullopt);
	return page;
}

QWidget *Viewer::BuildGalleryTab()
{
	galleryPeriod = new QComboBox();
	FillPeriods(galleryPeriod);
	galleryPeriod->setCurrentIndex(2); /* last 24 hours */
	galleryLabel = new QComboBox();
	connect(galleryPeriod, &QComboBox::currentIndexChanged, this, &Viewer::ReloadGallery);
	connect(galleryLabel, &QComboBox::currentIndexChanged, this, &Viewer::ReloadGallery);

	gallery = new QListWidget();
	gallery->setViewMode(QListView::IconMode);
	gallery->setResizeMode(QListView::Adjust);
	gallery->setMovement(QListView::Static);
	gallery->setIconSize(QSize(kThumbWidth, kThumbWidth * 9 / 16));
	gallery->setGridSize(QSize(kThumbWidth + 24, kThumbWidth * 9 / 16 + 64));
	gallery->setWordWrap(true);
	gallery->setUniformItemSizes(true);
	connect(gallery, &QListWidget::itemActivated, this,
		[this](QListWidgetItem *item) { ShowFrame(item->data(kRoleFrame).toLongLong()); });

	QHBoxLayout *filters = new QHBoxLayout();
	filters->addWidget(galleryPeriod);
	filters->addWidget(new QLabel(T("Lucida.Viewer.Tag")));
	filters->addWidget(galleryLabel);
	filters->addStretch(1);
	filters->addWidget(new QLabel(T("Lucida.Viewer.Gallery.Hint")));

	QWidget *page = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(page);
	layout->addLayout(filters);
	layout->addWidget(gallery, 1);
	return page;
}

/* --- the log -------------------------------------------------------------------- */

void Viewer::RefreshFilters()
{
	Store *s = store();
	FillChoices(channel, "Lucida.Viewer.AllChannels", s ? s->Channels() : QStringList());
	const QStringList labels = s ? s->Labels() : QStringList();
	FillChoices(label, "Lucida.Viewer.AllTags", labels);
	const QStringList regions = s ? s->RegionNames() : QStringList();
	FillChoices(region, "Lucida.Viewer.AllRegions", regions);
	/* the column and filter only matter once carnivore mode has read something */
	region->setEnabled(!regions.isEmpty());
	results->setColumnHidden(kRegion, regions.isEmpty());
	FillChoices(galleryLabel, "Lucida.Viewer.AllTags", labels);
}

Query Viewer::CurrentQuery() const
{
	Query q;
	q.text = search->text().trimmed();
	q.channel = channel->currentData().toString();
	q.label = label->currentData().toString();
	q.region = region->currentData().toString();
	q.from = PeriodStart(period);
	q.withShot = withShot->isChecked();
	q.frameId = onlyFrame;
	q.limit = kLimit;
	return q;
}

void Viewer::Reload()
{
	Store *s = store();
	const std::optional<long long> keep =
		results->currentItem() ? std::optional(results->currentItem()->data(kTime, kRoleLine).toLongLong())
				       : std::nullopt;
	std::vector<LogLine> lines = s ? s->Find(CurrentQuery()) : std::vector<LogLine>();

	if (onlyFrame) {
		std::optional<Frame> f = s ? s->GetFrame(*onlyFrame) : std::nullopt;
		frameFilter->setText(T("Lucida.Viewer.FrameFilter")
					     .arg(f ? QDateTime::fromSecsSinceEpoch(f->sortTs).toString(
							      QStringLiteral("yyyy-MM-dd HH:mm:ss"))
						    : QString::number(*onlyFrame)));
	}
	frameFilter->setVisible(onlyFrame.has_value());
	clearFrameFilter->setVisible(onlyFrame.has_value());

	const bool withCloud = cloud->isChecked() && !onlyFrame;
	results->setColumnHidden(kSource, !withCloud);
	cloudGeneration++; /* results still coming for the last query are dropped */

	loading = true;
	results->clear();
	QList<QTreeWidgetItem *> rows;
	for (const LogLine &l : lines) {
		QTreeWidgetItem *item = new LineItem();
		item->setData(kTime, kRoleLine, l.id);
		item->setData(kTime, kRoleSortTs, l.sortTs);
		item->setData(kTime, kRoleSeq, l.seq);
		item->setText(kSource, T("Lucida.Viewer.ThisPc"));
		item->setText(kTime,
			      QDateTime::fromSecsSinceEpoch(l.sortTs).toString(QStringLiteral("MM-dd HH:mm:ss")));
		item->setText(kClock, l.clock.value_or(QStringLiteral("--:--:--")));
		item->setText(kChannel, l.channel);
		item->setText(kRegion, l.region);
		item->setText(kTags, l.labels.join(QStringLiteral(", ")));
		item->setText(kText, l.body);
		item->setToolTip(kText, l.body);
		item->setText(kVideo, l.video ? VideoText(*l.video) : QString());
		if (l.frameId) {
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(kTime, Censored(l.id) ? Qt::Checked : Qt::Unchecked);
		} else {
			item->setToolTip(kTime, T("Lucida.Viewer.NoShotTip"));
		}
		rows << item;
	}
	results->addTopLevelItems(rows);
	loading = false;

	if (withCloud) {
		SearchCloud(CurrentQuery());
	}
	if (lines.empty()) {
		if (onlyFrame) {
			LoadFrame(*onlyFrame);
		} else {
			ClearImage(search->text().trimmed().isEmpty() ? T("Lucida.Viewer.Empty")
								      : T("Lucida.Viewer.NoMatch"));
		}
		ShowVideo(std::nullopt);
		return;
	}
	QTreeWidgetItem *select = keep ? ItemFor(*keep) : nullptr;
	if (!select) {
		select = onlyFrame ? rows.first() : rows.last();
	}
	results->setCurrentItem(select);
	results->scrollToItem(select);
	status->setText(T("Lucida.Viewer.Found").arg(lines.size()));
}

void Viewer::ShowLine(long long lineId)
{
	tabs->setCurrentIndex(0);
	if (!ItemFor(lineId)) {
		for (QComboBox *combo : {channel, label, region, period}) {
			combo->blockSignals(true);
			combo->setCurrentIndex(0);
			combo->blockSignals(false);
		}
		withShot->blockSignals(true);
		withShot->setChecked(false);
		withShot->blockSignals(false);
		search->blockSignals(true);
		search->clear();
		search->blockSignals(false);
		onlyFrame.reset();
		Reload();
	}
	if (QTreeWidgetItem *item = ItemFor(lineId)) {
		results->setCurrentItem(item);
		results->scrollToItem(item);
	}
}

void Viewer::ShowFrame(long long frameId)
{
	tabs->setCurrentIndex(0);
	onlyFrame = frameId;
	Reload();
}

QTreeWidgetItem *Viewer::ItemFor(long long lineId) const
{
	for (int i = 0; i < results->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = results->topLevelItem(i);
		if (item->data(kTime, kRoleLine).toLongLong() == lineId) {
			return item;
		}
	}
	return nullptr;
}

void Viewer::CurrentChanged()
{
	QTreeWidgetItem *item = results->currentItem();
	if (item && item->data(kTime, kRoleCloud).isValid()) {
		ShowCloudLine(item);
		return;
	}
	std::optional<LogLine> line = item && store() ? store()->Line(item->data(kTime, kRoleLine).toLongLong())
						      : std::nullopt;
	ShowVideo(line);
	if (!line) {
		return;
	}
	if (!line->frameId) {
		ClearImage(T("Lucida.Viewer.NoShot"));
		return;
	}
	if (!frame || frame->id != *line->frameId) {
		LoadFrame(*line->frameId);
	}
	for (auto &[id, entry] : items) {
		entry->SetSelected(id == line->id);
	}
}

/* --- the cloud ------------------------------------------------------------------ */

void Viewer::SearchCloud(const Query &q)
{
	std::shared_ptr<PrismaClient> client = source.cloud ? source.cloud() : nullptr;
	if (!client) {
		status->setText(T("Lucida.Viewer.CloudNoCredentials"));
		return;
	}
	const int generation = cloudGeneration;
	status->setText(T("Lucida.Viewer.CloudSearching"));
	QPointer<Viewer> self(this);
	std::thread([self, client, q, generation] {
		std::map<QString, QString> names;
		PrismaResult sources = client->Get(QStringLiteral("/v1/lucida/sources"));
		for (const QJsonValue &v : sources.body.toObject().value("sources").toArray()) {
			names[v.toObject().value("source").toString()] = v.toObject().value("name").toString();
		}
		QUrlQuery params;
		if (q.from) {
			params.addQueryItem("from_ts", QString::number(*q.from));
		}
		if (q.to) {
			params.addQueryItem("to_ts", QString::number(*q.to));
		}
		PrismaResult r;
		QJsonArray lines;
		if (!q.text.isEmpty()) {
			params.addQueryItem("q", q.text);
			params.addQueryItem("order", "newest");
			params.addQueryItem("limit", QString::number(kCloudSearchLimit));
			if (!q.label.isEmpty()) {
				params.addQueryItem("label", q.label);
			}
			if (!q.region.isEmpty()) {
				params.addQueryItem("region", q.region);
			}
			QUrlQuery words = params;
			words.addQueryItem("mode", "words");
			r = client->Get(QStringLiteral("/v1/lucida/search"), words);
			lines = r.body.toObject().value("results").toArray();
			/* OCR mangles words: fall back to matching part of one */
			if (r.Ok() && lines.isEmpty() && q.text.size() >= 3) {
				QUrlQuery part = params;
				part.addQueryItem("mode", "substring");
				r = client->Get(QStringLiteral("/v1/lucida/search"), part);
				lines = r.body.toObject().value("results").toArray();
			}
		} else {
			params.addQueryItem("newest_first", "true");
			params.addQueryItem("limit", QString::number(kCloudTimelineLimit));
			r = client->Get(QStringLiteral("/v1/lucida/lines"), params);
			lines = r.body.toObject().value("lines").toArray();
		}
		const QString error = r.Ok() ? QString() : r.Describe();
		const QString own = client->Credentials().clientId;
		QMetaObject::invokeMethod(
			qApp,
			[self, generation, q, lines, error, names, own] {
				if (!self) {
					return;
				}
				for (const auto &[id, name] : names) {
					self->sourceNames[id] = name;
				}
				/* this install's lines are already listed from the local log */
				QJsonArray others;
				for (const QJsonValue &v : lines) {
					if (v.toObject().value("source").toString() != own) {
						others.append(v);
					}
				}
				self->AddCloudLines(generation, q, others, error);
			},
			Qt::QueuedConnection);
	}).detach();
}

void Viewer::AddCloudLines(int generation, const Query &q, const QJsonArray &lines, const QString &error)
{
	if (generation != cloudGeneration) {
		return;
	}
	QList<QTreeWidgetItem *> rows;
	for (const QJsonValue &v : lines) {
		const QJsonObject l = v.toObject();
		QStringList labels;
		for (const QJsonValue &label : l.value("labels").toArray()) {
			labels << label.toString();
		}
		const QString channel = l.value("channel").toString();
		const QString region = l.value("region").toString();
		/* filters Prisma does not apply itself */
		if ((!q.channel.isEmpty() && channel != q.channel) ||
		    (!q.label.isEmpty() && !labels.contains(q.label)) || (!q.region.isEmpty() && region != q.region)) {
			continue;
		}
		const QString src = l.value("source").toString();
		const long long sortTs = l.value("sort_ts").toInteger();
		QTreeWidgetItem *item = new LineItem();
		item->setData(kTime, kRoleSortTs, sortTs);
		item->setData(kTime, kRoleSeq, l.value("seq").toInt());
		item->setData(kTime, kRoleCloud,
			      QVariantMap{{"source", src}, {"line_id", l.value("line_id").toString()}});
		item->setText(kTime, QDateTime::fromSecsSinceEpoch(sortTs).toString(QStringLiteral("MM-dd HH:mm:ss")));
		item->setText(kClock, l.value("clock").toString(QStringLiteral("--:--:--")));
		item->setText(kChannel, channel);
		item->setText(kRegion, region);
		item->setText(kTags, labels.join(QStringLiteral(", ")));
		item->setText(kText, l.value("body").toString());
		item->setToolTip(kText, l.value("snippet").toString(l.value("body").toString()));
		const auto name = sourceNames.find(src);
		item->setText(kSource, name != sourceNames.end() && !name->second.isEmpty() ? name->second : src);
		item->setToolTip(kSource, src);
		for (int c = 0; c < kColumns; c++) {
			item->setForeground(c, results->palette().color(QPalette::Link));
		}
		rows << item;
	}
	const int local = results->topLevelItemCount();
	loading = true;
	QTreeWidgetItem *keep = results->currentItem();
	results->addTopLevelItems(rows);
	results->sortItems(kTime, Qt::AscendingOrder);
	loading = false;
	if (!keep && results->topLevelItemCount()) {
		results->setCurrentItem(results->topLevelItem(results->topLevelItemCount() - 1));
	}
	if (QTreeWidgetItem *current = results->currentItem()) {
		results->scrollToItem(current);
	}
	status->setText(error.isEmpty() ? T("Lucida.Viewer.CloudFound").arg(local + rows.size()).arg(rows.size())
					: T("Lucida.Viewer.CloudFailed").arg(error));
}

void Viewer::ShowCloudLine(QTreeWidgetItem *item)
{
	std::shared_ptr<PrismaClient> client = source.cloud ? source.cloud() : nullptr;
	const QVariantMap id = item->data(kTime, kRoleCloud).toMap();
	ShowVideo(std::nullopt);
	ClearImage(T("Lucida.Viewer.CloudLoading"));
	if (!client) {
		return;
	}
	const QString src = id.value("source").toString();
	const QString lineId = id.value("line_id").toString();
	const QString caption = item->text(kSource) + QStringLiteral("  -  ") + item->text(kTime);
	const int generation = ++cloudShotGeneration;
	QPointer<Viewer> self(this);
	std::thread([self, client, src, lineId, caption, generation] {
		const QString base = QStringLiteral("/v1/lucida/sources/%1").arg(src);
		QImage shot;
		std::optional<spectra::Rect> rect;
		QString message;
		PrismaResult line = client->Get(base + "/lines/" + lineId);
		const QString frameKey = line.body.toObject().value("frame_key").toString();
		const QJsonArray r = line.body.toObject().value("rect").toArray();
		if (r.size() == 4) {
			rect = spectra::Rect{r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()};
		}
		if (!line.Ok()) {
			message = T("Lucida.Viewer.CloudFailed").arg(line.Describe());
		} else if (frameKey.isEmpty()) {
			message = T("Lucida.Viewer.CloudNoShot");
		} else {
			PrismaResult frame = client->Get(base + "/frames/" + frameKey);
			const QString url = frame.body.toObject().value("url").toString();
			if (!frame.Ok() || url.isEmpty()) {
				message = frame.Ok() ? T("Lucida.Viewer.CloudNoShot")
						     : T("Lucida.Viewer.CloudFailed").arg(frame.Describe());
			} else {
				const HttpResponse image = client->Fetch(url);
				if (image.status != 200 || !shot.loadFromData(image.body)) {
					message = T("Lucida.Viewer.CloudFailed")
							  .arg(image.error.isEmpty()
								       ? QStringLiteral("HTTP %1").arg(image.status)
								       : image.error);
				}
			}
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, generation, shot, rect, message, caption] {
				if (!self || generation != self->cloudShotGeneration) {
					return;
				}
				if (shot.isNull()) {
					self->ClearImage(message);
					return;
				}
				self->image->SetImage(shot);
				if (rect) {
					QPen pen(QColor(80, 140, 255), 2);
					pen.setCosmetic(true);
					auto *box = new QGraphicsRectItem(rect->x0, rect->y0, rect->x1 - rect->x0,
									  rect->y1 - rect->y0);
					box->setPen(pen);
					box->setAcceptedMouseButtons(Qt::NoButton);
					self->image->scene()->addItem(box);
				}
				self->status->setText(T("Lucida.Viewer.CloudShot").arg(caption));
			},
			Qt::QueuedConnection);
	}).detach();
}

void Viewer::ShowVideo(const std::optional<LogLine> &line)
{
	video = line && source.videoFor ? source.videoFor(*line) : std::nullopt;
	if (video) {
		videoLabel->setText(
			T("Lucida.Viewer.Video").arg(QFileInfo(video->path).fileName(), FormatOffset(video->offset)));
		videoLabel->setToolTip(QDir::toNativeSeparators(video->path));
	} else {
		videoLabel->setText(line ? T("Lucida.Viewer.NoVideo") : QString());
		videoLabel->setToolTip(QString());
	}
	editClipButton->setVisible(video && source.editClip);
	for (QPushButton *b : {playButton, showVideoButton, copyVideoButton}) {
		b->setEnabled(video.has_value());
	}
}

/* --- the screenshot ------------------------------------------------------------- */

void Viewer::LoadFrame(long long id)
{
	std::optional<Frame> f = store() ? store()->GetFrame(id) : std::nullopt;
	if (!f) {
		ClearImage(T("Lucida.Viewer.FrameGone"));
		return;
	}
	if (!QFileInfo::exists(f->path)) {
		ClearImage(T("Lucida.Viewer.FileGone").arg(f->path));
		return;
	}
	QImage q(f->path);
	if (q.isNull()) {
		ClearImage(T("Lucida.Viewer.Unreadable").arg(QFileInfo(f->path).fileName()));
		return;
	}

	original = FromQImage(q);
	frame = f;
	frameLines.clear();
	suggested.clear();
	for (LogLine &l : store()->FrameLines(id)) {
		if (l.rect) {
			frameLines.push_back(std::move(l));
		}
	}

	image->SetImage(q);
	items.clear();
	manual.clear();
	OutlineRegions();
	for (const LogLine &l : frameLines) {
		QString tip = QStringLiteral("[%1] %2").arg(l.clock.value_or(QStringLiteral("--:--:--")), l.body);
		auto *item = new EntryItem((int)l.id, {*l.rect}, tip,
					   [this](int lineId) { SetCensor(lineId, !Censored(lineId)); });
		image->scene()->addItem(item);
		items[l.id] = item;
	}
	Suggest();
	for (QPushButton *b : {saveButton, uploadButton, saveAsButton}) {
		b->setEnabled(true);
	}
	status->setText(
		T("Lucida.Viewer.FrameInfo")
			.arg(QFileInfo(f->path).fileName(),
			     QDateTime::fromSecsSinceEpoch(f->sortTs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
			     QStringLiteral("%1x%2").arg(f->width).arg(f->height))
			.arg(frameLines.size()));
}

void Viewer::OutlineRegions()
{
	std::map<QString, QRect> areas;
	for (const LogLine &l : frameLines) {
		if (!l.region.isEmpty() && l.rect) {
			const QRect r(QPoint(l.rect->x0, l.rect->y0), QPoint(l.rect->x1 - 1, l.rect->y1 - 1));
			areas[l.region] = areas[l.region].united(r);
		}
	}
	const QColor colour(255, 190, 40);
	for (const auto &[name, area] : areas) {
		QPen pen(colour, 2, Qt::DashLine);
		pen.setCosmetic(true);
		auto *box = new QGraphicsRectItem(area.adjusted(-4, -4, 4, 4));
		box->setPen(pen);
		box->setAcceptedMouseButtons(Qt::NoButton);
		box->setZValue(-1);
		image->scene()->addItem(box);
		auto *label = new QGraphicsSimpleTextItem(name);
		label->setBrush(colour);
		label->setFlag(QGraphicsItem::ItemIgnoresTransformations);
		label->setAcceptedMouseButtons(Qt::NoButton);
		/* above the box's corner at any zoom */
		label->setPos(area.left() - 4, area.top() - 4);
		label->setTransform(QTransform::fromTranslate(0, -label->boundingRect().height()));
		label->setZValue(-1);
		image->scene()->addItem(label);
	}
}

void Viewer::ClearImage(const QString &message)
{
	image->ClearImage(message);
	original = spectra::Image();
	frame.reset();
	frameLines.clear();
	suggested.clear();
	items.clear();
	manual.clear();
	for (QPushButton *b : {saveButton, uploadButton, saveAsButton}) {
		b->setEnabled(false);
	}
	status->setText(message);
}

void Viewer::LoadLearner()
{
	/* Obscura's learner, from the same training data and definitions as
	 * Obscura; loading it can take a moment */
	QPointer<Viewer> self(this);
	std::thread([self] {
		const obscura::Config cfg = obscura::Config::Load();
		std::optional<obscura::Definitions> defs;
		if (QFileInfo::exists(obscura::Config::DefinitionsPath())) {
			defs = obscura::ReadObx(obscura::Config::DefinitionsPath());
		}
		auto learner = std::make_shared<obscura::Learner>(obscura::AppDataDir(), cfg.seedSensitive, defs,
								  cfg.useInstalledDefinitions);
		QMetaObject::invokeMethod(
			qApp,
			[self, learner] {
				if (self) {
					self->learner = learner;
					self->Suggest();
				}
			},
			Qt::QueuedConnection);
	}).detach();
}

void Viewer::Suggest()
{
	if (learner && !frameLines.empty()) {
		std::vector<obscura::Features> features;
		for (const LogLine &l : frameLines) {
			features.push_back(FeaturesOf(l));
		}
		const std::vector<obscura::Prediction> predictions = learner->Predict(features);
		for (size_t i = 0; i < frameLines.size() && i < predictions.size(); i++) {
			suggested[frameLines[i].id] = predictions[i].Censor();
		}
	}
	/* show the result on the outlines and ticks */
	loading = true;
	for (const LogLine &l : frameLines) {
		const bool censor = Censored(l.id);
		if (auto it = items.find(l.id); it != items.end()) {
			it->second->SetCensor(censor);
		}
		if (QTreeWidgetItem *row = ItemFor(l.id)) {
			row->setCheckState(kTime, censor ? Qt::Checked : Qt::Unchecked);
		}
	}
	loading = false;
	UpdatePreview();
}

/* --- ticking lines ---------------------------------------------------------------- */

bool Viewer::Censored(long long lineId) const
{
	if (auto it = decided.find(lineId); it != decided.end()) {
		return it->second;
	}
	auto it = suggested.find(lineId);
	return it != suggested.end() && it->second;
}

void Viewer::SetCensor(long long lineId, bool value)
{
	decided[lineId] = value;
	if (auto it = items.find(lineId); it != items.end()) {
		it->second->SetCensor(value);
	}
	if (QTreeWidgetItem *row = ItemFor(lineId); row && (row->flags() & Qt::ItemIsUserCheckable)) {
		loading = true;
		row->setCheckState(kTime, value ? Qt::Checked : Qt::Unchecked);
		loading = false;
	}
	UpdatePreview();
}

void Viewer::ItemChanged(QTreeWidgetItem *item, int column)
{
	if (loading || column != kTime) {
		return;
	}
	SetCensor(item->data(kTime, kRoleLine).toLongLong(), item->checkState(kTime) == Qt::Checked);
}

void Viewer::SetAll(bool value)
{
	for (const LogLine &l : frameLines) {
		SetCensor(l.id, value);
	}
}

void Viewer::ResetSuggested()
{
	for (const LogLine &l : frameLines) {
		decided.erase(l.id);
	}
	Suggest();
}

void Viewer::RectDrawn(const spectra::Rect &rect)
{
	if (original.empty()) {
		return;
	}
	auto *item = new ManualItem(rect, [this](ManualItem *m) { RemoveManual(m); });
	image->scene()->addItem(item);
	manual.push_back(item);
	UpdatePreview();
}

void Viewer::RemoveManual(ManualItem *item)
{
	std::erase(manual, item);
	image->scene()->removeItem(item);
	delete item;
	UpdatePreview();
}

std::vector<spectra::Rect> Viewer::Rects() const
{
	std::vector<spectra::Rect> rects;
	for (const LogLine &l : frameLines) {
		if (l.rect && Censored(l.id)) {
			rects.push_back(*l.rect);
		}
	}
	for (ManualItem *m : manual) {
		rects.push_back(m->Box());
	}
	return rects;
}

spectra::Image Viewer::Redacted() const
{
	return spectra::Redact(original, Rects(), LoadObscuraPrefs().fill);
}

void Viewer::UpdatePreview()
{
	if (original.empty()) {
		return;
	}
	image->SwapPixmap(ToQImage(previewButton->isChecked() ? Redacted() : original));
}

/* --- output, as Obscura saves a reviewed screenshot -------------------------------- */

void Viewer::Save(bool upload)
{
	if (original.empty() || !frame) {
		return;
	}
	const obscura::Config cfg = obscura::Config::Load();
	bool hud = false;
	for (const LogLine &l : frameLines) {
		hud = hud || l.tsSource == QLatin1String("hud");
	}
	const QString suffix = obscura::TimestampSuffix(hud ? std::optional(frame->sortTs) : std::nullopt,
							QDateTime::fromSecsSinceEpoch(frame->sortTs), cfg.timestampTz);
	const QString folder = ObscuraOutputFolder(cfg);
	QDir().mkpath(folder);
	const QString path = obscura::OutputPath(folder, cfg.filenamePrefix, suffix);
	if (!WritePng(path, Redacted())) {
		QMessageBox::warning(this, T("Lucida.Viewer.SaveTitle"), T("Lucida.Viewer.SaveFailed").arg(path));
		return;
	}
	Learn();
	int censored = 0;
	for (const LogLine &l : frameLines) {
		censored += Censored(l.id) ? 1 : 0;
	}
	status->setText(T("Lucida.Viewer.Saved").arg(QDir::toNativeSeparators(path)) + " " +
			T("Lucida.Viewer.LinesCensored").arg(censored).arg(frameLines.size()));
	blog(LOG_INFO, "[Lucida] Saved %s", path.toUtf8().constData());
	if (upload) {
		Upload(path);
	}
}

void Viewer::SaveAs()
{
	if (original.empty() || !frame) {
		return;
	}
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	const QString suggestedName =
		QDir(dir).filePath(QFileInfo(frame->path).completeBaseName() + QStringLiteral("_censored.png"));
	const QString name = QFileDialog::getSaveFileName(this, T("Lucida.Viewer.SaveTitle"), suggestedName,
							  QStringLiteral("PNG (*.png)"));
	if (name.isEmpty()) {
		return;
	}
	if (WritePng(name, Redacted())) {
		status->setText(T("Lucida.Viewer.Saved").arg(QDir::toNativeSeparators(name)));
	} else {
		QMessageBox::warning(this, T("Lucida.Viewer.SaveTitle"), T("Lucida.Viewer.SaveFailed").arg(name));
	}
}

void Viewer::Learn()
{
	if (!learner || frameLines.empty()) {
		return;
	}
	std::vector<obscura::Features> features;
	std::vector<bool> labels;
	for (const LogLine &l : frameLines) {
		features.push_back(FeaturesOf(l));
		labels.push_back(Censored(l.id));
	}
	/* appends to Obscura's training.jsonl and refits */
	std::thread([learner = learner, features, labels] { learner->AddScreen(features, labels); }).detach();
}

void Viewer::Upload(const QString &path)
{
	if (!LoadImgbbKey()) {
		QMessageBox::information(this, T("Lucida.Viewer.UploadTitle"), T("Lucida.Viewer.NoKey"));
		return;
	}
	status->setText(T("Lucida.Viewer.Uploading"));
	const obscura::Config cfg = obscura::Config::Load();
	QPointer<Viewer> self(this);
	std::thread([self, path, cfg] {
		UploadResult result = UploadToImgbb(path, cfg.imgbbExpiration);
		QMetaObject::invokeMethod(
			qApp,
			[self, result, cfg] {
				if (!self) {
					return;
				}
				if (!result.ok) {
					self->status->setText(T("Lucida.Viewer.UploadFailed").arg(result.error));
					QMessageBox::warning(self, T("Lucida.Viewer.UploadTitle"), result.error);
					return;
				}
				const QString url = result.displayUrl.isEmpty() ? result.url : result.displayUrl;
				if (cfg.imgbbCopyLink) {
					QApplication::clipboard()->setText(url);
				}
				if (cfg.imgbbOpenLink) {
					QDesktopServices::openUrl(QUrl(url));
				}
				self->status->setText(
					T(cfg.imgbbCopyLink ? "Lucida.Viewer.UploadedCopied" : "Lucida.Viewer.Uploaded")
						.arg(url));
			},
			Qt::QueuedConnection);
	}).detach();
}

/* --- video ------------------------------------------------------------------------ */

void Viewer::PlayVideo()
{
	if (!video) {
		return;
	}
	const double start = video->offset - kPlayLeadIn;
	if (auto player = PlayerCommand(video->path, start)) {
		if (QProcess::startDetached(player->first, player->second)) {
			status->setText(T("Lucida.Viewer.Playing").arg(FormatOffset(std::max(0.0, start))));
			return;
		}
	}
	/* no player that can seek: open it and say where to go */
	QDesktopServices::openUrl(QUrl::fromLocalFile(video->path));
	status->setText(T("Lucida.Viewer.PlayFrom").arg(FormatOffset(video->offset)));
}

void Viewer::EditClip()
{
	if (!video || !source.editClip) {
		return;
	}
	if (!QFileInfo::exists(video->path)) {
		/* deleted since the line was selected (disk quota) */
		ShowVideo(std::nullopt);
		status->setText(T("Lucida.Viewer.NoVideo"));
		return;
	}
	if (source.editClip(*video, kClipPadding, kClipPadding)) {
		status->setText(T("Lucida.Viewer.EditingClip").arg(FormatOffset(video->offset)));
	} else {
		status->setText(T("Lucida.Viewer.EditClipFailed"));
	}
}

void Viewer::ShowVideoFile()
{
	if (!video) {
		return;
	}
#ifdef _WIN32
	QProcess::startDetached(QStringLiteral("explorer.exe"),
				{QStringLiteral("/select,"), QDir::toNativeSeparators(video->path)});
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(video->path).absolutePath()));
#endif
}

void Viewer::CopyVideo()
{
	if (!video) {
		return;
	}
	QApplication::clipboard()->setText(
		QStringLiteral("%1 @ %2").arg(QDir::toNativeSeparators(video->path), FormatOffset(video->offset)));
	status->setText(T("Lucida.Viewer.VideoCopied"));
}

/* --- screenshots gallery ------------------------------------------------------------ */

void Viewer::ReloadGallery()
{
	const int generation = ++galleryGeneration;
	if (galleryCancel) {
		*galleryCancel = true;
	}
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	galleryCancel = cancel;
	gallery->clear();
	Store *s = store();
	if (!s) {
		return;
	}
	const QString wanted = galleryLabel->currentData().toString();
	std::vector<std::pair<long long, QString>> paths;
	QPixmap blank(gallery->iconSize());
	blank.fill(Qt::transparent);
	for (const FrameInfo &f : s->Frames(kGalleryLimit, PeriodStart(galleryPeriod))) {
		if (!wanted.isEmpty() && !f.labels.contains(wanted)) {
			continue;
		}
		QString text =
			QDateTime::fromSecsSinceEpoch(f.frame.sortTs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
		text += "\n" + T("Lucida.Viewer.Gallery.Lines").arg(f.lines);
		if (!f.labels.isEmpty()) {
			text += " - " + f.labels.join(QStringLiteral(", "));
		}
		QListWidgetItem *item = new QListWidgetItem(QIcon(blank), text);
		item->setData(kRoleFrame, f.frame.id);
		item->setToolTip(QDir::toNativeSeparators(f.frame.path));
		gallery->addItem(item);
		paths.emplace_back(f.frame.id, f.frame.path);
	}
	status->setText(paths.empty() ? T("Lucida.Viewer.Gallery.Empty")
				      : T("Lucida.Viewer.Gallery.Count").arg(paths.size()));

	/* thumbnails off the UI thread, newest first */
	QPointer<Viewer> self(this);
	const QSize size = gallery->iconSize();
	std::thread([self, generation, paths, size, cancel] {
		for (const auto &[id, path] : paths) {
			if (*cancel) {
				return;
			}
			QImageReader reader(path);
			const QSize full = reader.size();
			if (full.isValid()) {
				reader.setScaledSize(full.scaled(size, Qt::KeepAspectRatio));
			}
			QImage thumb = reader.read();
			QMetaObject::invokeMethod(
				qApp,
				[self, generation, id, thumb] {
					if (!self || self->galleryGeneration != generation) {
						return;
					}
					for (int i = 0; i < self->gallery->count(); i++) {
						QListWidgetItem *item = self->gallery->item(i);
						if (item->data(kRoleFrame).toLongLong() == id && !thumb.isNull()) {
							item->setIcon(QIcon(QPixmap::fromImage(thumb)));
							break;
						}
					}
				},
				Qt::QueuedConnection);
		}
	}).detach();
}

} // namespace lucida
