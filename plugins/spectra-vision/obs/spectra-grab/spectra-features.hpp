#pragma once

/*
 * Spectra's feature switches, as the plugins see them. Spectra > Features in
 * the main window keeps them in the user config, section SpectraFeatures; a
 * feature is on unless its key is false. Like the frontend
 * (frontend/utility/SpectraFeatures.*), a plugin reads each switch once, so a
 * change takes effect when OBS-Spectra restarts. Keep the names in step.
 *
 * Needs obs-frontend-api; header only, so spectra-grab itself doesn't link it.
 */

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <map>
#include <mutex>
#include <string>

namespace spectra {

/* "Lucida", "Obscura", "Speech", "Browser" or "Streaming" */
inline bool FeatureEnabled(const char *name)
{
	static std::mutex mutex;
	static std::map<std::string, bool> atLaunch;
	std::lock_guard<std::mutex> lock(mutex);
	auto it = atLaunch.find(name);
	if (it != atLaunch.end()) {
		return it->second;
	}
	config_t *config = obs_frontend_get_user_config();
	if (!config) {
		/* too early to know: don't remember a guess */
		return true;
	}
	const bool on = !config_has_user_value(config, "SpectraFeatures", name) ||
			config_get_bool(config, "SpectraFeatures", name);
	atLaunch.emplace(name, on);
	return on;
}

} // namespace spectra
