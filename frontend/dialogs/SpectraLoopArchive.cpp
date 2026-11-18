#include "SpectraLoopArchive.hpp"

#include <utility/SpectraPrisma.hpp>
#include <utility/ClipExport.hpp>
#include <utility/LoopRecorder.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStorageInfo>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "moc_SpectraLoopArchive.cpp"

namespace {

/* As the Clip Maker: a longer gap between segments starts a new session */
constexpr double SESSION_GAP_SEC = 10.0;
constexpr int SegmentRole = Qt::UserRole;
constexpr int NoSegment = -1;
constexpr int COPY_CHUNK = 4 * 1024 * 1024;

QString Gigabytes(qint64 bytes)
{
	return QLocale().formattedDataSize(bytes, 1);
}

QString Duration(double seconds)
{
	const qint64 s = (qint64)(seconds + 0.5);
	if (s >= 3600) {
		return QStringLiteral("%1:%2:%3")
			.arg(s / 3600)
			.arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
			.arg(s % 60, 2, 10, QLatin1Char('0'));
	}
	return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

/* Copies a file in chunks so a big segment can be cancelled and shows progress */
bool CopyFile(const QString &from, const QString &to, const std::atomic<bool> &cancel,
	      const std::function<void(qint64)> &copied, QString &error)
{
	QFile in(from);
	if (!in.open(QIODevice::ReadOnly)) {
		error = in.errorString();
		return false;
	}
	QFile out(to);
	if (!out.open(QIODevice::WriteOnly)) {
		error = out.errorString();
		return false;
	}
	QByteArray buffer;
	while (!in.atEnd()) {
		if (cancel) {
			out.close();
			out.remove();
			error = QStringLiteral("cancelled");
			return false;
		}
		buffer = in.read(COPY_CHUNK);
		if (buffer.isEmpty() && in.error() != QFile::NoError) {
			error = in.errorString();
			out.close();
			out.remove();
			return false;
		}
		if (out.write(buffer) != buffer.size()) {
			error = out.errorString();
			out.close();
			out.remove();
			return false;
		}
		copied(buffer.size());
	}
	out.close();
	if (out.size() != in.size()) {
		error = QStringLiteral("the copy is incomplete");
		out.remove();
		return false;
	}
	return true;
}

QString UniquePath(const QString &base, const QString &ext)
{
	QString path = base + ext;
	for (int n = 2; QFileInfo::exists(path); n++) {
		path = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext);
	}
	return path;
}

} // namespace

SpectraLoopArchive::SpectraLoopArchive(QWidget *parent, LoopRecorder *recorder_) : QDialog(parent), recorder(recorder_)
{
	setWindowTitle(QTStr("Spectra.Archive.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(900, 720);

	auto *layout = new QVBoxLayout(this);

	auto *top = new QHBoxLayout();
	usage = new QLabel(this);
	usage->setWordWrap(true);
	refreshButton = new QPushButton(QTStr("Spectra.Gallery.Refresh"), this);
	auto *openFolder = new QPushButton(QTStr("Spectra.Loop.OpenLoopFolder"), this);
	top->addWidget(usage, 1);
	top->addWidget(refreshButton);
	top->addWidget(openFolder);
	layout->addLayout(top);

	tree = new QTreeWidget(this);
	tree->setColumnCount(5);
	tree->setHeaderLabels({QTStr("Spectra.Archive.Column.Time"), QTStr("Spectra.Archive.Column.Length"),
			       QTStr("Spectra.Archive.Column.Size"), QTStr("Spectra.Archive.Column.Game"),
			       QTStr("Spectra.Archive.Column.Events")});
	tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	tree->setUniformRowHeights(true);
	layout->addWidget(tree, 1);

	selection = new QLabel(this);
	layout->addWidget(selection);

	/* blocks of time: From/To selects every segment that overlaps it */
	auto *rangeBox = new QGroupBox(QTStr("Spectra.Archive.Range"), this);
	auto *rangeOuter = new QVBoxLayout(rangeBox);
	auto *rangeLayout = new QHBoxLayout();
	rangeOuter->addLayout(rangeLayout);
	from = new QDateTimeEdit(rangeBox);
	to = new QDateTimeEdit(rangeBox);
	for (QDateTimeEdit *edit : {from, to}) {
		edit->setCalendarPopup(true);
		edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
	}
	auto *selectRange = new QPushButton(QTStr("Spectra.Archive.SelectRange"), rangeBox);
	trimToRange = new QCheckBox(QTStr("Spectra.Archive.TrimToRange"), rangeBox);
	trimToRange->setToolTip(QTStr("Spectra.Archive.TrimToRangeTip"));
	rangeLayout->addWidget(new QLabel(QTStr("Spectra.Archive.From"), rangeBox));
	rangeLayout->addWidget(from);
	rangeLayout->addWidget(new QLabel(QTStr("Spectra.Archive.To"), rangeBox));
	rangeLayout->addWidget(to);
	rangeLayout->addWidget(selectRange);
	rangeLayout->addWidget(trimToRange);
	rangeLayout->addStretch();
	layout->addWidget(rangeBox);

	/* or the time around one of the events marked with the event hotkeys */
	auto *eventRow = new QHBoxLayout();
	eventPicker = new QComboBox(rangeBox);
	eventPicker->setMinimumContentsLength(30);
	eventMargin = new QSpinBox(rangeBox);
	eventMargin->setRange(1, 240);
	eventMargin->setValue(5);
	eventMargin->setSuffix(QStringLiteral(" min"));
	eventMargin->setPrefix(QStringLiteral("+/- "));
	eventRow->addWidget(new QLabel(QTStr("Spectra.Archive.AroundEvent"), rangeBox));
	eventRow->addWidget(eventPicker, 1);
	eventRow->addWidget(eventMargin);
	rangeOuter->addLayout(eventRow);

	auto *archiveBox = new QGroupBox(QTStr("Spectra.Archive.ArchiveGroup"), this);
	auto *form = new QFormLayout(archiveBox);
	backup.key = "BackupPath";
	archive.key = "ArchivePath";
	const std::pair<Target *, const char *> targets[] = {{&backup, "Spectra.Archive.BackupTo"},
							     {&archive, "Spectra.Archive.ArchiveTo"}};
	for (const auto &[target, label] : targets) {
		auto *row = new QHBoxLayout();
		target->path = new QLineEdit(archiveBox);
		auto *browse = new QPushButton(QTStr("Browse"), archiveBox);
		row->addWidget(target->path, 1);
		row->addWidget(browse);
		form->addRow(QTStr(label), row);
		target->info = new QLabel(archiveBox);
		target->info->setWordWrap(true);
		form->addRow(QString(), target->info);
		QLineEdit *edit = target->path;
		connect(browse, &QPushButton::clicked, this, [this, edit]() {
			QString dir = QFileDialog::getExistingDirectory(this, QTStr("Spectra.Archive.ChooseFolder"),
									edit->text());
			if (!dir.isEmpty()) {
				edit->setText(QDir::toNativeSeparators(dir));
			}
		});
		connect(edit, &QLineEdit::textChanged, this, &SpectraLoopArchive::UpdateSelection);
	}
	format = new QComboBox(archiveBox);
	format->addItem(QTStr("Spectra.Archive.Format.OneFile"), QStringLiteral("file"));
	format->addItem(QTStr("Spectra.Archive.Format.Segments"), QStringLiteral("segments"));
	form->addRow(QTStr("Spectra.Archive.Format"), format);
	auto *explain = new QLabel(QTStr("Spectra.Archive.Explain"), archiveBox);
	explain->setWordWrap(true);
	explain->setProperty("class", "text-muted");
	form->addRow(QString(), explain);
	layout->addWidget(archiveBox);

	progress = new QProgressBar(this);
	progress->setTextVisible(false);
	progress->hide();
	status = new QLabel(this);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(progress);
	layout->addWidget(status);

	auto *buttons = new QDialogButtonBox(this);
	backupButton = buttons->addButton(QTStr("Spectra.Archive.BackUp"), QDialogButtonBox::ActionRole);
	backupButton->setToolTip(QTStr("Spectra.Archive.BackUpTip"));
	archiveButton = buttons->addButton(QTStr("Spectra.Archive.Archive"), QDialogButtonBox::ActionRole);
	archiveButton->setToolTip(QTStr("Spectra.Archive.ArchiveTip"));
	deleteButton = buttons->addButton(QTStr("Spectra.Archive.Delete"), QDialogButtonBox::DestructiveRole);
	prismaButton = buttons->addButton(QTStr("Spectra.Prisma.Export"), QDialogButtonBox::ActionRole);
	prismaButton->setToolTip(QTStr("Spectra.Prisma.ExportLoopsTip"));
	prismaReady = SpectraPrisma::Ready();
	prismaButton->setVisible(prismaReady);
	buttons->addButton(QDialogButtonBox::Close);
	layout->addWidget(buttons);

	LoadSettings();

	connect(refreshButton, &QPushButton::clicked, this, &SpectraLoopArchive::Scan);
	connect(openFolder, &QPushButton::clicked, this, [this]() {
		if (recorder) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(recorder->LoopDirectory()));
		}
	});
	connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *, int column) {
		if (column == 0 && !updatingChecks) {
			rangeSelected = false;
			UpdateSelection();
		}
	});
	connect(selectRange, &QPushButton::clicked, this, &SpectraLoopArchive::SelectRange);
	connect(eventPicker, &QComboBox::activated, this, &SpectraLoopArchive::PickEvent);
	connect(eventMargin, &QSpinBox::valueChanged, this, [this]() {
		if (eventPicker->currentIndex() > 0) {
			PickEvent(eventPicker->currentIndex());
		}
	});
	connect(format, &QComboBox::currentIndexChanged, this, &SpectraLoopArchive::UpdateSelection);
	connect(backupButton, &QPushButton::clicked, this, [this]() { CopyOut(false); });
	connect(archiveButton, &QPushButton::clicked, this, [this]() { CopyOut(true); });
	connect(deleteButton, &QPushButton::clicked, this, &SpectraLoopArchive::DeleteChecked);
	connect(prismaButton, &QPushButton::clicked, this, &SpectraLoopArchive::ExportToPrisma);
	connect(buttons, &QDialogButtonBox::rejected, this, &SpectraLoopArchive::reject);
	if (recorder) {
		connect(recorder, &LoopRecorder::segmentsChanged, this, [this]() {
			if (!working) {
				Scan();
			}
		});
	}

	UpdateSelection();
	Scan();
}

SpectraLoopArchive::~SpectraLoopArchive()
{
	cancel = true;
	if (worker.joinable()) {
		worker.join();
	}
}

void SpectraLoopArchive::reject()
{
	if (working) {
		/* the worker reports "cancelled", which ends the job */
		cancel = true;
		status->setText(QTStr("Spectra.Archive.Cancelling"));
		return;
	}
	SaveSettings();
	QDialog::reject();
}

void SpectraLoopArchive::LoadSettings()
{
	config_t *config = OBSBasic::Get()->Config();
	for (Target *target : {&backup, &archive}) {
		const char *dest = config_get_string(config, "SpectraLoop", target->key);
		target->path->setText(QDir::toNativeSeparators(QString::fromUtf8(dest ? dest : "")));
	}
	const char *fmt = config_get_string(config, "SpectraLoop", "ArchiveFormat");
	int index = format->findData(QString::fromUtf8(fmt ? fmt : "file"));
	format->setCurrentIndex(index >= 0 ? index : 0);
}

void SpectraLoopArchive::SaveSettings()
{
	config_t *config = OBSBasic::Get()->Config();
	for (Target *target : {&backup, &archive}) {
		config_set_string(config, "SpectraLoop", target->key,
				  QT_TO_UTF8(QDir::fromNativeSeparators(target->path->text().trimmed())));
	}
	config_set_string(config, "SpectraLoop", "ArchiveFormat", QT_TO_UTF8(format->currentData().toString()));
	config_save_safe(config, "tmp", nullptr);
}

/* --- scanning ----------------------------------------------------------- */

void SpectraLoopArchive::Scan()
{
	if (!recorder || scanning) {
		return;
	}
	scanning = true;
	refreshButton->setEnabled(false);
	status->setText(QTStr("Spectra.Archive.Scanning"));

	const QString dir = recorder->LoopDirectory();
	const QString current = QDir::cleanPath(recorder->CurrentSegment());
	const int generation = ++scanGeneration;
	QPointer<SpectraLoopArchive> self(this);
	std::thread([self, dir, current, generation]() {
		static const QRegularExpression stamp("\\d{4}-\\d{2}-\\d{2} \\d{2}-\\d{2}-\\d{2}");
		std::vector<Segment> scanned;
		/* names are timestamps, so name order is time order */
		const QFileInfoList files = QDir(dir).entryInfoList({"*.mkv"}, QDir::Files, QDir::Name);
		for (const QFileInfo &file : files) {
			Segment s;
			s.path = QDir::cleanPath(file.absoluteFilePath());
			s.size = file.size();
			s.current = s.path.compare(current, Qt::CaseInsensitive) == 0;
			const QString base = file.completeBaseName();
			QRegularExpressionMatch match = stamp.match(base);
			if (match.hasMatch()) {
				s.start =
					QDateTime::fromString(match.captured(), QStringLiteral("yyyy-MM-dd HH-mm-ss"));
				s.label = base.left(match.capturedStart()).trimmed();
			}
			/* the open segment can't be probed yet */
			s.duration = s.current ? 0.0 : ClipExport::ProbeDuration(s.path.toStdString());
			if (!s.start.isValid()) {
				s.start = file.lastModified().addMSecs(-(qint64)(std::max(0.0, s.duration) * 1000.0));
			}
			scanned.push_back(std::move(s));
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, generation, scanned = std::move(scanned)]() mutable {
				if (self) {
					self->ScanFinished(generation, std::move(scanned));
				}
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpectraLoopArchive::ScanFinished(int generation, std::vector<Segment> scanned)
{
	if (generation != scanGeneration) {
		return;
	}
	scanning = false;
	refreshButton->setEnabled(!working);
	segments = std::move(scanned);
	prismaReady = SpectraPrisma::Ready();
	prismaButton->setVisible(prismaReady);
	events = recorder ? SpectraEvents::Load(recorder->LoopDirectory()) : std::vector<SpectraEvents::Event>();
	Populate();
	UpdateUsage();
	if (!working) {
		status->clear();
	}
}

void SpectraLoopArchive::Populate()
{
	/* keep what was checked across a rescan */
	QSet<QString> checkedPaths;
	QTreeWidgetItemIterator it(tree);
	for (; *it; ++it) {
		int index = (*it)->data(0, SegmentRole).toInt();
		if (index != NoSegment && (*it)->checkState(0) == Qt::Checked) {
			checkedPaths.insert((*it)->data(0, Qt::UserRole + 1).toString());
		}
	}

	updatingChecks = true;
	tree->clear();
	QTreeWidgetItem *day = nullptr;
	QTreeWidgetItem *session = nullptr;
	QDate dayDate;
	double sessionDuration = 0.0;
	qint64 sessionSize = 0, daySize = 0;
	double dayDuration = 0.0;
	QDateTime sessionStart, previousEnd;
	int sessionEvents = 0, dayEvents = 0;

	auto closeSession = [&]() {
		if (!session) {
			return;
		}
		const QDateTime end = previousEnd;
		session->setText(0, QTStr("Spectra.Archive.Session")
					    .arg(QLocale().toString(sessionStart.time(), QLocale::ShortFormat),
						 QLocale().toString(end.time(), QLocale::ShortFormat)));
		session->setText(1, Duration(sessionDuration));
		session->setText(2, Gigabytes(sessionSize));
		if (sessionEvents > 0) {
			session->setText(4, QTStr("Spectra.Archive.EventCount").arg(sessionEvents));
		}
	};
	auto closeDay = [&]() {
		if (!day) {
			return;
		}
		day->setText(1, Duration(dayDuration));
		day->setText(2, Gigabytes(daySize));
		if (dayEvents > 0) {
			day->setText(4, QTStr("Spectra.Archive.EventCount").arg(dayEvents));
		}
	};

	for (int i = 0; i < (int)segments.size(); i++) {
		const Segment &s = segments[i];
		if (!day || s.start.date() != dayDate) {
			closeSession();
			closeDay();
			session = nullptr;
			dayDate = s.start.date();
			day = new QTreeWidgetItem(tree);
			day->setText(0, QLocale().toString(dayDate, QLocale::LongFormat));
			day->setFlags(day->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
			day->setCheckState(0, Qt::Unchecked);
			day->setData(0, SegmentRole, NoSegment);
			daySize = 0;
			dayDuration = 0.0;
			dayEvents = 0;
		}
		const bool newSession = !session || previousEnd.msecsTo(s.start) > SESSION_GAP_SEC * 1000.0;
		if (newSession) {
			closeSession();
			session = new QTreeWidgetItem(day);
			session->setFlags(session->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
			session->setCheckState(0, Qt::Unchecked);
			session->setData(0, SegmentRole, NoSegment);
			session->setText(3, s.label);
			sessionStart = s.start;
			sessionDuration = 0.0;
			sessionSize = 0;
			sessionEvents = 0;
		}

		auto *item = new QTreeWidgetItem(session);
		item->setData(0, SegmentRole, i);
		item->setData(0, Qt::UserRole + 1, s.path);
		item->setText(0, QLocale().toString(s.start.time(), QLocale::LongFormat));
		item->setText(1, s.current ? QTStr("Spectra.Archive.Recording") : Duration(s.duration));
		item->setText(2, Gigabytes(s.size));
		item->setText(3, s.label);
		item->setToolTip(0, QDir::toNativeSeparators(s.path));
		{
			/* the pins that fall in this segment */
			const QDateTime end = s.start.addMSecs((qint64)(std::max(0.0, s.duration) * 1000.0));
			QStringList names, tips;
			for (const SpectraEvents::Event &e : events) {
				if (e.time >= s.start && (s.current || e.time <= end)) {
					names.append(SpectraEvents::DisplayName(e));
					tips.append(QStringLiteral("%1  %2").arg(
						QLocale().toString(e.time.time(), QLocale::LongFormat),
						SpectraEvents::DisplayName(e)));
				}
			}
			if (!names.isEmpty()) {
				item->setText(4, names.join(QStringLiteral(", ")));
				item->setToolTip(4, tips.join(QStringLiteral("\n")));
				sessionEvents += (int)names.size();
				dayEvents += (int)names.size();
			}
		}
		if (s.current) {
			/* being written: neither archived nor deleted */
			item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable & ~Qt::ItemIsEnabled);
		} else {
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(0, checkedPaths.contains(s.path) ? Qt::Checked : Qt::Unchecked);
		}

		const double length = std::max(0.0, s.duration);
		previousEnd = s.start.addMSecs((qint64)(length * 1000.0));
		sessionDuration += length;
		sessionSize += s.size;
		dayDuration += length;
		daySize += s.size;
	}
	closeSession();
	closeDay();
	updatingChecks = false;

	tree->expandToDepth(0);
	for (int c = 1; c < tree->columnCount(); c++) {
		tree->resizeColumnToContents(c);
	}

	/* the events the loop still has footage of */
	const QDateTime picked = eventPicker->currentData().toDateTime();
	eventPicker->clear();
	eventPicker->addItem(events.empty() ? QTStr("Spectra.Archive.NoEvents") : QTStr("Spectra.Archive.PickEvent"));
	for (const SpectraEvents::Event &e : events) {
		bool recorded = false;
		for (const Segment &s : segments) {
			const QDateTime end = s.start.addMSecs((qint64)(std::max(0.0, s.duration) * 1000.0));
			if (e.time >= s.start && (s.current || e.time <= end)) {
				recorded = true;
				break;
			}
		}
		if (recorded) {
			eventPicker->addItem(
				QStringLiteral("%1  %2").arg(QLocale().toString(e.time, QLocale::ShortFormat),
							     SpectraEvents::DisplayName(e)),
				e.time);
		}
	}
	const int again = picked.isValid() ? eventPicker->findData(picked) : -1;
	eventPicker->setCurrentIndex(again > 0 ? again : 0);
	eventPicker->setEnabled(eventPicker->count() > 1);
	if (!segments.empty()) {
		/* the range starts on what the loop holds */
		const QDateTime first = segments.front().start;
		const QDateTime last =
			segments.back().start.addMSecs((qint64)(std::max(0.0, segments.back().duration) * 1000.0));
		from->setDateTimeRange(first, last);
		to->setDateTimeRange(first, last);
		if (!rangeSelected) {
			from->setDateTime(first);
			to->setDateTime(last);
		}
	}
	UpdateSelection();
}

void SpectraLoopArchive::UpdateUsage()
{
	if (!recorder) {
		return;
	}
	qint64 total = 0;
	double seconds = 0.0;
	for (const Segment &s : segments) {
		total += s.size;
		seconds += std::max(0.0, s.duration);
	}
	const QString dir = recorder->LoopDirectory();
	QStorageInfo storage(dir);
	usage->setText(QTStr("Spectra.Archive.Usage")
			       .arg(QDir::toNativeSeparators(dir), Gigabytes(total),
				    Gigabytes((qint64)recorder->QuotaBytes()), Duration(seconds),
				    storage.isValid() ? Gigabytes(storage.bytesAvailable()) : QStringLiteral("?")));
}

/* --- selection ------------------------------------------------------------ */

std::vector<int> SpectraLoopArchive::CheckedSegments() const
{
	std::vector<int> checked;
	QTreeWidgetItemIterator it(tree);
	for (; *it; ++it) {
		const int index = (*it)->data(0, SegmentRole).toInt();
		if (index != NoSegment && index >= 0 && index < (int)segments.size() &&
		    (*it)->checkState(0) == Qt::Checked && !segments[index].current) {
			checked.push_back(index);
		}
	}
	std::sort(checked.begin(), checked.end());
	return checked;
}

std::vector<SpectraLoopArchive::Block> SpectraLoopArchive::Blocks(const std::vector<int> &checked) const
{
	std::vector<Block> blocks;
	for (int index : checked) {
		const Segment &s = segments[index];
		bool joins = false;
		if (!blocks.empty()) {
			const int previous = blocks.back().segments.back();
			const Segment &p = segments[previous];
			const QDateTime previousEnd = p.start.addMSecs((qint64)(std::max(0.0, p.duration) * 1000.0));
			joins = previous == index - 1 && previousEnd.msecsTo(s.start) <= SESSION_GAP_SEC * 1000.0;
		}
		if (!joins) {
			blocks.push_back(Block());
		}
		blocks.back().segments.push_back(index);
	}

	/* trimmed to the From/To range: the first and last segment of a block
	 * may start before or end after it */
	if (rangeSelected && trimToRange->isChecked() && format->currentData().toString() == "file") {
		const QDateTime rangeFrom = from->dateTime();
		const QDateTime rangeTo = to->dateTime();
		for (Block &block : blocks) {
			const QDateTime blockStart = segments[block.segments.front()].start;
			double offset = 0.0;
			double total = 0.0;
			for (int index : block.segments) {
				total += std::max(0.0, segments[index].duration);
			}
			offset = std::max(0.0, blockStart.msecsTo(rangeFrom) / 1000.0);
			const double end = blockStart.msecsTo(rangeTo) / 1000.0;
			block.trimStart = std::min(offset, total);
			block.trimEnd = end < total ? std::max(block.trimStart, end) : -1.0;
		}
	}
	return blocks;
}

void SpectraLoopArchive::UpdateSelection()
{
	const std::vector<int> checked = CheckedSegments();
	qint64 bytes = 0;
	double seconds = 0.0;
	for (int index : checked) {
		bytes += segments[index].size;
		seconds += std::max(0.0, segments[index].duration);
	}
	const size_t blocks = Blocks(checked).size();
	selection->setText(checked.empty() ? QTStr("Spectra.Archive.NothingSelected")
					   : QTStr("Spectra.Archive.Selected")
						     .arg(checked.size())
						     .arg(Duration(seconds), Gigabytes(bytes))
						     .arg(blocks));
	backupButton->setEnabled(!working && !checked.empty() && !backup.path->text().trimmed().isEmpty());
	archiveButton->setEnabled(!working && !checked.empty() && !archive.path->text().trimmed().isEmpty());
	deleteButton->setEnabled(!working && !checked.empty());
	prismaButton->setEnabled(!working && !checked.empty() && prismaReady);
	trimToRange->setEnabled(rangeSelected && format->currentData().toString() == "file");
	UpdateDestinations();
}

void SpectraLoopArchive::PickEvent(int index)
{
	const QDateTime t = eventPicker->itemData(index).toDateTime();
	if (!t.isValid()) {
		return;
	}
	const qint64 margin = (qint64)eventMargin->value() * 60;
	from->setDateTime(t.addSecs(-margin));
	to->setDateTime(t.addSecs(margin));
	SelectRange();
}

std::vector<SpectraEvents::Event> SpectraLoopArchive::BlockEvents(const Block &block) const
{
	std::vector<SpectraEvents::Event> inside;
	const Segment &first = segments[block.segments.front()];
	const Segment &last = segments[block.segments.back()];
	const QDateTime end = last.start.addMSecs((qint64)(std::max(0.0, last.duration) * 1000.0));
	for (const SpectraEvents::Event &e : events) {
		if (e.time < first.start || e.time > end) {
			continue;
		}
		/* the block's file plays its segments back to back: offset by
		 * the segments before the one the event is in */
		double offset = -1.0;
		double before = 0.0;
		for (int index : block.segments) {
			const Segment &s = segments[index];
			const qint64 into = s.start.msecsTo(e.time);
			if (into >= 0 && into <= (qint64)(std::max(0.0, s.duration) * 1000.0)) {
				offset = before + into / 1000.0;
				break;
			}
			before += std::max(0.0, s.duration);
		}
		if (offset < 0.0 || offset < block.trimStart || (block.trimEnd >= 0.0 && offset > block.trimEnd)) {
			continue;
		}
		SpectraEvents::Event copy = e;
		copy.offset = offset - block.trimStart;
		inside.push_back(copy);
	}
	return inside;
}

void SpectraLoopArchive::UpdateDestinations()
{
	UpdateDestination(backup);
	UpdateDestination(archive);
}

void SpectraLoopArchive::SelectRange()
{
	const QDateTime rangeFrom = from->dateTime();
	const QDateTime rangeTo = to->dateTime();
	if (rangeTo <= rangeFrom) {
		status->setText(QTStr("Spectra.Archive.RangeEmpty"));
		return;
	}
	updatingChecks = true;
	QTreeWidgetItemIterator it(tree);
	for (; *it; ++it) {
		const int index = (*it)->data(0, SegmentRole).toInt();
		if (index == NoSegment || index < 0 || index >= (int)segments.size() || segments[index].current) {
			continue;
		}
		const Segment &s = segments[index];
		const QDateTime end = s.start.addMSecs((qint64)(std::max(0.0, s.duration) * 1000.0));
		const bool overlaps = s.start < rangeTo && end > rangeFrom;
		(*it)->setCheckState(0, overlaps ? Qt::Checked : Qt::Unchecked);
	}
	updatingChecks = false;
	rangeSelected = true;
	UpdateSelection();
}

void SpectraLoopArchive::UpdateDestination(Target &target)
{
	const bool isArchive = &target == &archive;
	const QString dest = QDir::fromNativeSeparators(target.path->text().trimmed());
	if (dest.isEmpty()) {
		target.info->setText(
			QTStr(isArchive ? "Spectra.Archive.NoArchiveFolder" : "Spectra.Archive.NoBackupFolder"));
		return;
	}
	QStorageInfo storage(dest);
	if (!storage.isValid()) {
		/* the folder may not exist yet: look at its drive */
		storage = QStorageInfo(QDir(dest).absolutePath().left(3));
	}
	qint64 selected = 0;
	for (int index : CheckedSegments()) {
		selected += segments[index].size;
	}
	QString text = storage.isValid() ? QTStr("Spectra.Archive.Free").arg(Gigabytes(storage.bytesAvailable()))
					 : QString();
	if (recorder && storage.isValid() && QStorageInfo(recorder->LoopDirectory()).rootPath() == storage.rootPath()) {
		text += " " + QTStr(isArchive ? "Spectra.Archive.SameDrive" : "Spectra.Archive.SameDriveBackup");
	}
	if (storage.isValid() && selected > storage.bytesAvailable()) {
		text += " " + QTStr("Spectra.Archive.NoRoom");
	}
	target.info->setText(text);
}

QString SpectraLoopArchive::BlockName(const Block &block) const
{
	const Segment &first = segments[block.segments.front()];
	const Segment &last = segments[block.segments.back()];
	QDateTime start = first.start.addMSecs((qint64)(block.trimStart * 1000.0));
	double total = 0.0;
	for (int index : block.segments) {
		total += std::max(0.0, segments[index].duration);
	}
	QDateTime end = block.trimEnd >= 0.0 ? first.start.addMSecs((qint64)(block.trimEnd * 1000.0))
					     : last.start.addMSecs((qint64)(std::max(0.0, last.duration) * 1000.0));
	const QString label = first.label.isEmpty() ? QStringLiteral("Loop") : first.label;
	return QStringLiteral("%1 %2 to %3")
		.arg(label, start.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")),
		     end.toString(start.date() == end.date() ? QStringLiteral("HH-mm-ss")
							     : QStringLiteral("yyyy-MM-dd HH-mm-ss")));
}

void SpectraLoopArchive::SetWorking(bool on)
{
	working = on;
	tree->setEnabled(!on);
	refreshButton->setEnabled(!on && !scanning);
	backup.path->setEnabled(!on);
	archive.path->setEnabled(!on);
	format->setEnabled(!on);
	progress->setVisible(on);
	UpdateSelection();
}

/* --- archiving and deleting ---------------------------------------------- */

void SpectraLoopArchive::CopyOut(bool move)
{
	if (!recorder || working) {
		return;
	}
	const QString dest = QDir::fromNativeSeparators((move ? archive : backup).path->text().trimmed());
	if (dest.isEmpty() || !QDir().mkpath(dest)) {
		status->setText(QTStr("Spectra.Archive.BadDestination").arg(QDir::toNativeSeparators(dest)));
		return;
	}
	const std::vector<int> checked = CheckedSegments();
	const std::vector<Block> blocks = Blocks(checked);
	if (blocks.empty()) {
		return;
	}
	qint64 bytes = 0;
	for (int index : checked) {
		bytes += segments[index].size;
	}
	QStorageInfo storage(dest);
	if (storage.isValid() && bytes > storage.bytesAvailable()) {
		QMessageBox::warning(
			this, QTStr("Spectra.Archive.Title"),
			QTStr("Spectra.Archive.NoRoomLong").arg(Gigabytes(bytes), Gigabytes(storage.bytesAvailable())));
		return;
	}
	const bool remove = move;
	if (remove &&
	    QMessageBox::question(this, QTStr("Spectra.Archive.Title"),
				  QTStr("Spectra.Archive.RemoveConfirm").arg(checked.size())) != QMessageBox::Yes) {
		return;
	}
	SaveSettings();

	struct Job {
		QString name;
		QStringList files;
		double trimStart;
		double trimEnd;
		std::vector<SpectraEvents::Event> events;
	};
	std::vector<Job> jobs;
	QStringList all;
	for (const Block &block : blocks) {
		Job job{BlockName(block), {}, block.trimStart, block.trimEnd, BlockEvents(block)};
		for (int index : block.segments) {
			job.files.append(segments[index].path);
		}
		all.append(job.files);
		jobs.push_back(std::move(job));
	}

	/* the disk quota must not delete them while they are copied */
	recorder->Lock(all);
	cancel = false;
	SetWorking(true);
	progress->setRange(0, 1000);
	progress->setValue(0);
	status->setText(QTStr(move ? "Spectra.Archive.Working" : "Spectra.Archive.BackingUp").arg(jobs.size()));
	blog(LOG_INFO, "[Spectra] %s %d loop segment(s) in %d block(s) to %s", move ? "Archiving" : "Backing up",
	     (int)all.size(), (int)jobs.size(), QT_TO_UTF8(dest));

	const bool oneFile = format->currentData().toString() == "file";
	if (worker.joinable()) {
		worker.join();
	}
	QPointer<SpectraLoopArchive> self(this);
	worker = std::thread([this, self, jobs, all, dest, oneFile, bytes, remove]() {
		QStringList archived; /* segments safely in the archive */
		QStringList written;
		QString error;
		qint64 done = 0;
		auto report = [this, self, &done, bytes](qint64 more) {
			done += more;
			const int value = bytes > 0 ? (int)std::min<qint64>(1000, done * 1000 / bytes) : 0;
			QMetaObject::invokeMethod(
				qApp,
				[self, value]() {
					if (self) {
						self->progress->setValue(value);
					}
				},
				Qt::QueuedConnection);
		};

		for (const Job &job : jobs) {
			if (cancel) {
				error = QStringLiteral("cancelled");
				break;
			}
			qint64 jobBytes = 0;
			for (const QString &file : job.files) {
				jobBytes += QFileInfo(file).size();
			}
			bool ok = false;
			if (oneFile) {
				/* one portable file per block, joined without re-encoding */
				const QString out = UniquePath(QDir(dest).filePath(job.name), QStringLiteral(".mkv"));
				std::vector<std::string> inputs;
				for (const QString &file : job.files) {
					inputs.push_back(file.toStdString());
				}
				std::string exportError;
				const qint64 before = done;
				ok = ClipExport::Export(inputs, job.trimStart, job.trimEnd, out.toStdString(),
							exportError, [&](float fraction) {
								const qint64 target =
									before + (qint64)(fraction * jobBytes);
								if (target > done) {
									report(target - done);
								}
								return !cancel.load();
							});
				if (ok) {
					written.append(out);
					/* its pins, with their offsets into the file */
					if (!job.events.empty()) {
						SpectraEvents::SaveFile(SpectraEvents::SidecarPath(out), job.events);
					}
					report(before + jobBytes - done);
				} else {
					QFile::remove(out);
					error = cancel ? QStringLiteral("cancelled")
						       : QString::fromStdString(exportError);
				}
			} else {
				/* the segments as they are, in a folder per block */
				const QString folder = UniquePath(QDir(dest).filePath(job.name), QString());
				ok = QDir().mkpath(folder);
				if (!ok) {
					error = QStringLiteral("could not create %1")
							.arg(QDir::toNativeSeparators(folder));
				}
				for (const QString &file : job.files) {
					if (!ok) {
						break;
					}
					const QString out = QDir(folder).filePath(QFileInfo(file).fileName());
					ok = CopyFile(file, out, cancel, report, error);
				}
				if (ok) {
					written.append(folder);
					/* its pins: the folder opens in the Clip Maker like the loop */
					if (!job.events.empty()) {
						std::vector<SpectraEvents::Event> wallClock = job.events;
						for (SpectraEvents::Event &e : wallClock) {
							e.offset = -1.0;
						}
						SpectraEvents::SaveFile(SpectraEvents::FilePath(folder), wallClock);
					}
				}
			}
			if (!ok) {
				break;
			}
			/* trimmed blocks keep the parts outside the range only in the loop */
			if (job.trimStart <= 0.0 && job.trimEnd < 0.0) {
				archived.append(job.files);
			}
		}

		QMetaObject::invokeMethod(
			qApp,
			[self, all, archived, written, error, remove]() {
				if (!self) {
					return;
				}
				SpectraLoopArchive *dialog = self;
				if (dialog->recorder) {
					dialog->recorder->Unlock(all);
				}
				QString message;
				if (error.isEmpty()) {
					message = QTStr(remove ? "Spectra.Archive.Done" : "Spectra.Archive.BackedUp")
							  .arg(written.size());
					blog(LOG_INFO, "[Spectra] Copied %d block(s) of the loop out",
					     (int)written.size());
				} else if (error == "cancelled") {
					message = QTStr("Spectra.Archive.Cancelled").arg(written.size());
				} else {
					message = QTStr("Spectra.Archive.Failed").arg(error).arg(written.size());
					blog(LOG_WARNING, "[Spectra] Loop archive failed: %s", QT_TO_UTF8(error));
				}
				if (remove && !archived.isEmpty() && dialog->recorder) {
					int skipped = 0;
					const quint64 freed = dialog->recorder->RemoveSegments(archived, &skipped);
					message += " " + QTStr("Spectra.Archive.Removed").arg(Gigabytes((qint64)freed));
				}
				dialog->SetWorking(false);
				dialog->status->setText(message);
				dialog->Scan();
			},
			Qt::QueuedConnection);
	});
}

void SpectraLoopArchive::ExportToPrisma()
{
	const std::vector<int> checked = CheckedSegments();
	if (checked.empty() || !SpectraPrisma::Ready()) {
		return;
	}
	int queued = 0;
	for (int index : checked) {
		const Segment &s = segments[index];
		/* the segment's pins go along as tags */
		QStringList tags;
		const QDateTime end = s.start.addMSecs((qint64)(std::max(0.0, s.duration) * 1000.0));
		for (const SpectraEvents::Event &e : events) {
			if (e.time >= s.start && e.time <= end) {
				tags << SpectraEvents::DisplayName(e);
			}
		}
		tags.removeDuplicates();
		const QString label = s.label.isEmpty() ? QStringLiteral("Loop") : s.label;
		const QString title =
			QStringLiteral("%1 %2").arg(label, s.start.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
		if (SpectraPrisma::Export(QStringLiteral("loops"), s.path, title, s.label, s.start, s.duration, tags)) {
			queued++;
		}
	}
	blog(LOG_INFO, "[Spectra] Queued %d loop segment(s) for Prisma", queued);
	status->setText(QTStr("Spectra.Prisma.Queued").arg(queued));
}

void SpectraLoopArchive::DeleteChecked()
{
	if (!recorder || working) {
		return;
	}
	const std::vector<int> checked = CheckedSegments();
	if (checked.empty()) {
		return;
	}
	qint64 bytes = 0;
	double seconds = 0.0;
	QStringList files;
	for (int index : checked) {
		bytes += segments[index].size;
		seconds += std::max(0.0, segments[index].duration);
		files.append(segments[index].path);
	}
	/* loop segments are far too big for the Recycle Bin: say it is for good */
	if (QMessageBox::warning(
		    this, QTStr("Spectra.Archive.Delete"),
		    QTStr("Spectra.Archive.DeleteConfirm").arg(checked.size()).arg(Duration(seconds), Gigabytes(bytes)),
		    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
		return;
	}
	int skipped = 0;
	const quint64 freed = recorder->RemoveSegments(files, &skipped);
	status->setText(skipped ? QTStr("Spectra.Archive.DeletedSome").arg(Gigabytes((qint64)freed)).arg(skipped)
				: QTStr("Spectra.Archive.Deleted").arg(Gigabytes((qint64)freed)));
	Scan();
}
