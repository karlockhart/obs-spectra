#include "SpectraFeatures.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <util/config-file.h>

#include <map>

namespace SpectraFeatures {

static constexpr const char *SECTION = "SpectraFeatures";

const char *Key(Feature feature)
{
	switch (feature) {
	case Feature::Lucida:
		return "Lucida";
	case Feature::Obscura:
		return "Obscura";
	case Feature::Speech:
		return "Speech";
	case Feature::Browser:
		return "Browser";
	case Feature::Streaming:
		return "Streaming";
	}
	return "";
}

bool Available(Feature feature)
{
	switch (feature) {
	case Feature::Lucida:
	case Feature::Obscura:
	case Feature::Speech:
#ifdef SPECTRA_LITE
		return false;
#else
		return true;
#endif
	case Feature::Browser:
#ifdef BROWSER_AVAILABLE
		return true;
#else
		return false;
#endif
	case Feature::Streaming:
#ifdef SPECTRA_ULTRALIGHT
		return false;
#else
		return true;
#endif
	}
	return false;
}

bool Configured(Feature feature)
{
	config_t *config = App() ? App()->GetUserConfig() : nullptr;
	if (!config || !config_has_user_value(config, SECTION, Key(feature))) {
		return true;
	}
	return config_get_bool(config, SECTION, Key(feature));
}

void SetConfigured(Feature feature, bool on)
{
	config_t *config = App()->GetUserConfig();
	config_set_bool(config, SECTION, Key(feature), on);
	config_save_safe(config, "tmp", nullptr);
}

bool Enabled(Feature feature)
{
	if (!Available(feature)) {
		return false;
	}
	if (!App() || !App()->GetUserConfig()) {
		/* too early to know: don't remember a guess */
		return true;
	}
	/* What the session started with: later changes wait for a restart */
	static std::map<Feature, bool> atLaunch;
	auto it = atLaunch.find(feature);
	if (it == atLaunch.end()) {
		it = atLaunch.emplace(feature, Configured(feature)).first;
	}
	return it->second;
}

bool PresetIncludes(Preset preset, Feature feature)
{
	switch (preset) {
	case Preset::Full:
	case Preset::Custom:
		return true;
	case Preset::Lite:
		return feature == Feature::Browser || feature == Feature::Streaming;
	case Preset::Ultralight:
		return false;
	}
	return true;
}

bool PresetAvailable(Preset preset)
{
	switch (preset) {
	case Preset::Full:
		return Available(Feature::Lucida);
	case Preset::Lite:
		return Available(Feature::Streaming) && Available(Feature::Browser);
	case Preset::Ultralight:
		return true;
	case Preset::Custom:
		return AnyAvailable();
	}
	return false;
}

Preset PresetFor(bool (*isOn)(Feature))
{
	for (Preset preset : {Preset::Full, Preset::Lite, Preset::Ultralight}) {
		if (!PresetAvailable(preset)) {
			continue;
		}
		bool matches = true;
		for (Feature feature : All) {
			if (Available(feature) && isOn(feature) != PresetIncludes(preset, feature)) {
				matches = false;
				break;
			}
		}
		if (matches) {
			return preset;
		}
	}
	return Preset::Custom;
}

bool AnyAvailable()
{
	for (Feature feature : All) {
		if (Available(feature)) {
			return true;
		}
	}
	return false;
}

QString Name(Feature feature)
{
	return QTStr((std::string("Spectra.Features.") + Key(feature)).c_str());
}

QString Description(Feature feature)
{
	return QTStr((std::string("Spectra.Features.") + Key(feature) + "Tip").c_str());
}

QString PresetName(Preset preset)
{
	switch (preset) {
	case Preset::Full:
		return QTStr("Spectra.Features.Preset.Full");
	case Preset::Lite:
		return QTStr("Spectra.Features.Preset.Lite");
	case Preset::Ultralight:
		return QTStr("Spectra.Features.Preset.Ultralight");
	case Preset::Custom:
		return QTStr("Spectra.Features.Preset.Custom");
	}
	return QString();
}

} // namespace SpectraFeatures
