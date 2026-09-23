#pragma once

#include "store.hpp"

#include <QString>

#include <functional>
#include <optional>

namespace lucida {

/* What the log browser needs from where it runs: inside Spectra (the
 * plugin) or on its own (lucida-viewer.exe, next to obs-spectra.exe). Each
 * of them defines these. */

/* Localised UI text */
QString Text(const char *key);
/* A value from the current Spectra profile's basic.ini; empty if unset */
QString ProfileString(const char *section, const char *name);

/* The log to browse and where its lines are in the loop recording */
struct ViewerSource {
	std::function<Store *()> reader;
	std::function<std::optional<VideoSpot>(const LogLine &line)> videoFor;
};

} // namespace lucida
