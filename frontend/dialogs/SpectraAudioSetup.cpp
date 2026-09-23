#include "SpectraAudioSetup.hpp"

#include <components/SpectraAppPicker.hpp>
#include <settings/OBSHotkeyEdit.hpp>
#include <utility/SpectraDefaults.hpp>
#include <utility/SpectraGamepad.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

#include "moc_SpectraAudioSetup.cpp"

#define AUDIO_SECTION "SpectraAudio"
#define APP_AUDIO_ID "wasapi_process_output_capture"

static constexpr int DESKTOP_CHANNEL = 1;
static constexpr int MIC_CHANNEL = 3;
static constexpr int GAMEPAD_LISTEN_MS = 30;
static constexpr int GAMEPAD_LISTEN_POLLS = 10000 / GAMEPAD_LISTEN_MS;

enum KeyRoles {
	KeyTypeRole = Qt::UserRole,
	KeyModifiersRole,
	KeyCodeRole,
	GamepadButtonRole,
};

enum KeyType {
	KeyboardKey,
	GamepadKey,
};

/* The executable part of win-capture's "title:class:exe" window setting */
static QString WindowExe(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	QString exe = QString::fromUtf8(obs_data_get_string(settings, "window")).section(':', 2);
	return exe.replace("#3A", ":").replace("#22", "#");
}

struct AppAudioItem {
	OBSSceneItem item;
	QString exe;
};

static std::vector<AppAudioItem> AppAudioItems(obs_scene_t *scene)
{
	std::vector<AppAudioItem> items;
	if (!scene) {
		return items;
	}
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *items = static_cast<std::vector<AppAudioItem> *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (strcmp(obs_source_get_unversioned_id(source), APP_AUDIO_ID) == 0) {
				items->push_back({item, WindowExe(source)});
			}
			return true;
		},
		&items);
	return items;
}

SpectraAudioSetup::SpectraAudioSetup(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setWindowTitle(QTStr("Spectra.Audio.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(560);

	config_t *config = main->Config();
	OBSSourceAutoRelease mic = obs_get_output_source(MIC_CHANNEL);

	/* Microphone */
	micDevice = new QComboBox();
	micDevice->addItem(QTStr("Basic.Settings.Audio.Disabled"), "disabled");
	OBSProperties props = obs_get_source_properties(App()->InputAudioSource());
	obs_property_t *devices = props ? obs_properties_get(props, "device_id") : nullptr;
	for (size_t i = 0, count = devices ? obs_property_list_item_count(devices) : 0; i < count; i++) {
		micDevice->addItem(QString::fromUtf8(obs_property_list_item_name(devices, i)),
				   QString::fromUtf8(obs_property_list_item_string(devices, i)));
	}
	QString currentMic = "disabled";
	if (mic) {
		OBSDataAutoRelease settings = obs_source_get_settings(mic);
		currentMic = QString::fromUtf8(obs_data_get_string(settings, "device_id"));
	} else if (micDevice->findData("default") >= 0) {
		currentMic = "default";
	}
	int micIndex = micDevice->findData(currentMic);
	micDevice->setCurrentIndex(micIndex >= 0 ? micIndex : 0);
	connect(micDevice, &QComboBox::currentIndexChanged, this, &SpectraAudioSetup::UpdateMicState);

	/* Push-to-talk */
	pttGroup = new QGroupBox(QTStr("Spectra.Audio.PushToTalk"));
	pttGroup->setCheckable(true);
	pttGroup->setChecked(mic ? obs_source_push_to_talk_enabled(mic) : true);

	pttKeys = new QListWidget();
	pttKeys->setSelectionMode(QAbstractItemView::ExtendedSelection);
	pttKeys->setMaximumHeight(120);

	keyEdit = new OBSHotkeyEdit();
	keyEdit->setPlaceholderText(QTStr("Spectra.Audio.PressKey"));
	keyEdit->setToolTip(QTStr("Spectra.Audio.PressKeyTip"));
	connect(keyEdit, &OBSHotkeyEdit::KeyChanged, this, [this](obs_key_combination_t key) {
		if (obs_key_combination_is_empty(key)) {
			return;
		}
		AddKey(key);
		QMetaObject::invokeMethod(keyEdit, &OBSHotkeyEdit::ClearKey, Qt::QueuedConnection);
	});

	gamepadButton = new QPushButton(QTStr("Spectra.Audio.AddGamepad"));
	gamepadButton->setAutoDefault(false);
	gamepadButton->setEnabled(SpectraGamepadPTT::Supported());
	if (!SpectraGamepadPTT::Supported()) {
		gamepadButton->setToolTip(QTStr("Spectra.Audio.GamepadUnsupported"));
	}
	connect(gamepadButton, &QPushButton::clicked, this, [this]() {
		if (gamepadListen.isActive()) {
			StopGamepadListen();
		} else {
			StartGamepadListen();
		}
	});
	gamepadListen.setInterval(GAMEPAD_LISTEN_MS);
	connect(&gamepadListen, &QTimer::timeout, this, [this]() {
		QString id = SpectraGamepadPTT::HeldButton();
		if (!id.isEmpty()) {
			AddGamepadButton(id);
			StopGamepadListen();
		} else if (++gamepadListenPolls >= GAMEPAD_LISTEN_POLLS) {
			StopGamepadListen();
		}
	});

	removeKey = new QPushButton(QTStr("Remove"));
	removeKey->setAutoDefault(false);
	removeKey->setEnabled(false);
	connect(removeKey, &QPushButton::clicked, this, [this]() { qDeleteAll(pttKeys->selectedItems()); });
	connect(pttKeys, &QListWidget::itemSelectionChanged, this,
		[this]() { removeKey->setEnabled(!pttKeys->selectedItems().isEmpty()); });

	QPushButton *defaults = new QPushButton(QTStr("Spectra.Audio.DefaultKeys"));
	defaults->setAutoDefault(false);
	connect(defaults, &QPushButton::clicked, this, &SpectraAudioSetup::SetDefaultKeys);

	pttDelay = new QSpinBox();
	pttDelay->setRange(0, 2000);
	pttDelay->setSingleStep(50);
	pttDelay->setSuffix(" ms");
	pttDelay->setValue(mic ? (int)obs_source_get_push_to_talk_delay(mic) : 0);
	pttDelay->setToolTip(QTStr("Spectra.Audio.ReleaseDelayTip"));

	if (mic) {
		for (const obs_key_combination_t &key : SpectraDefaults::GetPushToTalkKeys(mic)) {
			AddKey(key);
		}
	} else {
		SetDefaultKeys();
	}
	if (SpectraGamepadPTT *gamepad = main->GetGamepadPTT()) {
		for (const QString &id : gamepad->Buttons()) {
			AddGamepadButton(id);
		}
	}

	auto *addRow = new QHBoxLayout();
	addRow->addWidget(keyEdit, 1);
	addRow->addWidget(gamepadButton);
	addRow->addWidget(removeKey);

	auto *pttForm = new QFormLayout();
	pttForm->addRow(QTStr("Spectra.Audio.ReleaseDelay"), pttDelay);

	auto *pttLayout = new QVBoxLayout(pttGroup);
	pttLayout->addWidget(pttKeys);
	pttLayout->addLayout(addRow);
	auto *defaultsRow = new QHBoxLayout();
	defaultsRow->addLayout(pttForm);
	defaultsRow->addStretch();
	defaultsRow->addWidget(defaults);
	pttLayout->addLayout(defaultsRow);

	QGroupBox *micGroup = new QGroupBox(QTStr("Spectra.Audio.Microphone"));
	auto *micForm = new QFormLayout();
	micForm->addRow(QTStr("Spectra.Audio.MicDevice"), micDevice);
	auto *micLayout = new QVBoxLayout(micGroup);
	micLayout->addLayout(micForm);
	micLayout->addWidget(pttGroup);

	/* Application audio */
	apps = new SpectraAppPicker(false);
	apps->SetEntryHint(QTStr("Spectra.Audio.Apps.Placeholder"), QTStr("Spectra.Audio.Apps.Tip"));
	QStringList exes;
	for (const AppAudioItem &item : AppAudioItems(main->GetProgramScene())) {
		exes << item.exe;
	}
	if (exes.isEmpty() && !config_get_bool(config, AUDIO_SECTION, "Configured")) {
		QString teamSpeak = SpectraDefaults::FindTeamSpeakExecutable();
		exes << (teamSpeak.isEmpty() ? QStringLiteral("TeamSpeak.exe") : teamSpeak);
	}
	apps->SetPatterns(exes.join(", "));

	OBSSourceAutoRelease desktop = obs_get_output_source(DESKTOP_CHANNEL);
	desktopAudio = new QCheckBox(QTStr("Spectra.Audio.DesktopAudio"));
	desktopAudio->setToolTip(QTStr("Spectra.Audio.DesktopAudioTip"));
	desktopAudio->setChecked(desktop != nullptr);

	QGroupBox *appGroup = new QGroupBox(QTStr("Spectra.Audio.Apps"));
	QLabel *appNote = new QLabel(QTStr("Spectra.Audio.Apps.Note"));
	appNote->setWordWrap(true);
	auto *appLayout = new QVBoxLayout(appGroup);
	appLayout->addWidget(appNote);
	appLayout->addWidget(apps);
	appLayout->addWidget(desktopAudio);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(micGroup);
	layout->addWidget(appGroup);
	layout->addWidget(buttons);

	UpdateMicState();
}

void SpectraAudioSetup::UpdateMicState()
{
	pttGroup->setEnabled(micDevice->currentData().toString() != "disabled");
}

void SpectraAudioSetup::AddKey(obs_key_combination_t key)
{
	for (int i = 0; i < pttKeys->count(); i++) {
		QListWidgetItem *item = pttKeys->item(i);
		if (item->data(KeyTypeRole).toInt() == KeyboardKey &&
		    item->data(KeyModifiersRole).toUInt() == key.modifiers &&
		    item->data(KeyCodeRole).toInt() == key.key) {
			pttKeys->setCurrentRow(i);
			return;
		}
	}

	DStr str;
	obs_key_combination_to_str(key, str);
	QListWidgetItem *item = new QListWidgetItem(QT_UTF8(str));
	item->setData(KeyTypeRole, KeyboardKey);
	item->setData(KeyModifiersRole, key.modifiers);
	item->setData(KeyCodeRole, (int)key.key);
	pttKeys->addItem(item);
}

void SpectraAudioSetup::AddGamepadButton(const QString &id)
{
	for (int i = 0; i < pttKeys->count(); i++) {
		QListWidgetItem *item = pttKeys->item(i);
		if (item->data(KeyTypeRole).toInt() == GamepadKey && item->data(GamepadButtonRole).toString() == id) {
			pttKeys->setCurrentRow(i);
			return;
		}
	}

	QListWidgetItem *item = new QListWidgetItem(SpectraGamepadPTT::ButtonLabel(id));
	item->setData(KeyTypeRole, GamepadKey);
	item->setData(GamepadButtonRole, id);
	pttKeys->addItem(item);
}

void SpectraAudioSetup::SetDefaultKeys()
{
	/* Keyboard keys go back to the defaults; gamepad buttons are kept */
	for (int i = pttKeys->count() - 1; i >= 0; i--) {
		if (pttKeys->item(i)->data(KeyTypeRole).toInt() == KeyboardKey) {
			delete pttKeys->takeItem(i);
		}
	}
	for (const obs_key_combination_t &key : SpectraDefaults::DefaultPushToTalkKeys()) {
		AddKey(key);
	}
}

void SpectraAudioSetup::StartGamepadListen()
{
	gamepadListenPolls = 0;
	gamepadListen.start();
	gamepadButton->setText(QTStr("Spectra.Audio.PressGamepad"));
}

void SpectraAudioSetup::StopGamepadListen()
{
	gamepadListen.stop();
	gamepadButton->setText(QTStr("Spectra.Audio.AddGamepad"));
}

void SpectraAudioSetup::accept()
{
	QStringList exes;
	QStringList wildcards;
	for (const QString &exe : apps->Patterns().split(',', Qt::SkipEmptyParts)) {
		QString name = exe.trimmed();
		if (name.contains(QRegularExpression("[*?]"))) {
			wildcards << name;
		} else if (!name.isEmpty()) {
			exes << name;
		}
	}
	if (!wildcards.isEmpty()) {
		OBSMessageBox::warning(this, QTStr("Spectra.Audio.Title"),
				       QTStr("Spectra.Audio.Apps.NoWildcards").arg(wildcards.join(", ")));
		return;
	}

	StopGamepadListen();
	config_t *config = main->Config();

	/* Desktop audio */
	OBSSourceAutoRelease desktop = obs_get_output_source(DESKTOP_CHANNEL);
	if (desktopAudio->isChecked() != (desktop != nullptr)) {
		main->ResetAudioDevice(App()->OutputAudioSource(), desktopAudio->isChecked() ? "default" : "disabled",
				       Str("Basic.DesktopDevice1"), DESKTOP_CHANNEL);
	}

	/* Microphone and push-to-talk */
	QString micId = micDevice->currentData().toString();
	main->ResetAudioDevice(App()->InputAudioSource(), QT_TO_UTF8(micId), Str("Basic.AuxDevice1"), MIC_CHANNEL);

	std::vector<obs_key_combination_t> keys;
	QStringList gamepadButtons;
	for (int i = 0; i < pttKeys->count(); i++) {
		QListWidgetItem *item = pttKeys->item(i);
		if (item->data(KeyTypeRole).toInt() == GamepadKey) {
			gamepadButtons << item->data(GamepadButtonRole).toString();
		} else {
			keys.push_back(
				{item->data(KeyModifiersRole).toUInt(), (obs_key_t)item->data(KeyCodeRole).toInt()});
		}
	}

	OBSSourceAutoRelease mic = obs_get_output_source(MIC_CHANNEL);
	if (mic) {
		obs_source_enable_push_to_talk(mic, pttGroup->isChecked());
		obs_source_set_push_to_talk_delay(mic, (uint64_t)pttDelay->value());
		SpectraDefaults::SetPushToTalkKeys(mic, keys);
	}
	/* Harmless while push-to-talk is off, so the buttons are kept */
	if (SpectraGamepadPTT *gamepad = main->GetGamepadPTT()) {
		gamepad->SetButtons(gamepadButtons);
	}

	ApplyApplicationAudio(exes);

	config_set_bool(config, AUDIO_SECTION, "Configured", true);
	config_save_safe(config, "tmp", nullptr);

	blog(LOG_INFO,
	     "[Spectra] Audio setup: mic '%s', push-to-talk %s (%zu keys, %lld gamepad buttons), "
	     "desktop audio %s, application audio: %s",
	     QT_TO_UTF8(micId), pttGroup->isChecked() ? "on" : "off", keys.size(), (long long)gamepadButtons.size(),
	     desktopAudio->isChecked() ? "on" : "off", QT_TO_UTF8(exes.join(", ")));

	main->SaveProject();
	QDialog::accept();
}

void SpectraAudioSetup::ApplyApplicationAudio(const QStringList &exes)
{
	OBSScene scene = main->GetProgramScene();
	if (!scene) {
		return;
	}

	QStringList have;
	for (const AppAudioItem &item : AppAudioItems(scene)) {
		if (exes.contains(item.exe, Qt::CaseInsensitive)) {
			have << item.exe;
		} else {
			blog(LOG_INFO, "[Spectra] Removing application audio capture of %s", QT_TO_UTF8(item.exe));
			obs_sceneitem_remove(item.item);
		}
	}

	for (const QString &exe : exes) {
		if (have.contains(exe, Qt::CaseInsensitive)) {
			continue;
		}

		QString baseName = exe;
		baseName.remove(QRegularExpression("\\.exe$", QRegularExpression::CaseInsensitiveOption));

		/* Reuse a capture of the same app that isn't in this scene */
		OBSSourceAutoRelease source;
		QString name = baseName;
		for (int n = 2;; n++) {
			OBSSourceAutoRelease existing = obs_get_source_by_name(QT_TO_UTF8(name));
			if (!existing) {
				break;
			}
			if (strcmp(obs_source_get_unversioned_id(existing), APP_AUDIO_ID) == 0 &&
			    WindowExe(existing).compare(exe, Qt::CaseInsensitive) == 0) {
				source = obs_source_get_ref(existing);
				break;
			}
			name = QStringLiteral("%1 %2").arg(baseName).arg(n);
		}

		if (!source) {
			OBSDataAutoRelease settings = obs_data_create();
			/* "title:class:exe", matched by executable */
			obs_data_set_string(settings, "window", QT_TO_UTF8(QString("::" + exe)));
			obs_data_set_int(settings, "priority", 2 /* WINDOW_PRIORITY_EXE */);
			source = obs_source_create(APP_AUDIO_ID, QT_TO_UTF8(name), settings, nullptr);
		}
		if (source) {
			obs_scene_add(scene, source);
			blog(LOG_INFO, "[Spectra] Added application audio capture of %s", QT_TO_UTF8(exe));
		}
	}
}
