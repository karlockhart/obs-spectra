#pragma once

/*
 * Where a non-portable OBS-Spectra keeps its settings: %USERPROFILE%\Spectra
 * on Windows (profiles, scene collections, plugin settings, logs and speech
 * models, under obs-studio\ as in a portable copy's config\). Earlier versions
 * used %APPDATA%\OBS-Spectra; MigrateConfig() moves that folder here once.
 * Lucida Viewer finds the same folder (lucida-viewer-main.cpp ConfigRoot).
 *
 * A portable copy keeps using config\ next to the program.
 */

#include <string>

namespace SpectraConfigLocation {

/* The settings folder, with forward slashes and no trailing one; empty if
 * the user profile can't be found */
const std::string &Root();

/* Moves %APPDATA%\OBS-Spectra to Root() when Root() holds no settings yet and
 * no OBS-Spectra is running from the old folder. Call before anything opens
 * the configuration (the log is not open yet: see Note()). */
void MigrateConfig();

/* What MigrateConfig() did, for the log; empty when there was nothing to do */
const std::string &Note();

} // namespace SpectraConfigLocation
