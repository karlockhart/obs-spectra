#include "SpectraScreenshotGallery.hpp"

#include <utility/SpectraFeatures.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_SpectraScreenshotGallery.cpp"

namespace {

constexpr int THUMB_WIDTH = 224;
constexpr int THUMB_HEIGHT = 126;
constexpr int PathRole = Qt::UserRole;
/* What the screenshot hotkey writes: PNG, or JPEG XR for HDR */
const QStringList IMAGE_PATTERNS = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.webp", "*.jxr"};
/* What imgbb takes: not JPEG XR */
const QStringList UPLOADABLE = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};

bool CanDecode(const QString &path)
{
	return QFileInfo(path).suffix().compare("jxr", Qt::CaseInsensitive) != 0;
}

bool CallObscura(const char *proc, const QString &path)
{
	const QByteArray utf8 = path.toUtf8();
	calldata_t cd = {0};
	calldata_set_string(&cd, "path", utf8.constData());
	const bool ok = proc_handler_call(obs_get_proc_handler(), proc, &cd);
	calldata_free(&cd);
	return ok;
}

} // namespace

QString SpectraScreenshotGallery::ScreenshotFolder()
{
	OBSBasic *main = OBSBasic::Get();
	config_t *config = main ? main->Config() : nullptr;
	if (!config) {
		return QString();
	}
	const char *spectra = config_get_string(config, "Spectra", "ScreenshotsPath");
	if (spectra && *spectra) {
		return QString::fromUtf8(spectra);
	}
	const char *mode = config_get_string(config, "Output", "Mode");
	const char *type = config_get_string(config, "AdvOut", "RecType");
	const char *path = nullptr;
	if (mode && strcmp(mode, "Advanced") == 0) {
		path = type && strcmp(type, "Standard") ? config_get_string(config, "AdvOut", "FFFilePath")
							: config_get_string(config, "AdvOut", "RecFilePath");
	} else {
		path = config_get_string(config, "SimpleOutput", "FilePath");
	}
	return QString::fromUtf8(path ? path : "");
}

SpectraScreenshotGallery::SpectraScreenshotGallery(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setWindowTitle(QTStr("Spectra.Gallery.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(1100, 680);

	auto *layout = new QVBoxLayout(this);

	auto *top = new QHBoxLayout();
	folderLabel = new QLabel(this);
	folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *openFolder = new QPushButton(QTStr("Spectra.Gallery.OpenFolder"), this);
	auto *refresh = new QPushButton(QTStr("Spectra.Gallery.Refresh"), this);
	top->addWidget(folderLabel, 1);
	top->addWidget(refresh);
	top->addWidget(openFolder);
	layout->addLayout(top);

	auto *splitter = new QSplitter(Qt::Horizontal, this);
	list = new QListWidget(splitter);
	list->setViewMode(QListView::IconMode);
	list->setIconSize(QSize(THUMB_WIDTH, THUMB_HEIGHT));
	list->setGridSize(QSize(THUMB_WIDTH + 24, THUMB_HEIGHT + 44));
	list->setResizeMode(QListView::Adjust);
	list->setMovement(QListView::Static);
	list->setUniformItemSizes(true);
	list->setWordWrap(true);
	list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	list->setContextMenuPolicy(Qt::CustomContextMenu);

	auto *side = new QWidget(splitter);
	auto *sideLayout = new QVBoxLayout(side);
	sideLayout->setContentsMargins(8, 0, 0, 0);
	preview = new QLabel(side);
	preview->setAlignment(Qt::AlignCenter);
	preview->setMinimumSize(320, 180);
	preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	details = new QLabel(side);
	details->setWordWrap(true);
	details->setTextInteractionFlags(Qt::TextSelectableByMouse);
	sideLayout->addWidget(preview, 1);
	sideLayout->addWidget(details);

	openButton = new QPushButton(QTStr("Spectra.Gallery.Open"), side);
	showButton = new QPushButton(QTStr("Spectra.Gallery.ShowInFolder"), side);
	copyButton = new QPushButton(QTStr("Spectra.Gallery.Copy"), side);
	obscuraButton = new QPushButton(QTStr("Spectra.Gallery.SendToObscura"), side);
	obscuraButton->setToolTip(QTStr("Spectra.Gallery.SendToObscuraTip"));
	uploadButton = new QPushButton(QTStr("Spectra.Gallery.Upload"), side);
	uploadButton->setToolTip(QTStr("Spectra.Gallery.UploadTip"));
	deleteButton = new QPushButton(QTStr("Spectra.Gallery.Delete"), side);
	auto *row1 = new QHBoxLayout();
	row1->addWidget(openButton);
	row1->addWidget(showButton);
	row1->addWidget(copyButton);
	auto *row2 = new QHBoxLayout();
	row2->addWidget(obscuraButton, 1);
	row2->addWidget(uploadButton);
	row2->addWidget(deleteButton);
	sideLayout->addLayout(row1);
	sideLayout->addLayout(row2);
	if (!SpectraFeatures::Available(SpectraFeatures::Feature::Obscura)) {
		obscuraButton->hide();
		uploadButton->hide();
	}
	obscuraNote = new QLabel(side);
	obscuraNote->setWordWrap(true);
	obscuraNote->setProperty("class", "text-muted");
	sideLayout->addWidget(obscuraNote);

	splitter->addWidget(list);
	splitter->addWidget(side);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	layout->addWidget(splitter, 1);

	connect(refresh, &QPushButton::clicked, this, &SpectraScreenshotGallery::Refresh);
	connect(openFolder, &QPushButton::clicked, this, [this]() {
		if (!folder.isEmpty()) {
			QDir().mkpath(folder);
			QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
		}
	});
	connect(list, &QListWidget::itemSelectionChanged, this, &SpectraScreenshotGallery::UpdateSelection);
	connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { OpenSelected(); });
	connect(list, &QListWidget::customContextMenuRequested, this, &SpectraScreenshotGallery::ShowContextMenu);
	connect(openButton, &QPushButton::clicked, this, &SpectraScreenshotGallery::OpenSelected);
	connect(showButton, &QPushButton::clicked, this, &SpectraScreenshotGallery::ShowInFolder);
	connect(copyButton, &QPushButton::clicked, this, &SpectraScreenshotGallery::CopySelected);
	connect(obscuraButton, &QPushButton::clicked, this, &SpectraScreenshotGallery::SendToObscura);
	connect(uploadButton, &QPushButton::clicked, this, &SpectraScreenshotGallery::UploadSelected);
	connect(deleteButton, &QPushButton::clicked, this, &SpectraScreenshotGallery::DeleteSelected);

	/* a burst of changes (a screenshot being written) refreshes once */
	refreshLater.setSingleShot(true);
	refreshLater.setInterval(400);
	connect(&refreshLater, &QTimer::timeout, this, &SpectraScreenshotGallery::Refresh);
	connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { refreshLater.start(); });

	worker = std::thread([this]() { RunWorker(); });

	Refresh();
}

SpectraScreenshotGallery::~SpectraScreenshotGallery()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
		queue.clear();
	}
	wake.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

void SpectraScreenshotGallery::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
	/* the folder or Obscura may have changed while the gallery was hidden */
	Refresh();
}

void SpectraScreenshotGallery::Refresh()
{
	const QString newFolder = ScreenshotFolder();
	if (newFolder != folder) {
		if (!watcher.directories().isEmpty()) {
			watcher.removePaths(watcher.directories());
		}
		folder = newFolder;
		if (!folder.isEmpty() && QFileInfo(folder).isDir()) {
			watcher.addPath(folder);
		}
	}
	folderLabel->setText(folder.isEmpty() ? QTStr("Spectra.Gallery.NoFolder")
					      : QTStr("Spectra.Gallery.Folder").arg(QDir::toNativeSeparators(folder)));

	const QStringList selected = SelectedPaths();
	QDir dir(folder);
	const QFileInfoList files =
		folder.isEmpty() ? QFileInfoList()
				 : dir.entryInfoList(IMAGE_PATTERNS, QDir::Files | QDir::Readable, QDir::Time);

	/* keep the thumbnails already made */
	QHash<QString, QIcon> icons;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		if (item->data(Qt::UserRole + 1).toBool()) {
			icons.insert(item->data(PathRole).toString(), item->icon());
		}
	}

	list->clear();
	int gen;
	{
		std::lock_guard<std::mutex> lock(mutex);
		gen = ++generation;
		queue.clear();
	}

	QPixmap placeholder(THUMB_WIDTH, THUMB_HEIGHT);
	placeholder.fill(palette().color(QPalette::Mid));
	const QIcon placeholderIcon(placeholder);

	for (const QFileInfo &file : files) {
		const QString path = file.absoluteFilePath();
		auto *item = new QListWidgetItem(file.completeBaseName(), list);
		item->setData(PathRole, path);
		item->setToolTip(QDir::toNativeSeparators(path));
		auto it = icons.find(path);
		if (it != icons.end()) {
			item->setIcon(it.value());
			item->setData(Qt::UserRole + 1, true);
		} else {
			item->setIcon(placeholderIcon);
			if (CanDecode(path)) {
				std::lock_guard<std::mutex> lock(mutex);
				queue.emplace_back(gen, path);
			}
		}
		if (selected.contains(path)) {
			item->setSelected(true);
		}
	}
	wake.notify_all();

	if (files.isEmpty()) {
		details->setText(QTStr("Spectra.Gallery.Empty"));
	}
	UpdateSelection();
}

void SpectraScreenshotGallery::RunWorker()
{
	for (;;) {
		std::pair<int, QString> job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [this]() { return stopping || !queue.empty(); });
			if (stopping) {
				return;
			}
			job = queue.front();
			queue.pop_front();
		}

		QImageReader reader(job.second);
		reader.setAutoTransform(true);
		const QSize full = reader.size();
		if (full.isValid()) {
			reader.setScaledSize(full.scaled(THUMB_WIDTH, THUMB_HEIGHT, Qt::KeepAspectRatio));
		}
		Thumb thumb{job.first, job.second, reader.read(), full};

		QPointer<SpectraScreenshotGallery> self(this);
		QMetaObject::invokeMethod(
			this,
			[self, thumb]() {
				if (self) {
					self->OnThumb(thumb);
				}
			},
			Qt::QueuedConnection);
	}
}

void SpectraScreenshotGallery::OnThumb(const Thumb &thumb)
{
	if (thumb.generation != generation || thumb.image.isNull()) {
		return;
	}
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		if (item->data(PathRole).toString() == thumb.path) {
			item->setIcon(QIcon(QPixmap::fromImage(thumb.image)));
			item->setData(Qt::UserRole + 1, true);
			break;
		}
	}
}

QStringList SpectraScreenshotGallery::SelectedPaths() const
{
	QStringList paths;
	if (!list) {
		return paths;
	}
	for (QListWidgetItem *item : list->selectedItems()) {
		paths.append(item->data(PathRole).toString());
	}
	return paths;
}

bool SpectraScreenshotGallery::ObscuraReady() const
{
	calldata_t cd = {0};
	bool ready = false;
	if (proc_handler_call(obs_get_proc_handler(), "spectra_obscura_ready", &cd)) {
		ready = calldata_bool(&cd, "ready");
	}
	calldata_free(&cd);
	return ready;
}

void SpectraScreenshotGallery::UpdateSelection()
{
	const QStringList paths = SelectedPaths();
	const bool any = !paths.isEmpty();
	const bool one = paths.size() == 1;
	const bool obscura = ObscuraReady();

	bool decodable = any, uploadable = any;
	for (const QString &path : paths) {
		decodable = decodable && CanDecode(path);
		uploadable = uploadable && UPLOADABLE.contains(QFileInfo(path).suffix().toLower());
	}

	openButton->setEnabled(any);
	showButton->setEnabled(one);
	copyButton->setEnabled(one && decodable);
	obscuraButton->setEnabled(obscura && decodable);
	uploadButton->setEnabled(obscura && uploadable);
	deleteButton->setEnabled(any);

	if (!SpectraFeatures::Available(SpectraFeatures::Feature::Obscura)) {
		/* not in this edition: the buttons are hidden, nothing to explain */
		obscuraNote->setText(QString());
	} else if (!obscura) {
		obscuraNote->setText(QTStr("Spectra.Gallery.ObscuraOff"));
	} else if (any && !decodable) {
		obscuraNote->setText(QTStr("Spectra.Gallery.HdrNote"));
	} else {
		obscuraNote->setText(QString());
	}
	obscuraNote->setVisible(!obscuraNote->text().isEmpty());

	UpdatePreview();
}

void SpectraScreenshotGallery::UpdatePreview()
{
	const QStringList paths = SelectedPaths();
	if (paths.size() != 1) {
		preview->setPixmap(QPixmap());
		previewImage = QImage();
		preview->setText(paths.isEmpty() ? QString() : QTStr("Spectra.Gallery.Selected").arg(paths.size()));
		if (!paths.isEmpty() || list->count() > 0) {
			details->setText(QString());
		}
		return;
	}

	const QString path = paths.first();
	const QFileInfo info(path);
	QImageReader reader(path);
	reader.setAutoTransform(true);
	const QSize full = reader.size();
	QString text = QStringLiteral("<b>%1</b><br>").arg(info.fileName().toHtmlEscaped());
	if (full.isValid()) {
		text += QStringLiteral("%1 x %2, ").arg(full.width()).arg(full.height());
	}
	text += QLocale().formattedDataSize(info.size()) + QStringLiteral(", ") +
		QLocale().toString(info.lastModified(), QLocale::ShortFormat);
	details->setText(text);

	if (!CanDecode(path)) {
		preview->setPixmap(QPixmap());
		preview->setText(QTStr("Spectra.Gallery.NoPreview"));
		return;
	}
	const QSize box = preview->size().boundedTo(QSize(1600, 900));
	if (full.isValid()) {
		reader.setScaledSize(full.scaled(box, Qt::KeepAspectRatio));
	}
	previewImage = reader.read();
	preview->setText(QString());
	preview->setPixmap(QPixmap::fromImage(previewImage));
}

void SpectraScreenshotGallery::OpenSelected()
{
	for (const QString &path : SelectedPaths()) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	}
}

void SpectraScreenshotGallery::ShowInFolder()
{
	const QStringList paths = SelectedPaths();
	if (paths.size() != 1) {
		return;
	}
#ifdef _WIN32
	QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(paths.first())});
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first()).absolutePath()));
#endif
}

void SpectraScreenshotGallery::CopySelected()
{
	const QStringList paths = SelectedPaths();
	if (paths.size() != 1) {
		return;
	}
	QImage image(paths.first());
	if (!image.isNull()) {
		QApplication::clipboard()->setImage(image);
		main->ShowStatusBarMessage(QTStr("Spectra.Gallery.Copied").arg(QFileInfo(paths.first()).fileName()));
	}
}

void SpectraScreenshotGallery::SendToObscura()
{
	const QStringList paths = SelectedPaths();
	int sent = 0;
	for (const QString &path : paths) {
		if (CanDecode(path) && CallObscura("spectra_obscura_review", path)) {
			sent++;
		}
	}
	if (sent == 0) {
		UpdateSelection();
		return;
	}
	blog(LOG_INFO, "[Spectra] Sent %d screenshot(s) to Obscura", sent);
	main->ShowStatusBarMessage(QTStr("Spectra.Gallery.SentToObscura").arg(sent));
}

void SpectraScreenshotGallery::UploadSelected()
{
	const QStringList paths = SelectedPaths();
	if (paths.isEmpty()) {
		return;
	}
	/* uploading skips censoring, so say so before anything leaves the PC */
	if (QMessageBox::question(this, QTStr("Spectra.Gallery.Upload"),
				  QTStr("Spectra.Gallery.UploadConfirm").arg(paths.size())) != QMessageBox::Yes) {
		return;
	}
	for (const QString &path : paths) {
		if (UPLOADABLE.contains(QFileInfo(path).suffix().toLower())) {
			CallObscura("spectra_obscura_upload", path);
		}
	}
}

void SpectraScreenshotGallery::DeleteSelected()
{
	const QStringList paths = SelectedPaths();
	if (paths.isEmpty()) {
		return;
	}
	const QString question = paths.size() == 1
					 ? QTStr("Spectra.Gallery.DeleteOne").arg(QFileInfo(paths.first()).fileName())
					 : QTStr("Spectra.Gallery.DeleteMany").arg(paths.size());
	if (QMessageBox::question(this, QTStr("Spectra.Gallery.Delete"), question) != QMessageBox::Yes) {
		return;
	}
	QStringList failed;
	for (const QString &path : paths) {
		/* to the Recycle Bin, never gone for good */
		if (!QFile::moveToTrash(path)) {
			failed.append(QFileInfo(path).fileName());
		}
	}
	if (!failed.isEmpty()) {
		QMessageBox::warning(this, QTStr("Spectra.Gallery.Delete"),
				     QTStr("Spectra.Gallery.DeleteFailed").arg(failed.join(", ")));
	}
	Refresh();
}

void SpectraScreenshotGallery::ShowContextMenu(const QPoint &pos)
{
	if (!list->itemAt(pos)) {
		return;
	}
	QMenu menu(this);
	auto add = [&menu](QPushButton *button, void (SpectraScreenshotGallery::*slot)(), QObject *target) {
		QAction *action = menu.addAction(button->text());
		action->setEnabled(button->isEnabled());
		QObject::connect(action, &QAction::triggered, target,
				 [target, slot]() { (static_cast<SpectraScreenshotGallery *>(target)->*slot)(); });
	};
	add(openButton, &SpectraScreenshotGallery::OpenSelected, this);
	add(showButton, &SpectraScreenshotGallery::ShowInFolder, this);
	add(copyButton, &SpectraScreenshotGallery::CopySelected, this);
	menu.addSeparator();
	add(obscuraButton, &SpectraScreenshotGallery::SendToObscura, this);
	add(uploadButton, &SpectraScreenshotGallery::UploadSelected, this);
	menu.addSeparator();
	add(deleteButton, &SpectraScreenshotGallery::DeleteSelected, this);
	menu.exec(list->viewport()->mapToGlobal(pos));
}
