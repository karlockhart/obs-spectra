#include "StarlingLink.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define INPUT_ID "wasapi_input_capture"
/* Private-settings tags; CAPTURE_TAG matches LoopCapture's managed sources */
#define CAPTURE_TAG "spectra_capture"
#define TAG_STARLING "starling"
#define MUTED_FLAG "spectra_starling_muted"

static constexpr int MIC_CHANNEL = 3;
/* Channels 1-6 are the saved desktop/mic devices; 7 is never saved, so the
 * capture never outlives the Starling session in the scene collection. */
static constexpr int STARLING_CHANNEL = 7;
static constexpr int POLL_MS = 2000;

namespace {

bool SessionRunning()
{
#ifdef _WIN32
	HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\StarlingSessionActive");
	if (!mutex) {
		return false;
	}
	CloseHandle(mutex);
	return true;
#else
	return false;
#endif
}

/* Output device the running session opened, e.g.
 * "CABLE In 16ch (VB-Audio Virtual Cable)" */
QString SessionOutputDevice()
{
	QFile file(qEnvironmentVariable("LOCALAPPDATA") + "/Starling/session.json");
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	return QJsonDocument::fromJson(file.readAll()).object().value("output_device").toString();
}

/* The recording end of a VB-Audio cable, by name prefix: "CABLE Input" and
 * "CABLE In 16ch" come out of "CABLE Output", "CABLE-A Input" out of
 * "CABLE-A Output". Empty for anything else. */
QString CablePrefixFor(const QString &renderDevice)
{
	static const QRegularExpression re("^(CABLE(?:-[A-Z])?) (?:Input|In \\d+ch)\\b",
					   QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch match = re.match(renderDevice);
	return match.hasMatch() ? match.captured(1) + " Output" : QString();
}

bool FindInputDevice(const QString &prefix, QString &id, QString &name)
{
	obs_properties_t *props = obs_get_source_properties(INPUT_ID);
	obs_property_t *devices = props ? obs_properties_get(props, "device_id") : nullptr;
	bool found = false;
	for (size_t i = 0; devices && i < obs_property_list_item_count(devices); i++) {
		QString itemName = QString::fromUtf8(obs_property_list_item_name(devices, i));
		if (itemName.startsWith(prefix, Qt::CaseInsensitive)) {
			id = QString::fromUtf8(obs_property_list_item_string(devices, i));
			name = itemName;
			found = true;
			break;
		}
	}
	obs_properties_destroy(props);
	return found;
}

QString DeviceIdOf(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	return QString::fromUtf8(obs_data_get_string(settings, "device_id"));
}

QString UniqueSourceName(const QString &base)
{
	QString name = base;
	for (int n = 2;; n++) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(QT_TO_UTF8(name));
		if (!existing) {
			return name;
		}
		name = QStringLiteral("%1 %2").arg(base).arg(n);
	}
}

} // namespace

StarlingLink::StarlingLink(OBSBasic *main) : QObject(main)
{
	/* Polls even while disabled, so a mic muted before a crash is restored
	 * once the scene collection has loaded */
	timer.setInterval(POLL_MS);
	connect(&timer, &QTimer::timeout, this, &StarlingLink::Poll);
	timer.start();
}

void StarlingLink::SetEnabled(bool enable)
{
	enabled = enable;
	Poll();
}

void StarlingLink::Poll()
{
	if (!enabled) {
		Release();
		return;
	}
	if (!SessionRunning()) {
		lastSkipReason.clear();
		Release();
		return;
	}

	QString output = SessionOutputDevice();
	QString prefix = CablePrefixFor(output);
	QString deviceId, deviceName;
	if (prefix.isEmpty()) {
		Skip(QStringLiteral("Starling plays into '%1', not a VB-Audio cable").arg(output));
		return;
	}
	if (!FindInputDevice(prefix, deviceId, deviceName)) {
		Skip(QStringLiteral("no '%1' recording device found").arg(prefix));
		return;
	}

	OBSSourceAutoRelease mic = obs_get_output_source(MIC_CHANNEL);
	if (mic && DeviceIdOf(mic) == deviceId) {
		Skip(QStringLiteral("the microphone already records '%1'").arg(deviceName));
		return;
	}

	lastSkipReason.clear();
	Engage(deviceId, deviceName);
}

void StarlingLink::Skip(const QString &reason)
{
	Release();
	if (reason != lastSkipReason) {
		blog(LOG_INFO, "[Spectra] Starling session running, voice not linked: %s", QT_TO_UTF8(reason));
		lastSkipReason = reason;
	}
}

void StarlingLink::Engage(const QString &deviceId, const QString &deviceName)
{
	/* The channel is cleared when the scene collection changes, and the
	 * device can change between sessions, so this is checked every poll. */
	OBSSourceAutoRelease voice = obs_get_output_source(STARLING_CHANNEL);
	if (!voice || strcmp(obs_source_get_id(voice), INPUT_ID) != 0 || DeviceIdOf(voice) != deviceId) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "device_id", QT_TO_UTF8(deviceId));
		QString name = UniqueSourceName(QTStr("Spectra.Starling.SourceName"));
		voice = obs_source_create(INPUT_ID, QT_TO_UTF8(name), settings, nullptr);
		OBSDataAutoRelease priv = obs_source_get_private_settings(voice);
		obs_data_set_string(priv, CAPTURE_TAG, TAG_STARLING);
		obs_set_output_source(STARLING_CHANNEL, voice);
		blog(LOG_INFO, "[Spectra] Starling voice linked: recording '%s'", QT_TO_UTF8(deviceName));
	}

	/* Mute each microphone source once; one the user unmutes afterwards
	 * stays unmuted, and one that was already muted is not ours to
	 * unmute later. */
	OBSSourceAutoRelease mic = obs_get_output_source(MIC_CHANNEL);
	QString micUuid = mic ? QString::fromUtf8(obs_source_get_uuid(mic)) : QString();
	if (mic && micUuid != handledMicUuid) {
		handledMicUuid = micUuid;
		if (!obs_source_muted(mic)) {
			OBSDataAutoRelease priv = obs_source_get_private_settings(mic);
			obs_data_set_bool(priv, MUTED_FLAG, true);
			obs_source_set_muted(mic, true);
			blog(LOG_INFO, "[Spectra] Microphone '%s' muted while Starling converts",
			     obs_source_get_name(mic));
		}
	}

	if (!engaged) {
		engaged = true;
		emit engagedChanged(true);
	}
}

void StarlingLink::Release()
{
	OBSSourceAutoRelease voice = obs_get_output_source(STARLING_CHANNEL);
	if (voice) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(voice);
		if (strcmp(obs_data_get_string(priv, CAPTURE_TAG), TAG_STARLING) == 0) {
			obs_set_output_source(STARLING_CHANNEL, nullptr);
			blog(LOG_INFO, "[Spectra] Starling voice unlinked");
		}
	}

	/* Also restores a mic muted before a crash: the flag is saved with the
	 * source in the scene collection. */
	OBSSourceAutoRelease mic = obs_get_output_source(MIC_CHANNEL);
	if (mic) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(mic);
		if (obs_data_get_bool(priv, MUTED_FLAG)) {
			obs_data_erase(priv, MUTED_FLAG);
			obs_source_set_muted(mic, false);
			blog(LOG_INFO, "[Spectra] Microphone '%s' unmuted, Starling stopped", obs_source_get_name(mic));
		}
	}
	handledMicUuid.clear();

	if (engaged) {
		engaged = false;
		emit engagedChanged(false);
	}
}
