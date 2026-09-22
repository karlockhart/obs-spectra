#include "lucida-viewer.hpp"
#include "lucida-controller.hpp"

#include <obs-module.h>

#include <spectra-vision/imaging.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>

#include <thread>

using namespace spectra::censor;

namespace lucida {

namespace {
constexpr int kLimit = 500;
constexpr int kRoleLine = Qt::UserRole;

QString Label(const LogLine &l)
{
	QString mark = l.frameId ? QString() : QStringLiteral("   (no shot)");
	return QStringLiteral("%1  %2 %3%4")
		.arg(l.clock.value_or(QStringLiteral("--:--:--")), l.channel.leftJustified(22, ' '), l.body, mark);
}
} // namespace

Viewer::Viewer(Controller *controller_, QWidget *parent) : QMainWindow(parent), controller(controller_)
{
	setWindowTitle(obs_module_text("Lucida.Viewer.Title"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1500, 900);

	image = new ImageView();
	connect(image, &ImageView::rectDrawn, this, &Viewer::RectDrawn);

	list = new QListWidget();
	list->setAlternatingRowColors(true);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	QFont mono = list->font();
	mono.setStyleHint(QFont::Monospace);
	mono.setFamily(QStringLiteral("Consolas"));
	list->setFont(mono);
	connect(list, &QListWidget::currentRowChanged, this, &Viewer::RowChanged);
	connect(list, &QListWidget::itemChanged, this, &Viewer::ItemChanged);

	QSplitter *splitter = new QSplitter();
	splitter->addWidget(list);
	splitter->addWidget(image);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({520, 980});
	setCentralWidget(splitter);

	BuildToolbar();
	status = new QLabel();
	statusBar()->addWidget(status, 1);
	Reload();
}

Store *Viewer::store() const
{
	return controller ? controller->Reader() : nullptr;
}

void Viewer::BuildToolbar()
{
	/* Push buttons rather than a QToolBar: OBS themes size tool buttons for
	 * icons, which elides text-only actions to "..." */
	QWidget *bar = new QWidget();
	QHBoxLayout *row = new QHBoxLayout(bar);
	row->setContentsMargins(4, 4, 4, 4);

	search = new QLineEdit();
	search->setPlaceholderText(obs_module_text("Lucida.Dock.Search"));
	search->setClearButtonEnabled(true);
	search->setMinimumWidth(240);
	search->setMaximumWidth(360);
	connect(search, &QLineEdit::returnPressed, this, &Viewer::Reload);
	row->addWidget(search);

	auto add = [&](const char *key, auto slot, const QKeySequence &shortcut = {}, bool checkable = false) {
		QPushButton *button = new QPushButton(obs_module_text(key));
		button->setCheckable(checkable);
		QString tip = QString::fromUtf8(obs_module_text((QByteArray(key) + ".Tip").constData()));
		if (tip.endsWith(".Tip")) {
			tip.clear();
		}
		if (!shortcut.isEmpty()) {
			QAction *a = new QAction(this);
			a->setShortcut(shortcut);
			addAction(a);
			connect(a, &QAction::triggered, button, &QPushButton::click);
			const QString keys = shortcut.toString(QKeySequence::NativeText);
			tip = tip.isEmpty() ? keys : QStringLiteral("%1 (%2)").arg(tip, keys);
		}
		button->setToolTip(tip);
		connect(button, &QPushButton::clicked, this, slot);
		row->addWidget(button);
		return button;
	};
	auto gap = [&] {
		row->addSpacing(12);
	};

	add("Lucida.Viewer.Reload", [this] { Reload(); }, QKeySequence::Refresh);
	gap();
	add("Lucida.Viewer.CheckStaff", [this] { CheckStaff(); });
	add("Lucida.Viewer.ClearTicks", [this] { SetAll(false); });
	drawButton = add("Lucida.Viewer.Draw", [this](bool checked) { image->drawing = checked; }, {}, true);
	gap();
	add("Lucida.Viewer.Censor", [this] { Censor(); }, QKeySequence(QStringLiteral("Ctrl+R")));
	add("Lucida.Viewer.UndoCensor", [this] { UndoCensor(); });
	add("Lucida.Viewer.SaveAs", [this] { Save(); }, QKeySequence::SaveAs);
	add("Lucida.Viewer.Upload", [this] { Upload(); }, QKeySequence(QStringLiteral("Ctrl+U")));
	row->addStretch(1);
	setMenuWidget(bar);
}

/* --- the log list -------------------------------------------------------- */

void Viewer::Reload()
{
	Store *s = store();
	const QString query = search->text().trimmed();
	std::vector<LogLine> lines;
	if (s) {
		lines = query.isEmpty() ? s->Recent(kLimit) : s->Search(query, kLimit);
	}
	loading = true;
	list->clear();
	for (const LogLine &l : lines) {
		QListWidgetItem *item = new QListWidgetItem(Label(l));
		item->setData(kRoleLine, l.id);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Unchecked);
		item->setToolTip(l.frameId ? QStringLiteral("%1\n%2").arg(l.When(), l.body)
					   : QString::fromUtf8(obs_module_text("Lucida.Viewer.NoShotTip")));
		list->addItem(item);
	}
	loading = false;
	frameId.reset();
	if (!lines.empty()) {
		list->setCurrentRow((int)lines.size() - 1);
	} else {
		ClearImage(obs_module_text(query.isEmpty() ? "Lucida.Viewer.Empty" : "Lucida.Viewer.NoMatch"));
	}
}

void Viewer::ShowLine(long long lineId)
{
	if (!ItemFor(lineId)) {
		search->clear();
		Reload();
	}
	if (QListWidgetItem *item = ItemFor(lineId)) {
		list->setCurrentItem(item);
		list->scrollToItem(item);
	}
}

std::optional<LogLine> Viewer::LineAt(int row)
{
	QListWidgetItem *item = list->item(row);
	if (!item) {
		return std::nullopt;
	}
	const long long id = item->data(kRoleLine).toLongLong();
	for (const LogLine &l : frameLines) {
		if (l.id == id) {
			return l;
		}
	}
	return store() ? store()->Line(id) : std::nullopt;
}

QListWidgetItem *Viewer::ItemFor(long long lineId)
{
	for (int row = 0; row < list->count(); row++) {
		QListWidgetItem *item = list->item(row);
		if (item->data(kRoleLine).toLongLong() == lineId) {
			return item;
		}
	}
	return nullptr;
}

bool Viewer::Checked(long long lineId)
{
	QListWidgetItem *item = ItemFor(lineId);
	return item && item->checkState() == Qt::Checked;
}

/* --- the image ----------------------------------------------------------- */

void Viewer::RowChanged(int row)
{
	std::optional<LogLine> line = LineAt(row);
	if (!line) {
		return;
	}
	if (!line->frameId) {
		ClearImage(obs_module_text("Lucida.Viewer.NoShot"));
		return;
	}
	if (frameId != line->frameId) {
		LoadFrame(*line->frameId);
	}
	for (auto &[id, item] : items) {
		item->SetSelected(id == line->id);
	}
}

void Viewer::LoadFrame(long long id)
{
	std::optional<Frame> frame = store() ? store()->GetFrame(id) : std::nullopt;
	if (!frame) {
		ClearImage(obs_module_text("Lucida.Viewer.FrameGone"));
		return;
	}
	if (!QFileInfo::exists(frame->path)) {
		ClearImage(QString::fromUtf8(obs_module_text("Lucida.Viewer.FileGone")).arg(frame->path));
		return;
	}
	QImage q(frame->path);
	if (q.isNull()) {
		ClearImage(QString::fromUtf8(obs_module_text("Lucida.Viewer.Unreadable"))
				   .arg(QFileInfo(frame->path).fileName()));
		return;
	}

	original = FromQImage(q);
	censored.reset();
	framePath = frame->path;
	frameId = id;
	frameLines.clear();
	for (LogLine &l : store()->FrameLines(id)) {
		if (l.rect) {
			frameLines.push_back(std::move(l));
		}
	}

	image->SetImage(q);
	items.clear();
	manual.clear();
	for (const LogLine &l : frameLines) {
		QString tip = QStringLiteral("[%1] %2").arg(l.clock.value_or(QStringLiteral("--:--:--")), l.body);
		auto *item = new EntryItem((int)l.id, {*l.rect}, tip, [this](int lineId) { OutlineClicked(lineId); });
		image->scene()->addItem(item);
		items[l.id] = item;
		item->SetCensor(Checked(l.id));
	}
	status->setText(QString::fromUtf8(obs_module_text("Lucida.Viewer.FrameInfo"))
				.arg(QFileInfo(framePath).fileName(),
				     QDateTime::fromSecsSinceEpoch(frame->sortTs)
					     .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
				     QStringLiteral("%1x%2").arg(frame->width).arg(frame->height))
				.arg(frameLines.size()));
}

void Viewer::ClearImage(const QString &message)
{
	image->ClearImage();
	original = spectra::Image();
	censored.reset();
	framePath.clear();
	frameId.reset();
	frameLines.clear();
	items.clear();
	manual.clear();
	status->setText(message);
}

/* --- ticking lines ------------------------------------------------------- */

void Viewer::ItemChanged(QListWidgetItem *item)
{
	if (loading) {
		return;
	}
	auto it = items.find(item->data(kRoleLine).toLongLong());
	if (it != items.end()) {
		it->second->SetCensor(item->checkState() == Qt::Checked);
	}
}

void Viewer::OutlineClicked(long long lineId)
{
	if (QListWidgetItem *item = ItemFor(lineId)) {
		item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
	}
}

void Viewer::SetAll(bool value)
{
	for (int row = 0; row < list->count(); row++) {
		list->item(row)->setCheckState(value ? Qt::Checked : Qt::Unchecked);
	}
}

void Viewer::CheckStaff()
{
	/* The channels Obscura is told to treat as sensitive, as a starting point */
	const QStringList words = LoadObscuraPrefs().seedSensitive;
	for (int row = 0; row < list->count(); row++) {
		QListWidgetItem *item = list->item(row);
		const QString text = item->text().toLower();
		for (const QString &w : words) {
			if (!w.isEmpty() && text.contains(w.toLower())) {
				item->setCheckState(Qt::Checked);
				break;
			}
		}
	}
}

/* --- redaction ----------------------------------------------------------- */

void Viewer::RectDrawn(const spectra::Rect &rect)
{
	if (original.empty()) {
		return;
	}
	auto *item = new ManualItem(rect, [this](ManualItem *m) { RemoveManual(m); });
	image->scene()->addItem(item);
	manual.push_back(item);
}

void Viewer::RemoveManual(ManualItem *item)
{
	std::erase(manual, item);
	image->scene()->removeItem(item);
	delete item;
}

std::vector<spectra::Rect> Viewer::Rects()
{
	std::vector<spectra::Rect> rects;
	for (const LogLine &l : frameLines) {
		if (l.rect && Checked(l.id)) {
			rects.push_back(*l.rect);
		}
	}
	for (ManualItem *m : manual) {
		rects.push_back(m->Box());
	}
	return rects;
}

void Viewer::Censor()
{
	if (original.empty()) {
		return;
	}
	std::vector<spectra::Rect> rects = Rects();
	if (rects.empty()) {
		status->setText(obs_module_text("Lucida.Viewer.NothingTicked"));
		return;
	}
	censored = spectra::Redact(original, rects, LoadObscuraPrefs().fill);
	image->SwapPixmap(ToQImage(*censored));
	status->setText(QString::fromUtf8(obs_module_text("Lucida.Viewer.Censored")).arg(rects.size()));
}

void Viewer::UndoCensor()
{
	if (original.empty()) {
		return;
	}
	censored.reset();
	image->SwapPixmap(ToQImage(original));
	status->setText(obs_module_text("Lucida.Viewer.Original"));
}

const spectra::Image *Viewer::Shown() const
{
	if (censored) {
		return &*censored;
	}
	return original.empty() ? nullptr : &original;
}

/* --- output -------------------------------------------------------------- */

void Viewer::Save()
{
	const spectra::Image *img = Shown();
	if (!img) {
		return;
	}
	QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	QString suggested = QDir(dir).filePath(QFileInfo(framePath).completeBaseName() +
					       (censored ? QStringLiteral("_censored.png") : QStringLiteral(".png")));
	QString name = QFileDialog::getSaveFileName(this, obs_module_text("Lucida.Viewer.SaveTitle"), suggested,
						    QStringLiteral("PNG (*.png)"));
	if (name.isEmpty()) {
		return;
	}
	if (WritePng(name, *img)) {
		status->setText(QString::fromUtf8(obs_module_text("Lucida.Viewer.Saved")).arg(name));
	} else {
		QMessageBox::warning(this, obs_module_text("Lucida.Viewer.SaveTitle"),
				     QString::fromUtf8(obs_module_text("Lucida.Viewer.SaveFailed")).arg(name));
	}
}

void Viewer::Upload()
{
	const spectra::Image *img = Shown();
	if (!img) {
		return;
	}
	if (!LoadImgbbKey()) {
		QMessageBox::information(this, obs_module_text("Lucida.Viewer.UploadTitle"),
					 obs_module_text("Lucida.Viewer.NoKey"));
		return;
	}
	QString warning =
		censored ? QString::fromUtf8(obs_module_text("Lucida.Viewer.UploadCensored")).arg(Rects().size())
			 : QString::fromUtf8(obs_module_text("Lucida.Viewer.UploadUncensored"));
	if (QMessageBox::question(this, obs_module_text("Lucida.Viewer.UploadTitle"),
				  warning + "\n\n" + obs_module_text("Lucida.Viewer.UploadConfirm"),
				  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
		return;
	}

	QString temp = QDir::temp().filePath(QStringLiteral("lucida_%1.png").arg(QDateTime::currentSecsSinceEpoch()));
	if (!WritePng(temp, *img)) {
		status->setText(QString::fromUtf8(obs_module_text("Lucida.Viewer.SaveFailed")).arg(temp));
		return;
	}
	status->setText(obs_module_text("Lucida.Viewer.Uploading"));

	/* imgbb can take a while; blocking the window looks like a crash */
	const ObscuraPrefs prefs = LoadObscuraPrefs();
	QPointer<Viewer> self(this);
	std::thread([self, temp, prefs] {
		UploadResult result = UploadToImgbb(temp, prefs.imgbbExpiration);
		QFile::remove(temp); /* a throwaway copy; imgbb has it now */
		QMetaObject::invokeMethod(
			qApp,
			[self, result, prefs] {
				if (!self) {
					return;
				}
				if (!result.ok) {
					self->status->setText(
						QString::fromUtf8(obs_module_text("Lucida.Viewer.UploadFailed"))
							.arg(result.error));
					QMessageBox::warning(self, obs_module_text("Lucida.Viewer.UploadTitle"),
							     result.error);
					return;
				}
				if (prefs.imgbbCopyLink) {
					QApplication::clipboard()->setText(result.url);
				}
				if (prefs.imgbbOpenLink) {
					QDesktopServices::openUrl(QUrl(result.url));
				}
				self->status->setText(
					QString::fromUtf8(obs_module_text(prefs.imgbbCopyLink
										  ? "Lucida.Viewer.UploadedCopied"
										  : "Lucida.Viewer.Uploaded"))
						.arg(result.url));
			},
			Qt::QueuedConnection);
	}).detach();
}

} // namespace lucida
