#include "SpectraConfigLocation.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace SpectraConfigLocation {

static std::string note;

/* The folder name earlier versions used under %APPDATA% */
static constexpr const char *LEGACY_DIR = "OBS-Spectra";
static constexpr const char *SETTINGS_MARKER = "obs-studio/global.ini";

const std::string &Root()
{
	static const std::string root = [] {
		std::string result;
#ifdef _WIN32
		PWSTR profile = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &profile)) && profile) {
			char *utf8 = nullptr;
			if (os_wcs_to_utf8_ptr(profile, 0, &utf8) && utf8) {
				result = std::string(utf8) + "/Spectra";
			}
			bfree(utf8);
		}
		CoTaskMemFree(profile);
		std::replace(result.begin(), result.end(), '\\', '/');
#else
		char path[512];
		if (os_get_config_path(path, sizeof(path), LEGACY_DIR) > 0) {
			result = path;
		}
#endif
		return result;
	}();
	return root;
}

const std::string &Note()
{
	return note;
}

#ifdef _WIN32
/* UTF-8 <-> paths through the wide strings Windows uses */
static fs::path U8(const std::string &s)
{
	wchar_t *wide = nullptr;
	os_utf8_to_wcs_ptr(s.c_str(), s.size(), &wide);
	fs::path path(wide ? wide : L"");
	bfree(wide);
	return path;
}

static std::string Str(const fs::path &p)
{
	char *utf8 = nullptr;
	os_wcs_to_utf8_ptr(p.c_str(), 0, &utf8);
	std::string result(utf8 ? utf8 : "");
	bfree(utf8);
	return result;
}

/* An OBS-Spectra already running (not portable) holds this mutex
 * (platform-windows.cpp CheckIfAlreadyRunning) */
static bool SpectraRunning()
{
	HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"OBSSpectraCore");
	if (mutex) {
		CloseHandle(mutex);
		return true;
	}
	return false;
}

/* Moves a file or folder tree; across drives by copying and then deleting
 * the original only once everything was copied */
static bool MoveTree(const fs::path &from, const fs::path &to, std::string &error)
{
	std::error_code ec;
	fs::rename(from, to, ec);
	if (!ec) {
		return true;
	}
	fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
	if (ec) {
		error = ec.message();
		return false;
	}
	fs::remove_all(from, ec);
	if (ec) {
		error = "copied, but the old folder could not be deleted: " + ec.message();
	}
	return true;
}
#endif

void MigrateConfig()
{
#ifdef _WIN32
	const std::string &root = Root();
	char legacyBuf[512];
	if (root.empty() || os_get_config_path(legacyBuf, sizeof(legacyBuf), LEGACY_DIR) <= 0) {
		return;
	}
	const fs::path legacy = U8(legacyBuf);
	const fs::path target = U8(root);
	std::error_code ec;

	if (!fs::is_directory(legacy, ec)) {
		return;
	}
	if (fs::exists(target / U8(SETTINGS_MARKER), ec)) {
		note = "Settings are in " + root + "; " + Str(legacy) + " is no longer used";
		return;
	}
	if (SpectraRunning()) {
		note = "Not moving the settings from " + Str(legacy) + " to " + root +
		       " while OBS-Spectra is running; they move on the next start";
		return;
	}

	std::string error;
	if (!fs::exists(target, ec)) {
		if (MoveTree(legacy, target, error)) {
			note = "Moved the settings from " + Str(legacy) + " to " + root;
			if (!error.empty()) {
				note += " (" + error + ")";
			}
		} else {
			note = "Could not move the settings from " + Str(legacy) + " to " + root + ": " + error;
		}
		return;
	}

	/* the folder is there already (made by hand, or holding something else):
	 * move each entry the folder doesn't have, keep anything it does */
	int moved = 0, kept = 0, failed = 0;
	for (const fs::directory_entry &entry : fs::directory_iterator(legacy, ec)) {
		const fs::path dest = target / entry.path().filename();
		if (fs::exists(dest, ec)) {
			kept++;
			continue;
		}
		std::string entryError;
		if (MoveTree(entry.path(), dest, entryError)) {
			moved++;
		} else {
			failed++;
			error = entryError;
		}
	}
	if (failed == 0 && kept == 0) {
		fs::remove(legacy, ec);
	}
	note = "Moved " + std::to_string(moved) + " item(s) of the settings from " + Str(legacy) + " into " + root;
	if (kept) {
		note += "; left " + std::to_string(kept) + " that " + root + " already has";
	}
	if (failed) {
		note += "; " + std::to_string(failed) + " could not be moved: " + error;
	}
#endif
}

} // namespace SpectraConfigLocation
