#include "AutoConfigSpectraPage.hpp"
#include "AutoConfig.hpp"

#include <components/SpectraAppPicker.hpp>
#include <components/SpectraHotkeyEdit.hpp>
#include <utility/LoopRecorder.hpp>
#include <utility/SpectraDefaults.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>

#include "moc_AutoConfigSpectraPage.cpp"

#define wiz reinterpret_cast<AutoConfig *>(wizard())

static const char *spectraFolders[] = {"Loop", "Clips", "Screenshots", "Lucida", "Obscura"};

static QString DefaultBaseFolder()
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("Spectra");
}

AutoConfigSpectraPage::AutoConfigSpectraPage(QWidget *parent) : QWizardPage(parent)
{
	setTitle(QTStr("Spectra.Wizard.Title"));
	setSubTitle(QTStr("Spectra.Wizard.SubTitle"));

	config_t *config = OBSBasic::Get()->Config();

	QString base = QString::fromUtf8(config_get_string(config, "Spectra", "BaseFolder"));
	baseFolder = new QLineEdit(base.isEmpty() ? QDir::toNativeSeparators(DefaultBaseFolder()) : base);
	QPushButton *browse = new QPushButton(QTStr("Browse"));
	connect(browse, &QPushButton::clicked, this, [this]() {
		QString dir =
			QFileDialog::getExistingDirectory(this, QTStr("Spectra.Wizard.BaseFolder"), baseFolder->text());
		if (!dir.isEmpty()) {
			baseFolder->setText(QDir::toNativeSeparators(dir));
		}
	});
	connect(baseFolder, &QLineEdit::textChanged, this, &AutoConfigSpectraPage::UpdatePreview);

	QHBoxLayout *folderRow = new QHBoxLayout();
	folderRow->setContentsMargins(0, 0, 0, 0);
	folderRow->addWidget(baseFolder);
	folderRow->addWidget(browse);

	folderPreview = new QLabel();
	folderPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);

	processes = new SpectraAppPicker();
	processes->SetPatterns(QString::fromUtf8(config_get_string(config, "SpectraLoop", "Processes")));
	processes->SetAnyFullscreen(config_get_bool(config, "SpectraLoop", "AnyFullscreen"));

	quotaGB = new QSpinBox();
	quotaGB->setRange(1, 100000);
	quotaGB->setSuffix(" GB");
	quotaGB->setValue((int)config_get_uint(config, "SpectraLoop", "QuotaGB"));

	clipSec = new QSpinBox();
	clipSec->setRange(5, 24 * 3600);
	clipSec->setSuffix(" s");
	clipSec->setValue(std::max((int)config_get_int(config, "SpectraLoop", "ClipSec"), 5));
	clipSec->setToolTip(QTStr("Spectra.Loop.Settings.ClipLengthTip"));

	resolution = new QComboBox();
	SpectraDefaults::FillResolutionCombo(resolution, (int)config_get_int(config, "SpectraLoop", "CanvasCX"),
					     (int)config_get_int(config, "SpectraLoop", "CanvasCY"));
	resolution->setToolTip(QTStr("Spectra.Video.ResolutionTip"));
	quality = new QComboBox();
	SpectraDefaults::FillQualityCombo(quality,
					  QString::fromUtf8(config_get_string(config, "SpectraLoop", "Quality")));
	quality->setToolTip(QTStr("Spectra.Video.QualityTip"));

	autoStart = new QCheckBox(QTStr("Spectra.Loop.Settings.AutoStart"));
	autoStart->setChecked(config_get_bool(config, "SpectraLoop", "AutoStart"));
	autoCapture = new QCheckBox(QTStr("Spectra.Loop.Settings.AutoCapture"));
	autoCapture->setChecked(config_get_bool(config, "SpectraLoop", "AutoCapture"));
	autoCapture->setToolTip(QTStr("Spectra.Loop.Settings.AutoCaptureTip"));

	QFormLayout *form = new QFormLayout();
	form->addRow(QTStr("Spectra.Wizard.BaseFolder"), folderRow);
	form->addRow(QString(), folderPreview);
	form->addRow(QTStr("Spectra.Loop.Settings.Processes"), processes);
	form->addRow(QTStr("Spectra.Loop.Settings.Quota"), quotaGB);
	form->addRow(QTStr("Spectra.Loop.Settings.ClipLength"), clipSec);
	form->addRow(QTStr("Spectra.Video.Resolution"), resolution);
	form->addRow(QTStr("Spectra.Video.Quality"), quality);
	form->addRow(QString(), autoStart);
	form->addRow(QString(), autoCapture);

	QGroupBox *shortcutGroup = new QGroupBox(QTStr("Spectra.Hotkey.Shortcuts"));
	auto *shortcutForm = new QFormLayout(shortcutGroup);
	QLabel *shortcutNote = new QLabel(QTStr("Spectra.Hotkey.ShortcutsNote"));
	shortcutNote->setWordWrap(true);
	shortcutForm->addRow(shortcutNote);
	shortcuts = SpectraHotkeyEdit::AddShortcutRows(shortcutForm);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(shortcutGroup);
	layout->addStretch();

	UpdatePreview();
}

void AutoConfigSpectraPage::UpdatePreview()
{
	QDir base(baseFolder->text().trimmed());
	QStringList lines;
	for (const char *folder : spectraFolders) {
		lines << QDir::toNativeSeparators(base.filePath(folder));
	}
	folderPreview->setText(QTStr("Spectra.Wizard.WillCreate") + "\n" + lines.join("\n"));
}

int AutoConfigSpectraPage::nextId() const
{
	return wiz->type == AutoConfig::Type::VirtualCam ? AutoConfig::TestPage : AutoConfig::VideoPage;
}

bool AutoConfigSpectraPage::validatePage()
{
	int cx = 0, cy = 0;
	if (!SpectraDefaults::ParseResolution(resolution->currentText(), cx, cy)) {
		OBSMessageBox::warning(this, QTStr("Spectra.Wizard.Title"), QTStr("Spectra.Video.InvalidResolution"));
		return false;
	}

	QString base = baseFolder->text().trimmed();
	if (base.isEmpty() || !QDir().mkpath(base)) {
		OBSMessageBox::warning(this, QTStr("Spectra.Wizard.Title"),
				       QTStr("Spectra.Loop.Error.Folder").arg(base));
		return false;
	}
	Save();
	return true;
}

void AutoConfigSpectraPage::Save()
{
	OBSBasic *main = OBSBasic::Get();
	config_t *config = main->Config();

	QDir base(QDir::cleanPath(QDir::fromNativeSeparators(baseFolder->text().trimmed())));
	auto folder = [&](const char *name) {
		QString path = base.filePath(name);
		QDir().mkpath(path);
		return QDir::toNativeSeparators(path);
	};

	config_set_string(config, "Spectra", "BaseFolder", QT_TO_UTF8(QDir::toNativeSeparators(base.path())));
	config_set_string(config, "Spectra", "ScreenshotsPath", QT_TO_UTF8(folder("Screenshots")));
	config_set_string(config, "Spectra", "LucidaPath", QT_TO_UTF8(folder("Lucida")));
	config_set_string(config, "Spectra", "ObscuraPath", QT_TO_UTF8(folder("Obscura")));

	config_set_string(config, "SpectraLoop", "Path", QT_TO_UTF8(folder("Loop")));
	config_set_string(config, "SpectraLoop", "ClipsPath", QT_TO_UTF8(folder("Clips")));
	config_set_string(config, "SpectraLoop", "Processes", QT_TO_UTF8(processes->Patterns()));
	config_set_bool(config, "SpectraLoop", "AnyFullscreen", processes->AnyFullscreen());
	config_set_uint(config, "SpectraLoop", "QuotaGB", (uint64_t)quotaGB->value());
	config_set_int(config, "SpectraLoop", "ClipSec", clipSec->value());
	for (SpectraHotkeyEdit *shortcut : shortcuts) {
		shortcut->Save(config);
	}
	config_set_bool(config, "SpectraLoop", "AutoStart", autoStart->isChecked());
	config_set_bool(config, "SpectraLoop", "AutoCapture", autoCapture->isChecked());

	int cx = SpectraDefaults::DefaultCanvasCX, cy = SpectraDefaults::DefaultCanvasCY;
	SpectraDefaults::ParseResolution(resolution->currentText(), cx, cy);
	config_set_int(config, "SpectraLoop", "CanvasCX", cx);
	config_set_int(config, "SpectraLoop", "CanvasCY", cy);
	config_set_string(config, "SpectraLoop", "Quality", QT_TO_UTF8(quality->currentData().toString()));
	config_save_safe(config, "tmp", nullptr);

	blog(LOG_INFO, "[Spectra] Setup wizard: base folder '%s'", QT_TO_UTF8(base.path()));

	if (LoopRecorder *recorder = main->GetLoopRecorder()) {
		recorder->SettingsChanged();
	}
}
