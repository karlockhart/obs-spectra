#pragma once

#include "store.hpp"

#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace lucida {

class PrismaClient;

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
	/* Opens `before` / `after` seconds either side of a spot in Spectra's
	 * clip editor; unset where there is none (the standalone viewer) */
	std::function<bool(const VideoSpot &spot, double before, double after)> editClip;
	/* Prisma, for the Cloud toggle: other installs' lines and screenshots;
	 * unset or null without credentials */
	std::function<std::shared_ptr<PrismaClient>()> cloud;
};

} // namespace lucida
