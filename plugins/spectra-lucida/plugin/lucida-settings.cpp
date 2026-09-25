#include "lucida-settings.hpp"
#include "lucida-regions.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace lucida {

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QLabel *Note(const QString &text)
{
	QLabel *l = new QLabel(text);
	l->setWordWrap(true);
	l->setStyleSheet(QStringLiteral("color: gray;"));
	return l;
}

QCheckBox *Check(const char *key, bool checked)
{
	QCheckBox *b = new QCheckBox(T(key));
	b->setChecked(checked);
	return b;
}

QDoubleSpinBox *Seconds(double value, double min, double max)
{
	QDoubleSpinBox *s = new QDoubleSpinBox();
	s->setRange(min, max);
	s->setDecimals(1);
	s->setSingleStep(0.5);
	s->setSuffix(QStringLiteral(" s"));
	s->setValue(value);
	return s;
}

QSpinBox *Int(int value, int min, int max, const QString &suffix = QString())
{
	QSpinBox *s = new QSpinBox();
	s->setRange(min, max);
	s->setSuffix(suffix);
	s->setValue(value);
	return s;
}

QWidget *Page(QLayout *layout)
{
	QWidget *page = new QWidget();
	page->setLayout(layout);
	return page;
}

enum Column { kTag, kTriggers };

} // namespace

SettingsDialog::SettingsDialog(Controller *controller_, QWidget *parent) : QDialog(parent), controller(controller_)
{
	setWindowTitle(T("Lucida.Settings.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(640, 560);

	const Settings &s = controller->CurrentSettings();
	QTabWidget *tabs = new QTabWidget();
	tabs->addTab(GeneralPage(s), T("Lucida.Settings.General"));
	tabs->addTab(TagsPage(s), T("Lucida.Settings.Tags"));
	tabs->addTab(SamplingPage(s), T("Lucida.Settings.Sampling"));
	tabs->addTab(ScreenshotsPage(s), T("Lucida.Settings.Screenshots"));

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addWidget(buttons);
}

QWidget *SettingsDialog::PathRow(QLineEdit *edit, bool file)
{
	QWidget *row = new QWidget();
	QHBoxLayout *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(edit, 1);
	QPushButton *browse = new QPushButton(T("Lucida.Settings.Browse"));
	browse->setAutoDefault(false);
	connect(browse, &QPushButton::clicked, this, [this, edit, file] {
		QString path =
			file ? QFileDialog::getSaveFileName(this, T("Lucida.Settings.DbPath"), edit->text(),
							    QStringLiteral("SQLite (*.db)"), nullptr,
							    QFileDialog::DontConfirmOverwrite)
			     : QFileDialog::getExistingDirectory(this, T("Lucida.Settings.Browse"), edit->text());
		if (!path.isEmpty()) {
			edit->setText(QDir::toNativeSeparators(path));
		}
	});
	layout->addWidget(browse);
	return row;
}

/* --- pages ---------------------------------------------------------------- */

QWidget *SettingsDialog::GeneralPage(const Settings &s)
{
	enabled = Check("Lucida.Settings.Enabled", s.enabled);
	followLoop = Check("Lucida.Settings.FollowLoop", s.followLoop);
	followLoop->setToolTip(T("Lucida.Settings.FollowLoop.Tip"));
	targetProcess = new QLineEdit(s.targetProcess);
	targetProcess->setToolTip(T("Lucida.Settings.TargetProcess.Tip"));
	dbPath = new QLineEdit(QDir::toNativeSeparators(s.dbPath));
	retentionDays = Int(s.retentionDays, 0, 3650, T("Lucida.Settings.Days"));
	retentionDays->setSpecialValueText(T("Lucida.Settings.KeepForever"));

	QFormLayout *form = new QFormLayout();
	form->addRow(enabled);
	form->addRow(followLoop);
	form->addRow(Note(T("Lucida.Settings.FollowLoop.Note")));
	form->addRow(T("Lucida.Settings.TargetProcess"), targetProcess);
	form->addRow(T("Lucida.Settings.DbPath"), PathRow(dbPath, true));
	form->addRow(T("Lucida.Settings.Retention"), retentionDays);
	return Page(form);
}

QWidget *SettingsDialog::SamplingPage(const Settings &s)
{
	const RecorderConfig &r = s.recorder;
	interval = Seconds(r.interval, 0.5, 600);
	minInterval = Seconds(r.minInterval, 0.5, 600);
	maxInterval = Seconds(r.maxInterval, 1, 3600);
	idleInterval = Seconds(r.idleInterval, 1, 3600);
	idleInterval->setToolTip(T("Lucida.Settings.IdleInterval.Tip"));
	adaptive = Check("Lucida.Settings.Adaptive", r.adaptive);
	adaptive->setToolTip(T("Lucida.Settings.Adaptive.Tip"));
	readHud = Check("Lucida.Settings.ReadHud", r.readHud);
	readHud->setToolTip(T("Lucida.Settings.ReadHud.Tip"));
	carnivore = Check("Lucida.Settings.Carnivore", r.carnivore);
	carnivore->setToolTip(T("Lucida.Settings.Carnivore.Tip"));
	gateThreshold = new QDoubleSpinBox();
	gateThreshold->setRange(0.0, 1.0);
	gateThreshold->setDecimals(3);
	gateThreshold->setSingleStep(0.01);
	gateThreshold->setValue(r.gateThreshold);
	gateThreshold->setToolTip(T("Lucida.Settings.Gate.Tip"));
	ocrThreads = Int(s.ocrThreads, 1, 16);

	/* regions as fractions of the frame */
	auto regionRow = [](QDoubleSpinBox **boxes, const spectra::Region &region) {
		QWidget *row = new QWidget();
		QHBoxLayout *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		const double values[4] = {region.left, region.top, region.right, region.bottom};
		const char *labels[4] = {"Lucida.Settings.Left", "Lucida.Settings.Top", "Lucida.Settings.Right",
					 "Lucida.Settings.Bottom"};
		for (int i = 0; i < 4; i++) {
			boxes[i] = new QDoubleSpinBox();
			boxes[i]->setRange(0.0, 1.0);
			boxes[i]->setDecimals(3);
			boxes[i]->setSingleStep(0.01);
			boxes[i]->setValue(values[i]);
			layout->addWidget(new QLabel(T(labels[i])));
			layout->addWidget(boxes[i], 1);
		}
		return row;
	};

	QFormLayout *form = new QFormLayout();
	form->addRow(carnivore);
	form->addRow(Note(T("Lucida.Settings.Carnivore.Note")));
	QPushButton *regions = new QPushButton(T("Lucida.Settings.Regions"));
	regions->setAutoDefault(false);
	connect(regions, &QPushButton::clicked, this, [this] {
		RegionEditor editor(controller, this);
		editor.exec();
	});
	form->addRow(regions);
	form->addRow(T("Lucida.Settings.Interval"), interval);
	form->addRow(adaptive);
	form->addRow(T("Lucida.Settings.MinInterval"), minInterval);
	form->addRow(T("Lucida.Settings.MaxInterval"), maxInterval);
	form->addRow(T("Lucida.Settings.IdleInterval"), idleInterval);
	form->addRow(T("Lucida.Settings.Gate"), gateThreshold);
	form->addRow(T("Lucida.Settings.OcrThreads"), ocrThreads);
	form->addRow(readHud);
	form->addRow(T("Lucida.Settings.ChatRegion"), regionRow(chat, r.chatRegion));
	form->addRow(T("Lucida.Settings.HudRegion"), regionRow(hud, r.hudRegion));
	form->addRow(Note(T("Lucida.Settings.Regions.Note")));
	return Page(form);
}

QWidget *SettingsDialog::ScreenshotsPage(const Settings &s)
{
	const RecorderConfig &r = s.recorder;
	keepFrames = Check("Lucida.Settings.KeepFrames", r.keepFrames);
	keepFrames->setToolTip(T("Lucida.Settings.KeepFrames.Tip"));
	framesDir = new QLineEdit(QDir::toNativeSeparators(r.framesDir));
	frameQuality = Int(r.frameQuality, 10, 100, QStringLiteral(" %"));
	frameRetention = Int(r.frameRetentionDays, 0, 3650, T("Lucida.Settings.Days"));
	frameRetention->setSpecialValueText(T("Lucida.Settings.KeepForever"));
	keepCrops = Check("Lucida.Settings.KeepCrops", r.keepCrops);
	keepCrops->setToolTip(T("Lucida.Settings.KeepCrops.Tip"));
	cropsDir = new QLineEdit(QDir::toNativeSeparators(r.cropsDir));
	cropQuality = Int(r.cropQuality, 10, 100, QStringLiteral(" %"));
	cropRetention = Int(r.cropRetentionDays, 0, 3650, T("Lucida.Settings.Days"));
	cropRetention->setSpecialValueText(T("Lucida.Settings.KeepForever"));

	QFormLayout *form = new QFormLayout();
	form->addRow(keepFrames);
	form->addRow(T("Lucida.Settings.Folder"), PathRow(framesDir, false));
	form->addRow(T("Lucida.Settings.Quality"), frameQuality);
	form->addRow(T("Lucida.Settings.Retention"), frameRetention);
	form->addRow(keepCrops);
	form->addRow(T("Lucida.Settings.Folder"), PathRow(cropsDir, false));
	form->addRow(T("Lucida.Settings.Quality"), cropQuality);
	form->addRow(T("Lucida.Settings.Retention"), cropRetention);
	return Page(form);
}

QWidget *SettingsDialog::TagsPage(const Settings &s)
{
	rules = new QTableWidget(0, 2);
	rules->setHorizontalHeaderLabels({T("Lucida.Settings.Tag"), T("Lucida.Settings.Triggers")});
	rules->horizontalHeader()->setSectionResizeMode(kTag, QHeaderView::ResizeToContents);
	rules->horizontalHeader()->setStretchLastSection(true);
	rules->verticalHeader()->setVisible(false);
	rules->setSelectionBehavior(QAbstractItemView::SelectRows);
	for (const TagRule &rule : s.tagRules) {
		AddRule(rule);
	}
	connect(rules, &QTableWidget::itemChanged, this, &SettingsDialog::UpdateTry);

	QPushButton *add = new QPushButton(T("Lucida.Settings.AddRule"));
	QPushButton *remove = new QPushButton(T("Lucida.Settings.RemoveRule"));
	QPushButton *defaults = new QPushButton(T("Lucida.Settings.DefaultRules"));
	QPushButton *relabel = new QPushButton(T("Lucida.Settings.Relabel"));
	relabel->setToolTip(T("Lucida.Settings.Relabel.Tip"));
	for (QPushButton *b : {add, remove, defaults, relabel}) {
		b->setAutoDefault(false);
	}
	connect(add, &QPushButton::clicked, this, [this] {
		AddRule({});
		rules->setCurrentCell(rules->rowCount() - 1, kTag);
		rules->editItem(rules->item(rules->rowCount() - 1, kTag));
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		QList<int> rows;
		for (const QModelIndex &index : rules->selectionModel()->selectedRows()) {
			rows << index.row();
		}
		std::sort(rows.begin(), rows.end(), std::greater<int>());
		for (int row : rows) {
			rules->removeRow(row);
		}
		UpdateTry();
	});
	connect(defaults, &QPushButton::clicked, this, [this] {
		rules->setRowCount(0);
		for (const TagRule &rule : DefaultTagRules()) {
			AddRule(rule);
		}
	});
	connect(relabel, &QPushButton::clicked, this, [this, relabel] {
		/* re-tag with the rules as shown: apply them first */
		Settings settings = Collect();
		controller->ApplySettings(settings);
		relabel->setEnabled(false);
		relabel->setText(T("Lucida.Settings.Relabelling"));
		controller->Relabel();
	});
	connect(controller, &Controller::relabelled, relabel, [this, relabel](int changed) {
		relabel->setEnabled(true);
		relabel->setText(T("Lucida.Settings.Relabel"));
		QMessageBox::information(this, T("Lucida.Settings.Title"),
					 changed < 0 ? T("Lucida.Settings.RelabelFailed")
						     : T("Lucida.Settings.Relabelled").arg(changed));
	});

	tolerateTypos = Check("Lucida.Settings.TolerateTypos", s.tolerateTypos);
	tolerateTypos->setToolTip(T("Lucida.Settings.TolerateTypos.Tip"));
	connect(tolerateTypos, &QCheckBox::toggled, this, &SettingsDialog::UpdateTry);

	tryLine = new QLineEdit();
	tryLine->setPlaceholderText(T("Lucida.Settings.Try.Placeholder"));
	tryResult = new QLabel();
	connect(tryLine, &QLineEdit::textChanged, this, &SettingsDialog::UpdateTry);

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(add);
	buttons->addWidget(remove);
	buttons->addWidget(defaults);
	buttons->addStretch(1);
	buttons->addWidget(relabel);

	QFormLayout *tryForm = new QFormLayout();
	tryForm->addRow(T("Lucida.Settings.Try"), tryLine);
	tryForm->addRow(QString(), tryResult);

	QVBoxLayout *layout = new QVBoxLayout();
	layout->addWidget(Note(T("Lucida.Settings.Tags.Note")));
	layout->addWidget(rules, 1);
	layout->addLayout(buttons);
	layout->addWidget(tolerateTypos);
	layout->addLayout(tryForm);
	UpdateTry();
	return Page(layout);
}

/* --- tag rules -------------------------------------------------------------- */

void SettingsDialog::AddRule(const TagRule &rule)
{
	const bool blocked = rules->blockSignals(true);
	const int row = rules->rowCount();
	rules->insertRow(row);
	rules->setItem(row, kTag, new QTableWidgetItem(rule.tag));
	QTableWidgetItem *triggers = new QTableWidgetItem(rule.triggers.join(QStringLiteral(", ")));
	triggers->setToolTip(T("Lucida.Settings.Triggers.Tip"));
	rules->setItem(row, kTriggers, triggers);
	rules->blockSignals(blocked);
}

QList<TagRule> SettingsDialog::Rules() const
{
	QList<TagRule> out;
	for (int row = 0; row < rules->rowCount(); row++) {
		TagRule rule;
		rule.tag = rules->item(row, kTag) ? rules->item(row, kTag)->text().trimmed() : QString();
		const QString triggers = rules->item(row, kTriggers) ? rules->item(row, kTriggers)->text() : QString();
		for (const QString &t : triggers.split(',', Qt::SkipEmptyParts)) {
			if (!t.trimmed().isEmpty()) {
				rule.triggers << t.trimmed();
			}
		}
		if (!rule.tag.isEmpty() && !rule.triggers.isEmpty()) {
			out << rule;
		}
	}
	return out;
}

void SettingsDialog::UpdateTry()
{
	if (!tryLine || !tryResult) {
		return;
	}
	const QString text = tryLine->text();
	if (text.trimmed().isEmpty()) {
		tryResult->setText(QString());
		return;
	}
	const QStringList tags = Tagger(Rules(), tolerateTypos->isChecked()).Tags(text);
	tryResult->setText(tags.isEmpty() ? T("Lucida.Settings.Try.None")
					  : T("Lucida.Settings.Try.Tags").arg(tags.join(QStringLiteral(", "))));
}

/* --- saving ----------------------------------------------------------------- */

Settings SettingsDialog::Collect() const
{
	Settings s = controller->CurrentSettings();
	RecorderConfig &r = s.recorder;
	s.enabled = enabled->isChecked();
	s.followLoop = followLoop->isChecked();
	s.targetProcess = targetProcess->text().trimmed();
	s.dbPath = QDir::cleanPath(QDir::fromNativeSeparators(dbPath->text().trimmed()));
	s.retentionDays = retentionDays->value();

	r.interval = interval->value();
	r.minInterval = std::min(minInterval->value(), maxInterval->value());
	r.maxInterval = std::max(minInterval->value(), maxInterval->value());
	r.idleInterval = idleInterval->value();
	r.adaptive = adaptive->isChecked();
	r.readHud = readHud->isChecked();
	r.carnivore = carnivore->isChecked();
	r.gateThreshold = gateThreshold->value();
	s.ocrThreads = ocrThreads->value();
	r.chatRegion = {chat[0]->value(), chat[1]->value(), chat[2]->value(), chat[3]->value()};
	r.hudRegion = {hud[0]->value(), hud[1]->value(), hud[2]->value(), hud[3]->value()};

	r.keepFrames = keepFrames->isChecked();
	r.framesDir = QDir::cleanPath(QDir::fromNativeSeparators(framesDir->text().trimmed()));
	r.frameQuality = frameQuality->value();
	r.frameRetentionDays = frameRetention->value();
	r.keepCrops = keepCrops->isChecked();
	r.cropsDir = QDir::cleanPath(QDir::fromNativeSeparators(cropsDir->text().trimmed()));
	r.cropQuality = cropQuality->value();
	r.cropRetentionDays = cropRetention->value();

	s.tagRules = Rules();
	s.tolerateTypos = tolerateTypos->isChecked();
	return s;
}

void SettingsDialog::accept()
{
	Settings s = Collect();
	if (s.dbPath.isEmpty()) {
		QMessageBox::warning(this, T("Lucida.Settings.Title"), T("Lucida.Settings.NoDbPath"));
		return;
	}
	if (!s.targetProcess.isEmpty() && !QRegularExpression(s.targetProcess).isValid()) {
		QMessageBox::warning(this, T("Lucida.Settings.Title"), T("Lucida.Settings.BadTargetProcess"));
		return;
	}
	for (const spectra::Region &region : {s.recorder.chatRegion, s.recorder.hudRegion}) {
		if (region.right <= region.left || region.bottom <= region.top) {
			QMessageBox::warning(this, T("Lucida.Settings.Title"), T("Lucida.Settings.BadRegion"));
			return;
		}
	}
	controller->ApplySettings(s);
	QDialog::accept();
}

} // namespace lucida
