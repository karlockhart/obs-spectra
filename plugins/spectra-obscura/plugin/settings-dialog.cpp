#include "settings-dialog.hpp"
#include "obscura-controller.hpp"

#include <obs-module.h>

#include <spectra-censor/censor.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

namespace obscura {

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

QWidget *Page(QFormLayout *form)
{
	QWidget *page = new QWidget();
	page->setLayout(form);
	return page;
}

const std::pair<const char *, const char *> kCropReview[] = {
	{"Obscura.Settings.CropReview.Auto", "auto"},
	{"Obscura.Settings.CropReview.Always", "always"},
	{"Obscura.Settings.CropReview.Never", "never"},
};

const std::pair<const char *, int> kExpiry[] = {
	{"Obscura.Settings.Expiry.Never", 0},   {"Obscura.Settings.Expiry.10m", 600},
	{"Obscura.Settings.Expiry.1h", 3600},   {"Obscura.Settings.Expiry.1d", 86400},
	{"Obscura.Settings.Expiry.1w", 604800}, {"Obscura.Settings.Expiry.1mo", 2592000},
};

} // namespace

SettingsDialog::SettingsDialog(Controller *controller, QWidget *parent) : QDialog(parent), c(controller)
{
	setWindowTitle(T("Obscura.Settings.Title"));
	resize(680, 540);
	QTabWidget *tabs = new QTabWidget();
	tabs->addTab(CaptureTab(), T("Obscura.Settings.Tab.Capture"));
	tabs->addTab(FilesTab(), T("Obscura.Settings.Tab.Files"));
	tabs->addTab(DetectionTab(), T("Obscura.Settings.Tab.Detection"));
	tabs->addTab(LearningTab(), T("Obscura.Settings.Tab.Learning"));
	tabs->addTab(UploadTab(), T("Obscura.Settings.Tab.Upload"));
	tabs->addTab(UpdatesTab(), T("Obscura.Settings.Tab.Updates"));

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
	QVBoxLayout *lay = new QVBoxLayout(this);
	lay->addWidget(tabs);
	lay->addWidget(buttons);
}

QWidget *SettingsDialog::PathEdit(QLineEdit *&edit, const QString &value)
{
	QWidget *w = new QWidget();
	QHBoxLayout *lay = new QHBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);
	edit = new QLineEdit(value);
	QPushButton *browse = new QPushButton(T("Obscura.Settings.Browse"));
	QLineEdit *target = edit;
	connect(browse, &QPushButton::clicked, this, [this, target] {
		QString folder =
			QFileDialog::getExistingDirectory(this, T("Obscura.Settings.ChooseFolder"), target->text());
		if (!folder.isEmpty()) {
			target->setText(QDir::toNativeSeparators(folder));
		}
	});
	lay->addWidget(edit, 1);
	lay->addWidget(browse);
	return w;
}

QWidget *SettingsDialog::RegionEdit(int which, const spectra::Region &r)
{
	QWidget *w = new QWidget();
	QHBoxLayout *lay = new QHBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);
	const char *labels[4] = {"L", "T", "R", "B"};
	const double values[4] = {r.left, r.top, r.right, r.bottom};
	for (int i = 0; i < 4; i++) {
		QDoubleSpinBox *spin = new QDoubleSpinBox();
		spin->setDecimals(3);
		spin->setRange(0.0, 1.0);
		spin->setSingleStep(0.005);
		spin->setValue(values[i]);
		lay->addWidget(new QLabel(QString::fromLatin1(labels[i])));
		lay->addWidget(spin);
		region[which][i] = spin;
	}
	return w;
}

spectra::Region SettingsDialog::RegionValue(int which) const
{
	auto v = [&](int i) {
		return std::round(region[which][i]->value() * 10000.0) / 10000.0;
	};
	return {v(0), v(1), v(2), v(3)};
}

QWidget *SettingsDialog::CaptureTab()
{
	const Config &cfg = c->Cfg();
	QFormLayout *form = new QFormLayout();
	form->addRow(Note(T("Obscura.Settings.HotkeysNote")));
	cropReview = new QComboBox();
	for (const auto &[key, value] : kCropReview) {
		cropReview->addItem(T(key), QString::fromLatin1(value));
	}
	cropReview->setCurrentIndex(std::max(0, cropReview->findData(cfg.cropReview)));
	process = new QLineEdit(cfg.targetProcess);
	title = new QLineEdit(cfg.targetTitle);
	form->addRow(T("Obscura.Settings.CropReview"), cropReview);
	form->addRow(T("Obscura.Settings.Process"), process);
	form->addRow(T("Obscura.Settings.WindowTitle"), title);
	form->addRow(Note(T("Obscura.Settings.CaptureNote")));
	return Page(form);
}

QWidget *SettingsDialog::FilesTab()
{
	const Config &cfg = c->Cfg();
	QFormLayout *form = new QFormLayout();
	watchEnabled = Check("Obscura.Settings.WatchEnabled", cfg.watchEnabled);
	watchList = new QListWidget();
	watchList->addItems(cfg.watchFolders);
	watchList->setMaximumHeight(90);
	QPushButton *add = new QPushButton(T("Obscura.Settings.Add"));
	QPushButton *remove = new QPushButton(T("Obscura.Settings.Remove"));
	connect(add, &QPushButton::clicked, this, [this] {
		QString folder = QFileDialog::getExistingDirectory(this, T("Obscura.Settings.WatchFolder"));
		if (!folder.isEmpty()) {
			watchList->addItem(QDir::toNativeSeparators(folder));
		}
	});
	connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(watchList->selectedItems()); });
	QHBoxLayout *row = new QHBoxLayout();
	row->addWidget(add);
	row->addWidget(remove);
	row->addStretch();
	moveWatched = Check("Obscura.Settings.MoveWatched", cfg.moveWatchedOriginals);
	openedSrc = Check("Obscura.Settings.OpenedToSource", cfg.openedToSourceFolder);
	keepOriginals = Check("Obscura.Settings.KeepOriginals", cfg.keepOriginals);
	prefix = new QLineEdit(cfg.filenamePrefix);
	tz = new QComboBox();
	tz->addItems({QStringLiteral("local"), QStringLiteral("utc")});
	tz->setCurrentText(cfg.timestampTz);
	form->addRow(watchEnabled);
	form->addRow(watchList);
	form->addRow(row);
	form->addRow(moveWatched);
	form->addRow(T("Obscura.Settings.OutputFolder"), PathEdit(outputDir, cfg.outputDir));
	form->addRow(openedSrc);
	form->addRow(keepOriginals);
	form->addRow(T("Obscura.Settings.OriginalsFolder"), PathEdit(originalsDir, cfg.originalsDir));
	form->addRow(T("Obscura.Settings.Prefix"), prefix);
	form->addRow(T("Obscura.Settings.Timezone"), tz);
	return Page(form);
}

QWidget *SettingsDialog::DetectionTab()
{
	const Config &cfg = c->Cfg();
	QFormLayout *form = new QFormLayout();
	fill = new QLineEdit(cfg.fill);
	fill->setToolTip(T("Obscura.Settings.Fill.Tip"));
	form->addRow(Note(T("Obscura.Settings.RegionsNote")));
	form->addRow(T("Obscura.Settings.ChatRegion"), RegionEdit(0, cfg.chatRegion));
	form->addRow(T("Obscura.Settings.HudRegion"), RegionEdit(1, cfg.hudRegion));
	form->addRow(T("Obscura.Settings.Fill"), fill);
	return Page(form);
}

QWidget *SettingsDialog::LearningTab()
{
	const Config &cfg = c->Cfg();
	QFormLayout *form = new QFormLayout();
	seeds = new QLineEdit(cfg.seedSensitive.join(", "));
	seeds->setToolTip(T("Obscura.Settings.Seeds.Tip"));
	autoApply = Check("Obscura.Settings.AutoApply", cfg.autoApply);
	minScreens = new QSpinBox();
	minScreens->setRange(1, 1000);
	minScreens->setValue(cfg.autoMinScreens);
	confidence = new QDoubleSpinBox();
	confidence->setDecimals(2);
	confidence->setRange(0.5, 1.0);
	confidence->setSingleStep(0.01);
	confidence->setValue(cfg.autoConfidence);
	useInstalled = Check("Obscura.Settings.UseInstalled", cfg.useInstalledDefinitions);
	useInstalled->setToolTip(T("Obscura.Settings.UseInstalled.Tip"));
	stats = new QLabel();
	stats->setWordWrap(true);
	QPushButton *reset = new QPushButton(T("Obscura.Settings.Forget"));
	connect(reset, &QPushButton::clicked, this, [this] {
		if (QMessageBox::question(this, T("Obscura.Title"), T("Obscura.Settings.Forget.Confirm")) ==
		    QMessageBox::Yes) {
			c->GetLearner()->Reset();
			UpdateStats();
		}
	});
	QPushButton *exportDefs = new QPushButton(T("Obscura.Settings.Export"));
	connect(exportDefs, &QPushButton::clicked, this, [this] {
		const int next = c->GetLearner()->InstalledVersion() + 1;
		const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
						  .filePath(QStringLiteral("obscura-defs-v%1.obx").arg(next));
		const QString path = QFileDialog::getSaveFileName(this, T("Obscura.Settings.Export"), suggested,
								  T("Obscura.Settings.ExportFilter"));
		if (path.isEmpty()) {
			return;
		}
		QString message;
		if (c->ExportDefinitions(path, &message)) {
			QMessageBox::information(this, T("Obscura.Title"), message);
		} else {
			QMessageBox::warning(this, T("Obscura.Title"), message);
		}
	});
	form->addRow(T("Obscura.Settings.Seeds"), seeds);
	form->addRow(autoApply);
	form->addRow(T("Obscura.Settings.MinScreens"), minScreens);
	form->addRow(T("Obscura.Settings.Confidence"), confidence);
	form->addRow(useInstalled);
	form->addRow(stats);
	form->addRow(reset);
	form->addRow(exportDefs);
	UpdateStats();
	return Page(form);
}

void SettingsDialog::UpdateStats()
{
	Learner *l = c->GetLearner();
	const QStringList channels = l->ChannelsSeen();
	stats->setText(T("Obscura.Settings.Stats")
			       .arg(l->Screens())
			       .arg(l->Samples())
			       .arg(channels.isEmpty() ? T("Obscura.Settings.NoneYet") : channels.join(", ")));
}

QWidget *SettingsDialog::UploadTab()
{
	const Config &cfg = c->Cfg();
	QFormLayout *form = new QFormLayout();
	imgbbKeyInitial = spectra::censor::LoadImgbbKey().value_or(QString());
	imgbbKey = new QLineEdit(imgbbKeyInitial);
	imgbbKey->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	imgbbKey->setPlaceholderText(T("Obscura.Settings.ImgbbKey.Placeholder"));
	imgbbExpiry = new QComboBox();
	for (const auto &[key, seconds] : kExpiry) {
		imgbbExpiry->addItem(T(key), seconds);
	}
	imgbbExpiry->setCurrentIndex(std::max(0, imgbbExpiry->findData(cfg.imgbbExpiration)));
	imgbbCopy = Check("Obscura.Settings.CopyLink", cfg.imgbbCopyLink);
	imgbbOpen = Check("Obscura.Settings.OpenLink", cfg.imgbbOpenLink);
	form->addRow(T("Obscura.Settings.ImgbbKey"), imgbbKey);
	form->addRow(T("Obscura.Settings.Expiry"), imgbbExpiry);
	form->addRow(imgbbCopy);
	form->addRow(imgbbOpen);
	form->addRow(Note(T("Obscura.Settings.UploadNote")));
	return Page(form);
}

QWidget *SettingsDialog::UpdatesTab()
{
	const Config &cfg = c->Cfg();
	QFormLayout *form = new QFormLayout();
	distRepo = new QLineEdit(cfg.distRepo);
	distRepo->setToolTip(T("Obscura.Settings.DistRepo.Tip"));
	autoDefs = Check("Obscura.Settings.AutoDefs", cfg.autoUpdateDefs);
	prereleaseDefs = Check("Obscura.Settings.PrereleaseDefs", cfg.includePrereleaseDefs);
	QPushButton *check = new QPushButton(T("Obscura.Settings.CheckNow"));
	connect(check, &QPushButton::clicked, this, [this] { c->CheckUpdates(true); });
	form->addRow(new QLabel(T("Obscura.Settings.DefsVersion").arg(c->GetLearner()->InstalledVersion())));
	form->addRow(T("Obscura.Settings.DistRepo"), distRepo);
	form->addRow(autoDefs);
	form->addRow(prereleaseDefs);
	form->addRow(check);
	form->addRow(Note(T("Obscura.Settings.UpdatesNote")));
	return Page(form);
}

void SettingsDialog::accept()
{
	QRegularExpression re(process->text());
	if (!re.isValid()) {
		QMessageBox::warning(this, T("Obscura.Title"), T("Obscura.Settings.BadRegex").arg(re.errorString()));
		return;
	}
	const QString f = fill->text().trimmed().toLower();
	static const QRegularExpression hex(QStringLiteral("^#[0-9a-f]{6}$"));
	if (f != "auto" && !hex.match(f).hasMatch()) {
		QMessageBox::warning(this, T("Obscura.Title"), T("Obscura.Settings.BadFill"));
		return;
	}
	Config &cfg = c->Cfg();
	cfg.targetProcess = process->text();
	cfg.targetTitle = title->text();
	cfg.cropReview = cropReview->currentData().toString();
	cfg.watchEnabled = watchEnabled->isChecked();
	cfg.watchFolders.clear();
	for (int i = 0; i < watchList->count(); i++) {
		cfg.watchFolders << watchList->item(i)->text();
	}
	cfg.moveWatchedOriginals = moveWatched->isChecked();
	cfg.outputDir = outputDir->text().trimmed();
	cfg.originalsDir = originalsDir->text().trimmed();
	cfg.openedToSourceFolder = openedSrc->isChecked();
	cfg.keepOriginals = keepOriginals->isChecked();
	cfg.filenamePrefix = prefix->text().trimmed().isEmpty() ? QStringLiteral("fivem") : prefix->text().trimmed();
	cfg.timestampTz = tz->currentText();
	cfg.chatRegion = RegionValue(0);
	cfg.hudRegion = RegionValue(1);
	cfg.fill = f;
	cfg.seedSensitive.clear();
	for (const QString &s : seeds->text().split(',')) {
		if (!s.trimmed().isEmpty()) {
			cfg.seedSensitive << s.trimmed().toLower();
		}
	}
	cfg.autoApply = autoApply->isChecked();
	cfg.autoMinScreens = minScreens->value();
	cfg.autoConfidence = std::round(confidence->value() * 100.0) / 100.0;
	cfg.useInstalledDefinitions = useInstalled->isChecked();
	cfg.distRepo = distRepo->text().trimmed();
	cfg.autoUpdateDefs = autoDefs->isChecked();
	cfg.includePrereleaseDefs = prereleaseDefs->isChecked();
	cfg.imgbbExpiration = imgbbExpiry->currentData().toInt();
	cfg.imgbbCopyLink = imgbbCopy->isChecked();
	cfg.imgbbOpenLink = imgbbOpen->isChecked();
	const QString key = imgbbKey->text().trimmed();
	if (key != imgbbKeyInitial && !spectra::censor::SaveImgbbKey(key)) {
		QMessageBox::warning(this, T("Obscura.Title"), T("Obscura.Settings.KeyStoreFailed"));
	}
	QDialog::accept();
}

} // namespace obscura
