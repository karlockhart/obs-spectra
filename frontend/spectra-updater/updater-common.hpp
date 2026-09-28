#pragma once

/*
 * Shared by spectra-updater.exe and the in-app updater (SpectraUpdateDialog):
 * where an update is staged and how its leftovers are removed. Win32 only, no
 * Qt, no OBS.
 *
 * For an install at C:\bin\obs-spectra:
 *   C:\bin\obs-spectra.update\           the update's work folder
 *   C:\bin\obs-spectra.update\new\       the new version, extracted
 *   C:\bin\obs-spectra.update\old\       the old version while it's swapped out
 *   C:\bin\obs-spectra.update.log        what the updater did
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace SpectraUpdater {

constexpr const wchar_t *WORK_SUFFIX = L".update";
constexpr const wchar_t *LOG_SUFFIX = L".update.log";
constexpr const wchar_t *STAGING_NAME = L"new";
constexpr const wchar_t *BACKUP_NAME = L"old";
constexpr const wchar_t *HELPER_EXE = L"spectra-updater.exe";
constexpr const wchar_t *APP_EXE = L"obs-spectra.exe";
/* Written into the work folder by Spectra: only a folder with it is ever deleted */
constexpr const wchar_t *WORK_MARKER = L"spectra-update.txt";
constexpr const char *WORK_MARKER_TEXT =
	"OBS-Spectra downloads its updates into this folder and deletes it when it is done.\r\n";
/* Held by spectra-updater.exe while it runs, so Spectra leaves its folders alone */
constexpr const wchar_t *MUTEX_NAME = L"Local\\OBS-Spectra-Updater";

/* The files whose presence puts OBS in portable mode (see obs-main.cpp) */
constexpr const wchar_t *PORTABLE_MARKERS[] = {L"portable_mode", L"obs_portable_mode", L"portable_mode.txt",
					       L"obs_portable_mode.txt"};

inline std::wstring Join(const std::wstring &dir, const std::wstring &name)
{
	if (dir.empty() || dir.back() == L'\\' || dir.back() == L'/') {
		return dir + name;
	}
	return dir + L"\\" + name;
}

inline bool Exists(const std::wstring &path)
{
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool IsDir(const std::wstring &path)
{
	DWORD attr = GetFileAttributesW(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* An install's work folder, staging folder, backup folder and log */
inline std::wstring WorkDir(const std::wstring &install)
{
	return install + WORK_SUFFIX;
}

inline std::wstring StagingDir(const std::wstring &install)
{
	return Join(WorkDir(install), STAGING_NAME);
}

inline std::wstring BackupDir(const std::wstring &install)
{
	return Join(WorkDir(install), BACKUP_NAME);
}

inline std::wstring LogPath(const std::wstring &install)
{
	return install + LOG_SUFFIX;
}

/* True when a folder holds settings: a config folder or a portable marker.
 * A backup like that is never deleted automatically: it is what a failed
 * rollback left behind. */
inline bool HoldsSettings(const std::wstring &dir)
{
	if (Exists(Join(dir, L"config"))) {
		return true;
	}
	for (const wchar_t *marker : PORTABLE_MARKERS) {
		if (Exists(Join(dir, marker))) {
			return true;
		}
	}
	return false;
}

/* Deletes a file or folder tree. Junctions and symbolic links are removed
 * themselves, never followed, so nothing outside the tree is touched. Returns
 * false if anything could not be deleted (the rest is still deleted). */
inline bool RemoveTree(const std::wstring &path)
{
	DWORD attr = GetFileAttributesW(path.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES) {
		return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
	}
	if (attr & FILE_ATTRIBUTE_READONLY) {
		SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
	}
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		return DeleteFileW(path.c_str()) != 0;
	}
	if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
		/* a junction or directory symlink: remove the link only */
		return RemoveDirectoryW(path.c_str()) != 0;
	}

	bool ok = true;
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW(Join(path, L"*").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") {
				continue;
			}
			ok = RemoveTree(Join(path, name)) && ok;
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	return RemoveDirectoryW(path.c_str()) != 0 && ok;
}

} // namespace SpectraUpdater
