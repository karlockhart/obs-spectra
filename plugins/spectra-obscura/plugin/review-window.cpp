#include "review-window.hpp"

#include <obs-module.h>

#include <spectra-vision/imaging.hpp>

#include <QAction>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDateTime>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTimeZone>
#include <QVBoxLayout>

using namespace spectra::censor;

namespace obscura {

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

constexpr long long kMinTs = 1400000000, kMaxTs = 2200000000;

} // namespace

ReviewWindow::ReviewWindow(Controller *controller) : QMainWindow(nullptr), c(controller)
{
	setWindowTitle(T("Obscura.Review.Title"));
	resize(1500, 860);

	view = new ImageView();
	connect(view, &ImageView::rectDrawn, this, &ReviewWindow::RectDrawn);

	/* Push buttons rather than a QToolBar: OBS themes elide text-only tool buttons */
	QWidget *bar = new QWidget();
	QHBoxLayout *tools = new QHBoxLayout(bar);
	tools->setContentsMargins(4, 4, 4, 4);
	QButtonGroup *modes = new QButtonGroup(this);
	auto mode = [&](const char *key, const char *tip) {
		QPushButton *b = new QPushButton(T(key));
		b->setCheckable(true);
		if (tip) {
			b->setToolTip(T(tip));
		}
		modes->addButton(b);
		tools->addWidget(b);
		connect(b, &QPushButton::toggled, this, &ReviewWindow::ModeChanged);
		return b;
	};
	auto button = [&](const char *key, auto slot) {
		QPushButton *b = new QPushButton(T(key));
		tools->addWidget(b);
		connect(b, &QPushButton::clicked, this, slot);
		return b;
	};
	modeSelect = mode("Obscura.Review.SelectLines", nullptr);
	modeDraw = mode("Obscura.Review.DrawBox", "Obscura.Review.DrawBox.Tip");
	modeRegion = mode("Obscura.Review.SetRegion", "Obscura.Review.SetRegion.Tip");
	modeSelect->setChecked(true);
	tools->addSpacing(12);
	button("Obscura.Review.CensorAll", [this] { SetAll(true); });
	button("Obscura.Review.KeepAll", [this] { SetAll(false); });
	button("Obscura.Review.Suggested", [this] { ResetSuggested(); });
	tools->addSpacing(12);
	preview = new QPushButton(T("Obscura.Review.Preview"));
	preview->setCheckable(true);
	preview->setToolTip(T("Obscura.Review.Preview.Tip"));
	tools->addWidget(preview);
	connect(preview, &QPushButton::toggled, this, &ReviewWindow::UpdatePreview);
	tools->addSpacing(12);
	button("Obscura.Review.Settings", [this] { c->OpenSettings(this); });
	tools->addStretch(1);
	setMenuWidget(bar);

	QWidget *side = new QWidget();
	QVBoxLayout *lay = new QVBoxLayout(side);
	sourceLabel = new QLabel();
	sourceLabel->setWordWrap(true);
	sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	lay->addWidget(sourceLabel);
	QHBoxLayout *tsRow = new QHBoxLayout();
	tsRow->addWidget(new QLabel(T("Obscura.Review.HudTimestamp")));
	tsEdit = new QLineEdit();
	tsEdit->setPlaceholderText(T("Obscura.Review.UnixTimestamp"));
	connect(tsEdit, &QLineEdit::textChanged, this, &ReviewWindow::TsChanged);
	tsRow->addWidget(tsEdit);
	lay->addLayout(tsRow);
	tsReadable = new QLabel();
	lay->addWidget(tsReadable);
	notes = new QLabel();
	notes->setWordWrap(true);
	notes->setStyleSheet(QStringLiteral("color: #e0a030;"));
	lay->addWidget(notes);
	lay->addWidget(new QLabel(T("Obscura.Review.EntriesHeader")));
	list = new QListWidget();
	list->setTextElideMode(Qt::ElideRight);
	connect(list, &QListWidget::itemChanged, this, &ReviewWindow::ListChanged);
	connect(list, &QListWidget::currentRowChanged, this, &ReviewWindow::ListSelected);
	lay->addWidget(list, 1);
	QLabel *hint = new QLabel(T("Obscura.Review.Hint"));
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color: gray;"));
	lay->addWidget(hint);

	QHBoxLayout *buttons = new QHBoxLayout();
	queueLabel = new QLabel();
	buttons->addWidget(queueLabel, 1);
	QPushButton *skip = new QPushButton(T("Obscura.Review.Skip"));
	skip->setToolTip(T("Obscura.Review.Skip.Tip"));
	connect(skip, &QPushButton::clicked, this, &ReviewWindow::SkipJob);
	uploadBtn = new QPushButton(T("Obscura.Review.SaveUpload"));
	connect(uploadBtn, &QPushButton::clicked, this, [this] { Save(true); });
	saveBtn = new QPushButton(T("Obscura.Review.Save"));
	saveBtn->setDefault(true);
	saveBtn->setToolTip(QStringLiteral("Ctrl+S"));
	connect(saveBtn, &QPushButton::clicked, this, [this] { Save(false); });
	buttons->addWidget(skip);
	buttons->addWidget(uploadBtn);
	buttons->addWidget(saveBtn);
	lay->addLayout(buttons);

	auto shortcut = [this](const char *keys, auto slot) {
		QAction *a = new QAction(this);
		a->setShortcut(QKeySequence(QString::fromLatin1(keys)));
		connect(a, &QAction::triggered, this, slot);
		addAction(a);
	};
	shortcut("Ctrl+S", [this] { Save(false); });
	shortcut("Ctrl+U", [this] { Save(true); });
	shortcut("Ctrl+Shift+K", [this] { SkipJob(); });
	shortcut("P", [this] { preview->toggle(); });

	QSplitter *splitter = new QSplitter();
	splitter->addWidget(view);
	splitter->addWidget(side);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({1100, 420});
	setCentralWidget(splitter);
}

/* --- loading ------------------------------------------------------------------- */

void ReviewWindow::Load(const JobPtr &j, int queued)
{
	job = j;
	const spectra::Analysis &a = job->analysis;
	preview->setChecked(false);
	modeSelect->setChecked(true);
	modeRegion->setEnabled(job->source != "crop"); /* a crop can't teach the full-screen region */
	entryItems.clear();
	manualItems.clear();
	list->blockSignals(true);
	list->clear();
	list->blockSignals(false);

	view->SetImage(ToQImage(job->image));
	QPen regionPen(QColor(120, 160, 255, 150), 1, Qt::DotLine);
	regionPen.setCosmetic(true);
	const spectra::Rect &cr = a.chatRect;
	view->scene()->addRect(QRectF(cr.x0, cr.y0, cr.x1 - cr.x0, cr.y1 - cr.y0), regionPen)->setZValue(0.5);
	for (size_t i = 0; i < a.entries.size(); i++) {
		const spectra::ChatEntry &e = a.entries[i];
		auto *item = new EntryItem((int)i, e.rects, QStringLiteral("[%1] %2").arg(e.channel, e.Text()),
					   [this](int idx) { Toggle(idx); });
		view->scene()->addItem(item);
		entryItems.push_back(item);
	}
	censor.clear();
	for (const Prediction &p : job->predictions) {
		censor.push_back(p.Censor());
	}
	censor.resize(a.entries.size(), false);
	PopulateList();

	sourceLabel->setText(QStringLiteral("<b>%1</b>").arg(job->Label().toHtmlEscaped()));
	tsEdit->setText(a.unixTs ? QString::number(*a.unixTs) : QString());
	TsChanged();
	QStringList n = a.notes;
	if (job->uploadAfter && !a.entries.empty()) {
		n << T("Obscura.Review.CropHasChat");
	}
	notes->setText(n.join("\n"));
	const bool keyed = LoadImgbbKey().has_value();
	uploadBtn->setEnabled(keyed);
	uploadBtn->setToolTip(keyed ? T("Obscura.Review.SaveUpload.Tip") : T("Obscura.Error.NoKey"));
	/* a region crop was captured to be uploaded: make that the Enter button */
	const bool wantsUpload = job->uploadAfter && keyed;
	uploadBtn->setDefault(wantsUpload);
	saveBtn->setDefault(!wantsUpload);
	SetQueueCount(queued);
	Present();
}

void ReviewWindow::Clear()
{
	job.reset();
	entryItems.clear();
	manualItems.clear();
	censor.clear();
	list->blockSignals(true);
	list->clear();
	list->blockSignals(false);
	view->ClearImage();
	hide();
}

void ReviewWindow::Present()
{
	show();
	setWindowState(windowState() & ~Qt::WindowMinimized);
	raise();
	activateWindow();
}

void ReviewWindow::SetQueueCount(int queued)
{
	queueLabel->setText(queued ? T("Obscura.Review.MoreQueued").arg(queued) : QString());
}

void ReviewWindow::PopulateList()
{
	list->blockSignals(true);
	list->clear();
	const auto &entries = job->analysis.entries;
	for (size_t i = 0; i < entries.size(); i++) {
		const spectra::ChatEntry &e = entries[i];
		const Prediction p = i < job->predictions.size() ? job->predictions[i] : Prediction{};
		const QString verdict = p.Censor() ? T("Obscura.Review.Censor") : T("Obscura.Review.Keep");
		auto *item = new QListWidgetItem(
			QStringLiteral("%1  %2\n      %3  ·  %4 %5%")
				.arg(e.time.value_or(QStringLiteral("···")), e.body, e.channel, verdict)
				.arg(qRound(p.prob * 100)));
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
		item->setCheckState(censor[i] ? Qt::Checked : Qt::Unchecked);
		item->setToolTip(
			T("Obscura.Review.EntryTip").arg(e.Text(), e.channel).arg(qRound(p.prob * 100)).arg(p.source));
		item->setData(Qt::UserRole, (int)i);
		list->addItem(item);
		entryItems[i]->SetCensor(censor[i]);
	}
	list->blockSignals(false);
}

/* --- editing ------------------------------------------------------------------- */

void ReviewWindow::SetCensor(int i, bool value)
{
	censor[i] = value;
	entryItems[i]->SetCensor(value);
	list->blockSignals(true);
	list->item(i)->setCheckState(value ? Qt::Checked : Qt::Unchecked);
	list->blockSignals(false);
	if (preview->isChecked()) {
		UpdatePreview(true);
	}
}

void ReviewWindow::Toggle(int i)
{
	SetCensor(i, !censor[i]);
	list->setCurrentRow(i);
}

void ReviewWindow::SetAll(bool value)
{
	for (int i = 0; i < (int)censor.size(); i++) {
		SetCensor(i, value);
	}
}

void ReviewWindow::ResetSuggested()
{
	if (!job) {
		return;
	}
	for (int i = 0; i < (int)censor.size() && i < (int)job->predictions.size(); i++) {
		SetCensor(i, job->predictions[i].Censor());
	}
}

void ReviewWindow::ListChanged(QListWidgetItem *item)
{
	const int i = item->data(Qt::UserRole).toInt();
	const bool value = item->checkState() == Qt::Checked;
	if (value != censor[i]) {
		SetCensor(i, value);
	}
}

void ReviewWindow::ListSelected(int row)
{
	for (int i = 0; i < (int)entryItems.size(); i++) {
		entryItems[i]->SetSelected(i == row);
	}
	if (row >= 0 && row < (int)entryItems.size()) {
		view->ensureVisible(entryItems[row]);
	}
}

void ReviewWindow::ModeChanged()
{
	const bool drawing = modeDraw->isChecked() || modeRegion->isChecked();
	view->drawing = drawing;
	view->viewport()->setCursor(drawing ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ReviewWindow::RectDrawn(const spectra::Rect &rect)
{
	if (!job) {
		return;
	}
	if (modeDraw->isChecked()) {
		auto *item = new ManualItem(rect, [this](ManualItem *m) { RemoveManual(m); });
		view->scene()->addItem(item);
		manualItems.push_back(item);
		if (preview->isChecked()) {
			UpdatePreview(true);
		}
	} else if (modeRegion->isChecked()) {
		modeSelect->setChecked(true);
		notes->setText(T("Obscura.Review.Reanalysing"));
		c->SetChatRegion(job, rect);
	}
}

void ReviewWindow::RemoveManual(ManualItem *item)
{
	std::erase(manualItems, item);
	view->scene()->removeItem(item);
	delete item;
	if (preview->isChecked()) {
		UpdatePreview(true);
	}
}

std::vector<spectra::Rect> ReviewWindow::ManualRects() const
{
	std::vector<spectra::Rect> out;
	for (ManualItem *m : manualItems) {
		out.push_back(m->Box());
	}
	return out;
}

std::vector<spectra::Rect> ReviewWindow::Rects() const
{
	std::vector<spectra::Rect> rects;
	const auto &entries = job->analysis.entries;
	for (size_t i = 0; i < entries.size(); i++) {
		if (censor[i]) {
			rects.insert(rects.end(), entries[i].rects.begin(), entries[i].rects.end());
		}
	}
	std::vector<spectra::Rect> manual = ManualRects();
	rects.insert(rects.end(), manual.begin(), manual.end());
	return rects;
}

void ReviewWindow::UpdatePreview(bool on)
{
	if (!job) {
		return;
	}
	view->SwapPixmap(ToQImage(on ? spectra::Redact(job->image, Rects(), c->Cfg().fill) : job->image));
	for (EntryItem *item : entryItems) {
		item->setVisible(!on);
	}
	for (ManualItem *item : manualItems) {
		item->setVisible(!on);
	}
}

std::optional<long long> ReviewWindow::TsValue() const
{
	bool ok = false;
	const long long ts = tsEdit->text().trimmed().toLongLong(&ok);
	if (ok && ts >= kMinTs && ts <= kMaxTs) {
		return ts;
	}
	return std::nullopt;
}

void ReviewWindow::TsChanged()
{
	std::optional<long long> ts = TsValue();
	if (!ts) {
		tsReadable->setText(QStringLiteral("<i>%1</i>").arg(T("Obscura.Review.NoTimestamp")));
		return;
	}
	const QDateTime utc = QDateTime::fromSecsSinceEpoch(*ts, QTimeZone::UTC);
	tsReadable->setText(T("Obscura.Review.TimestampReadable")
				    .arg(utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
					 utc.toString(QStringLiteral("HH:mm:ss"))));
}

/* --- actions ------------------------------------------------------------------- */

void ReviewWindow::Save(bool upload)
{
	if (job) {
		c->Finalise(job, censor, ManualRects(), TsValue(), true, upload);
	}
}

void ReviewWindow::SkipJob()
{
	if (job) {
		c->Skip(job);
	}
}

void ReviewWindow::closeEvent(QCloseEvent *event)
{
	if (allowClose) {
		event->accept();
	} else {
		event->ignore();
		hide();
	}
}

} // namespace obscura
