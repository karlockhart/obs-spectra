#include "lucida-settings.hpp"
#include "lucida-speech.hpp"

#include <spectra-speech/models.hpp>

#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QPointer>
#include <QProgressBar>
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

#include <thread>

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
	tabs->addTab(SpeechPage(s), T("Lucida.Settings.Speech"));

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addWidget(buttons);
}

SettingsDialog::~SettingsDialog()
{
	if (downloadCancel) {
		*downloadCancel = true;
	}
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

QWidget *SettingsDialog::SpeechPage(const Settings &s)
{
	using namespace spectra::speech;
	const SpeechSettings &sp = s.speech;

	speechEnabled = Check("Lucida.Settings.Speech.Enabled", sp.enabled);

	speechModel = new QComboBox();
	for (const ModelInfo &model : WhisperModels()) {
		speechModel->addItem(QStringLiteral("%1 (%2 MB)").arg(model.title).arg(model.size / (1024 * 1024)),
				     model.id);
	}
	const int modelIndex = speechModel->findData(sp.model.isEmpty() ? DefaultWhisperModel() : sp.model);
	speechModel->setCurrentIndex(std::max(modelIndex, 0));
	speechDownload = new QPushButton(T("Lucida.Settings.Speech.Download"));
	speechDownload->setAutoDefault(false);
	speechProgress = new QProgressBar();
	speechProgress->setVisible(false);
	speechModelState = new QLabel();
	connect(speechModel, &QComboBox::currentIndexChanged, this, &SettingsDialog::UpdateModelState);
	connect(speechDownload, &QPushButton::clicked, this, &SettingsDialog::DownloadModel);

	QWidget *modelRow = new QWidget();
	QHBoxLayout *modelLayout = new QHBoxLayout(modelRow);
	modelLayout->setContentsMargins(0, 0, 0, 0);
	modelLayout->addWidget(speechModel, 1);
	modelLayout->addWidget(speechDownload);

	speechLanguage = new QComboBox();
	const std::pair<const char *, const char *> languages[] = {
		{"auto", "Lucida.Settings.Speech.Language.Auto"}, {"en", "Lucida.Settings.Speech.Language.en"},
		{"es", "Lucida.Settings.Speech.Language.es"},     {"fr", "Lucida.Settings.Speech.Language.fr"},
		{"de", "Lucida.Settings.Speech.Language.de"},     {"nl", "Lucida.Settings.Speech.Language.nl"},
		{"pt", "Lucida.Settings.Speech.Language.pt"},     {"it", "Lucida.Settings.Speech.Language.it"},
		{"pl", "Lucida.Settings.Speech.Language.pl"},
	};
	for (const auto &[code, key] : languages) {
		speechLanguage->addItem(T(key), QString::fromLatin1(code));
	}
	speechLanguage->setCurrentIndex(std::max(speechLanguage->findData(sp.language), 0));

	speechWhen = new QComboBox();
	speechWhen->addItem(T("Lucida.Settings.Speech.When.AfterSegment"), (int)SpeechSettings::When::AfterSegment);
	speechWhen->addItem(T("Lucida.Settings.Speech.When.AfterGame"), (int)SpeechSettings::When::AfterGame);
	speechWhen->setCurrentIndex(std::max(speechWhen->findData((int)sp.when), 0));
	speechWhen->setToolTip(T("Lucida.Settings.Speech.When.Tip"));

	speechGpu = Check("Lucida.Settings.Speech.Gpu", sp.useGpu);
	speechGpu->setToolTip(T("Lucida.Settings.Speech.Gpu.Tip"));
	speechMe = Check("Lucida.Settings.Speech.Me", sp.me);
	speechTeamSpeak = Check("Lucida.Settings.Speech.TeamSpeak", sp.teamSpeak);
	speechGame = Check("Lucida.Settings.Speech.Game", sp.game);
	QWidget *speakers = new QWidget();
	QHBoxLayout *speakerLayout = new QHBoxLayout(speakers);
	speakerLayout->setContentsMargins(0, 0, 0, 0);
	speakerLayout->addWidget(speechMe);
	speakerLayout->addWidget(speechTeamSpeak);
	speakerLayout->addWidget(speechGame);
	speakerLayout->addStretch(1);

	speechPrompt = new QLineEdit(sp.prompt);
	speechPrompt->setPlaceholderText(T("Lucida.Settings.Speech.Prompt.Placeholder"));
	speechPrompt->setToolTip(T("Lucida.Settings.Speech.Prompt.Tip"));

	speechOlder = new QPushButton(T("Lucida.Settings.Speech.Older"));
	speechOlder->setAutoDefault(false);
	speechOlderNote = Note(sp.enabled && sp.since <= 0.0 ? T("Lucida.Settings.Speech.Older.All") : QString());
	speechOlder->setEnabled(!(sp.enabled && sp.since <= 0.0));
	connect(speechOlder, &QPushButton::clicked, this, [this] {
		if (QMessageBox::question(this, T("Lucida.Settings.Title"),
					  T("Lucida.Settings.Speech.Older.Confirm")) != QMessageBox::Yes) {
			return;
		}
		speechIncludeOlder = true;
		speechOlder->setEnabled(false);
		speechOlderNote->setText(T("Lucida.Settings.Speech.Older.All"));
	});

	speechStatus = new QLabel(controller->Speech()->Status());
	speechStatus->setWordWrap(true);
	connect(controller->Speech(), &SpeechController::statusChanged, speechStatus, &QLabel::setText);

	QFormLayout *form = new QFormLayout();
	form->addRow(speechEnabled);
	form->addRow(Note(T("Lucida.Settings.Speech.Note")));
	form->addRow(T("Lucida.Settings.Speech.Model"), modelRow);
	form->addRow(QString(), speechModelState);
	form->addRow(QString(), speechProgress);
	form->addRow(T("Lucida.Settings.Speech.Language"), speechLanguage);
	form->addRow(T("Lucida.Settings.Speech.When"), speechWhen);
	form->addRow(speechGpu);
	form->addRow(T("Lucida.Settings.Speech.Speakers"), speakers);
	form->addRow(Note(T("Lucida.Settings.Speech.Speakers.Note")));
	form->addRow(T("Lucida.Settings.Speech.Prompt"), speechPrompt);
	form->addRow(speechOlder);
	form->addRow(speechOlderNote);
	form->addRow(T("Lucida.Settings.Speech.Status"), speechStatus);
	UpdateModelState();
	return Page(form);
}

void SettingsDialog::UpdateModelState()
{
	using namespace spectra::speech;
	const ModelInfo *model = FindWhisperModel(speechModel->currentData().toString());
	if (!model) {
		return;
	}
	const bool installed = ModelInstalled(*model) && ModelInstalled(VadModel());
	const bool downloading = downloadCancel && !*downloadCancel;
	speechModelState->setText(installed ? T("Lucida.Settings.Speech.Installed")
					    : T("Lucida.Settings.Speech.NotInstalled").arg(ModelDirectory()));
	speechDownload->setVisible(!installed || downloading);
	speechDownload->setText(downloading ? T("Lucida.Settings.Speech.Cancel")
					    : T("Lucida.Settings.Speech.Download"));
	speechModel->setEnabled(!downloading);
}

void SettingsDialog::DownloadModel()
{
	using namespace spectra::speech;
	if (downloadCancel && !*downloadCancel) {
		*downloadCancel = true;
		return;
	}
	const ModelInfo *model = FindWhisperModel(speechModel->currentData().toString());
	if (!model) {
		return;
	}

	auto cancel = std::make_shared<std::atomic<bool>>(false);
	downloadCancel = cancel;
	speechProgress->setRange(0, 1000);
	speechProgress->setValue(0);
	speechProgress->setVisible(true);
	UpdateModelState();

	QPointer<SettingsDialog> self(this);
	const ModelInfo whisper = *model;
	std::thread([self, cancel, whisper]() {
		QString error;
		bool ok = true;
		for (const ModelInfo &m : {VadModel(), whisper}) {
			if (ModelInstalled(m)) {
				continue;
			}
			ok = spectra::speech::DownloadModel(
				m,
				[self, cancel, &m](qint64 received, qint64 total) {
					const int permille = total > 0 ? (int)(received * 1000 / total) : 0;
					QMetaObject::invokeMethod(
						qApp,
						[self, permille, title = m.title]() {
							if (self) {
								self->speechProgress->setValue(permille);
								self->speechProgress->setFormat(
									QStringLiteral("%1: %p%").arg(title));
							}
						},
						Qt::QueuedConnection);
					return !*cancel;
				},
				&error);
			if (!ok) {
				break;
			}
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, cancel, ok, error]() {
				const bool cancelled = *cancel;
				*cancel = true;
				if (!self) {
					return;
				}
				self->speechProgress->setVisible(false);
				self->UpdateModelState();
				if (!ok && !cancelled) {
					QMessageBox::warning(self, T("Lucida.Settings.Title"),
							     T("Lucida.Settings.Speech.DownloadFailed").arg(error));
				}
			},
			Qt::QueuedConnection);
	}).detach();
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

	SpeechSettings &sp = s.speech;
	const bool wasEnabled = sp.enabled;
	sp.enabled = speechEnabled->isChecked();
	sp.model = speechModel->currentData().toString();
	sp.language = speechLanguage->currentData().toString();
	sp.when = (SpeechSettings::When)speechWhen->currentData().toInt();
	sp.useGpu = speechGpu->isChecked();
	sp.me = speechMe->isChecked();
	sp.teamSpeak = speechTeamSpeak->isChecked();
	sp.game = speechGame->isChecked();
	sp.prompt = speechPrompt->text().trimmed();
	if (speechIncludeOlder) {
		sp.since = 0.0;
	} else if (sp.enabled && !wasEnabled && sp.since <= 0.0) {
		/* Turning it on starts from now, not the whole loop folder */
		sp.since = QDateTime::currentSecsSinceEpoch();
	}
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
