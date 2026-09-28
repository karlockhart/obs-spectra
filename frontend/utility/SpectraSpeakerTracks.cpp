#include "SpectraSpeakerTracks.hpp"

#include <obs.hpp>

#include <QString>

#include <set>

namespace SpectraSpeakerTracks {

namespace {

/* Output channels 1-2 are desktop audio, 3-6 the microphone/aux devices;
 * Spectra puts the Starling voice on 7 */
constexpr int kFirstChannel = 1;
constexpr int kFirstMicChannel = 3;
constexpr int kLastMicChannel = 7;

bool IsTeamSpeak(obs_source_t *source)
{
	if (strcmp(obs_source_get_unversioned_id(source), "wasapi_process_output_capture") != 0) {
		return false;
	}
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	/* "title:class:exe" */
	const QString window = QString::fromUtf8(obs_data_get_string(settings, "window")).toLower();
	return window.endsWith(QStringLiteral("ts3client_win64.exe")) ||
	       window.endsWith(QStringLiteral("ts3client_win32.exe")) ||
	       window.endsWith(QStringLiteral("teamspeak.exe"));
}

void Route(obs_source_t *source, Speaker speaker)
{
	if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
		return;
	}
	const uint32_t current = obs_source_get_audio_mixers(source);
	uint32_t wanted = (current & ~(kLoopMixers & ~1u)) | 1u;
	if (speaker != Speaker::Mix) {
		wanted |= 1u << (int)speaker;
	}
	if (wanted != current) {
		obs_source_set_audio_mixers(source, wanted);
	}
}

} // namespace

bool Enabled(config_t *profile)
{
	return config_get_bool(profile, "SpectraLoop", "SpeakerTracks");
}

void Apply()
{
	std::set<obs_source_t *> global;
	for (int channel = kFirstChannel; channel <= kLastMicChannel; channel++) {
		OBSSourceAutoRelease source = obs_get_output_source(channel);
		if (source) {
			global.insert(source.Get());
			Route(source, channel >= kFirstMicChannel ? Speaker::Me : Speaker::Game);
		}
	}

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto &global = *static_cast<std::set<obs_source_t *> *>(param);
			if (global.count(source)) {
				return true;
			}
			/* Everything that isn't TeamSpeak counts as the game */
			Route(source, IsTeamSpeak(source) ? Speaker::TeamSpeak : Speaker::Game);
			return true;
		},
		&global);
}

const char *Label(Speaker speaker)
{
	switch (speaker) {
	case Speaker::Me:
		return "me";
	case Speaker::TeamSpeak:
		return "teamspeak";
	case Speaker::Game:
		return "game";
	default:
		return "";
	}
}

} // namespace SpectraSpeakerTracks
