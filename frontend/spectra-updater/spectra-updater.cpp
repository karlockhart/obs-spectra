/*
 * spectra-updater: puts a downloaded OBS-Spectra version into an install.
 *
 * Spectra downloads and extracts the new version into <install>.update\new,
 * copies this program into <install>.update, starts it and exits. This then
 *
 *   1. waits for every program running from the install to exit,
 *   2. renames the install to <install>.update\old,
 *   3. moves its settings and the user's own files into the new version:
 *      config\, the portable_mode marker and any other file or folder in the
 *      install's root that isn't part of OBS-Spectra,
 *   4. renames the new version to the install's name,
 *   5. starts OBS-Spectra again and deletes the old version.
 *
 * Every step is a rename inside one folder, so nothing is copied and a
 * failure is undone by renaming back: the old version starts as before.
 * Nothing outside the install, <install>.update and <install>.update.log (the
 * log) is touched; user data in %APPDATA% or elsewhere is never read.
 *
 *   spectra-updater --install <dir> --staging <dir>\..\<name>.update\new
 *                   [--wait-pid <pid>] [--version <tag>] [--timeout <seconds>]
 *                   [--relaunch-args <arguments>] [--no-relaunch] [--quiet]
 *
 * --quiet shows no window and asks nothing (a timeout fails the update). Exit
 * codes: 0 updated, 1 bad arguments, 2 nothing changed, 3 failed and rolled
 * back, 4 failed and could not roll back (the log says what to do).
 *
 * Only Win32 and the static C runtime: nothing it loads lives in the install.
 */

#include "updater-common.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

using namespace SpectraUpdater;

namespace {

enum ExitCode { EXIT_UPDATED = 0, EXIT_BAD_ARGS = 1, EXIT_UNCHANGED = 2, EXIT_ROLLED_BACK = 3, EXIT_BROKEN = 4 };

constexpr const wchar_t *TITLE = L"OBS-Spectra Updater";
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
/* How long a rename is retried while something (often an antivirus scan) holds a file */
constexpr DWORD RENAME_RETRY_MS = 10000;
/* The log is started over once it grows past this */
constexpr long MAX_LOG_BYTES = 1 << 20;

/* Top-level folders of an OBS-Spectra install: always the new version's */
constexpr const wchar_t *PROGRAM_DIRS[] = {L"bin", L"data", L"obs-plugins"};
/* Left behind by Update-Portable.ps1, which this replaces: not carried over */
constexpr const wchar_t *OBSOLETE_FILES[] = {L"Update-Portable.cmd", L"Update-Portable.ps1", L"spectra-version.txt",
					     L".spectra-update"};

struct Options {
	std::wstring install;
	std::wstring staging;
	std::wstring version;
	std::wstring relaunchArgs;
	DWORD waitPid = 0;
	DWORD timeoutSec = 60;
	bool relaunch = true;
	bool ui = true;
};

Options opts;
FILE *logFile = nullptr;
HWND window = nullptr;
HWND statusLabel = nullptr;
std::atomic<int> exitCode = EXIT_UNCHANGED;

/* --- text and logging --------------------------------------------------------- */

std::string Utf8(const std::wstring &text)
{
	if (text.empty()) {
		return {};
	}
	int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
	std::string out(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), len, nullptr, nullptr);
	return out;
}

std::wstring Format(const wchar_t *fmt, va_list args)
{
	std::vector<wchar_t> buf(1024);
	for (;;) {
		va_list copy;
		va_copy(copy, args);
		int n = _vsnwprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, copy);
		va_end(copy);
		if (n >= 0) {
			return std::wstring(buf.data(), n);
		}
		buf.resize(buf.size() * 2);
	}
}

std::wstring Fmt(const wchar_t *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::wstring s = Format(fmt, args);
	va_end(args);
	return s;
}

void Log(const wchar_t *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::wstring line = Format(fmt, args);
	va_end(args);

	SYSTEMTIME t;
	GetLocalTime(&t);
	std::wstring stamped = Fmt(L"%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
				   t.wMinute, t.wSecond, line.c_str());
	OutputDebugStringW(stamped.c_str());
	if (logFile) {
		std::string utf8 = Utf8(stamped);
		fwrite(utf8.data(), 1, utf8.size(), logFile);
		fflush(logFile);
	}
}

std::wstring ErrorText(DWORD err)
{
	wchar_t *msg = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		       nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
	std::wstring text = msg ? msg : L"";
	LocalFree(msg);
	while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L'.')) {
		text.pop_back();
	}
	return Fmt(L"%s (error %lu)", text.c_str(), err);
}

void OpenLog()
{
	std::wstring path = LogPath(opts.install);
	logFile = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
	if (logFile && ftell(logFile) > MAX_LOG_BYTES) {
		fclose(logFile);
		logFile = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
	}
}

/* --- window ------------------------------------------------------------------- */

void SetStatus(const std::wstring &text)
{
	Log(L"%s", text.c_str());
	if (window) {
		PostMessageW(window, WM_APP_STATUS, 0, (LPARAM) new std::wstring(text));
	}
}

int Ask(const std::wstring &text, UINT flags)
{
	if (!opts.ui) {
		return (flags & MB_TYPEMASK) == MB_OK ? IDOK : IDCANCEL;
	}
	return MessageBoxW(window, text.c_str(), TITLE, flags | MB_SETFOREGROUND | MB_TOPMOST);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_APP_STATUS: {
		std::wstring *text = (std::wstring *)lp;
		SetWindowTextW(statusLabel, text->c_str());
		delete text;
		return 0;
	}
	case WM_APP_DONE:
		DestroyWindow(hwnd);
		return 0;
	case WM_CLOSE:
		/* closing midway would leave the install half swapped: the work finishes first */
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void CreateUi(HINSTANCE instance)
{
	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
	InitCommonControlsEx(&icc);

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = instance;
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"SpectraUpdater";
	RegisterClassExW(&wc);

	HDC dc = GetDC(nullptr);
	int dpi = GetDeviceCaps(dc, LOGPIXELSX);
	ReleaseDC(nullptr, dc);
	auto px = [dpi](int v) {
		return MulDiv(v, dpi, 96);
	};

	RECT rc = {0, 0, px(440), px(96)};
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRect(&rc, style, FALSE);
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	window = CreateWindowExW(0, wc.lpszClassName, L"Updating OBS-Spectra", style,
				 (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
				 nullptr, nullptr, instance, nullptr);

	NONCLIENTMETRICSW ncm = {sizeof(ncm)};
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

	statusLabel = CreateWindowExW(0, L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
				      px(16), px(16), px(408), px(36), window, nullptr, instance, nullptr);
	SendMessageW(statusLabel, WM_SETFONT, (WPARAM)font, TRUE);
	HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE, px(16), px(60),
				   px(408), px(18), window, nullptr, instance, nullptr);
	SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);

	ShowWindow(window, SW_SHOWNORMAL);
	UpdateWindow(window);
}

/* --- paths and processes ------------------------------------------------------ */

std::wstring FullPath(const std::wstring &path)
{
	if (path.empty()) {
		return path;
	}
	DWORD len = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
	std::wstring full(len, L'\0');
	len = GetFullPathNameW(path.c_str(), len, full.data(), nullptr);
	full.resize(len);
	while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) {
		full.pop_back();
	}
	return full;
}

bool SameText(const std::wstring &a, const std::wstring &b)
{
	return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool StartsWithText(const std::wstring &text, const std::wstring &prefix)
{
	return text.size() >= prefix.size() && SameText(text.substr(0, prefix.size()), prefix);
}

std::wstring ParentOf(const std::wstring &path)
{
	size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring NameOf(const std::wstring &path)
{
	size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring AppExe(const std::wstring &install)
{
	return Join(install, std::wstring(L"bin\\64bit\\") + APP_EXE);
}

struct Process {
	DWORD pid;
	std::wstring name;
};

std::wstring ImagePath(HANDLE process)
{
	std::vector<wchar_t> buf(32768);
	DWORD size = (DWORD)buf.size();
	if (!QueryFullProcessImageNameW(process, 0, buf.data(), &size)) {
		return {};
	}
	return std::wstring(buf.data(), size);
}

/* Every process whose program lives in dir */
std::vector<Process> RunningFrom(const std::wstring &dir)
{
	std::vector<Process> found;
	std::wstring prefix = dir + L"\\";
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return found;
	}
	PROCESSENTRY32W pe = {sizeof(pe)};
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
		if (pe.th32ProcessID == GetCurrentProcessId() || pe.th32ProcessID == 0) {
			continue;
		}
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
		if (!h) {
			continue;
		}
		std::wstring image = ImagePath(h);
		CloseHandle(h);
		if (StartsWithText(image, prefix)) {
			found.push_back({pe.th32ProcessID, pe.szExeFile});
		}
	}
	CloseHandle(snap);
	return found;
}

std::wstring Describe(const std::vector<Process> &procs)
{
	std::wstring text;
	for (const Process &p : procs) {
		text += Fmt(L"    %s (process %lu)\n", p.name.c_str(), p.pid);
	}
	return text;
}

/* Waits until nothing runs from the install. False when the user gives up. */
bool WaitForInstallToClose()
{
	SetStatus(L"Waiting for OBS-Spectra to close...");

	if (opts.waitPid) {
		HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, opts.waitPid);
		if (h) {
			/* only wait for it if it is still the Spectra that started us */
			if (StartsWithText(ImagePath(h), opts.install + L"\\")) {
				Log(L"Waiting for process %lu to exit", opts.waitPid);
				WaitForSingleObject(h, opts.timeoutSec * 1000);
			}
			CloseHandle(h);
		}
	}

	for (;;) {
		ULONGLONG deadline = GetTickCount64() + opts.timeoutSec * 1000ull;
		std::vector<Process> running = RunningFrom(opts.install);
		while (!running.empty() && GetTickCount64() < deadline) {
			Sleep(250);
			running = RunningFrom(opts.install);
		}
		if (running.empty()) {
			return true;
		}

		Log(L"Still running after %lu seconds:\r\n%s", opts.timeoutSec, Describe(running).c_str());
		std::wstring text =
			Fmt(L"OBS-Spectra can't be updated while these programs from\n%s\nare running:\n\n%s\n"
			    L"Close them (OBS-Spectra may still be saving), then click Retry.\n"
			    L"Cancel leaves OBS-Spectra as it is.",
			    opts.install.c_str(), Describe(running).c_str());
		if (Ask(text, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) {
			Log(L"Gave up waiting");
			return false;
		}
		SetStatus(L"Waiting for OBS-Spectra to close...");
	}
}

/* A rename, retried for a while when something briefly holds a file open */
bool Move(const std::wstring &from, const std::wstring &to)
{
	ULONGLONG until = GetTickCount64() + RENAME_RETRY_MS;
	for (;;) {
		if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
			return true;
		}
		DWORD err = GetLastError();
		bool transient = err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ||
				 err == ERROR_LOCK_VIOLATION;
		if (!transient || GetTickCount64() >= until) {
			Log(L"Could not move %s to %s: %s", from.c_str(), to.c_str(), ErrorText(err).c_str());
			SetLastError(err);
			return false;
		}
		Sleep(250);
	}
}

bool InList(const std::wstring &name, const wchar_t *const *list, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (SameText(name, list[i])) {
			return true;
		}
	}
	return false;
}

template<size_t N> bool InList(const std::wstring &name, const wchar_t *const (&list)[N])
{
	return InList(name, list, N);
}

std::vector<std::wstring> ListDir(const std::wstring &dir)
{
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE) {
		return names;
	}
	do {
		std::wstring name = fd.cFileName;
		if (name != L"." && name != L"..") {
			names.push_back(name);
		}
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	return names;
}

/* --- the swap ----------------------------------------------------------------- */

enum class Result { Updated, Unchanged, RolledBack, Broken };

struct Swap {
	std::wstring install = opts.install;
	std::wstring staging = opts.staging;
	std::wstring backup = BackupDir(opts.install);
	std::wstring error;
	bool installMoved = false;         /* install renamed to backup */
	bool stagingIsInstall = false;     /* staging renamed to install */
	std::vector<std::wstring> carried; /* names moved from backup into staging */

	Result Fail(const std::wstring &why)
	{
		error = why;
		Log(L"Failed: %s", why.c_str());
		return installMoved ? Rollback() : Result::Unchanged;
	}

	Result Rollback()
	{
		SetStatus(L"Putting the previous version back...");
		bool ok = true;
		if (stagingIsInstall) {
			if (!Move(install, staging)) {
				ok = false;
			} else {
				stagingIsInstall = false;
			}
		}
		if (!stagingIsInstall) {
			for (auto it = carried.rbegin(); it != carried.rend(); ++it) {
				if (!Move(Join(staging, *it), Join(backup, *it))) {
					ok = false;
				}
			}
		}
		if (ok && Move(backup, install)) {
			installMoved = false;
			Log(L"Rolled back: %s is the previous version again", install.c_str());
			return Result::RolledBack;
		}
		Log(L"ROLLBACK FAILED. The previous version is in %s, the new one in %s. To get OBS-Spectra back, "
		    L"move everything from the new version's folder that isn't bin, data or obs-plugins (config, "
		    L"portable_mode.txt, your own files) back into %s, then rename %s to %s.",
		    backup.c_str(), stagingIsInstall ? install.c_str() : staging.c_str(), backup.c_str(),
		    backup.c_str(), install.c_str());
		return Result::Broken;
	}

	Result Run()
	{
		if (Exists(backup)) {
			if (HoldsSettings(backup)) {
				return Fail(
					Fmt(L"An earlier update left a previous version with settings in %s. Check it "
					    L"(and %s), move back what you need and delete it, then try again.",
					    backup.c_str(), LogPath(install).c_str()));
			}
			Log(L"Deleting what an earlier update left in %s", backup.c_str());
			if (!RemoveTree(backup)) {
				return Fail(Fmt(L"Could not delete %s, left by an earlier update.", backup.c_str()));
			}
		}

		SetStatus(L"Moving the current version aside...");
		while (!Move(install, backup)) {
			DWORD err = GetLastError();
			std::wstring text = Fmt(
				L"The current version in\n%s\ncan't be moved aside: %s.\n\nSomething still has a file "
				L"or folder in it open: a command prompt started there, a program run from it, or a "
				L"file open in an editor. Close it, then click Retry.\nCancel leaves OBS-Spectra as it "
				L"is.",
				install.c_str(), ErrorText(err).c_str());
			if (Ask(text, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) {
				return Fail(Fmt(L"%s could not be moved aside: %s", install.c_str(),
						ErrorText(err).c_str()));
			}
		}
		installMoved = true;
		Log(L"Moved %s to %s", install.c_str(), backup.c_str());

		SetStatus(L"Keeping your settings...");
		for (const std::wstring &name : ListDir(backup)) {
			if (InList(name, PROGRAM_DIRS)) {
				continue;
			}
			if (InList(name, OBSOLETE_FILES)) {
				Log(L"Leaving out %s (replaced by this updater)", name.c_str());
				continue;
			}
			bool settings = SameText(name, L"config") || InList(name, PORTABLE_MARKERS);
			std::wstring from = Join(backup, name);
			std::wstring to = Join(staging, name);
			if (Exists(to)) {
				if (!settings) {
					Log(L"Keeping the new version's %s", name.c_str());
					continue;
				}
				/* the package's own copy gives way to the install's */
				if (!RemoveTree(to)) {
					return Fail(Fmt(L"Could not replace the new version's %s.", name.c_str()));
				}
			}
			if (!Move(from, to)) {
				return Fail(Fmt(L"Could not move %s into the new version: %s", from.c_str(),
						ErrorText(GetLastError()).c_str()));
			}
			carried.push_back(name);
			Log(L"Kept %s", name.c_str());
		}

		/* keep the install in the mode it was in: a portable zip must not make an
		 * installed-style folder portable, which would hide its settings */
		for (const wchar_t *marker : PORTABLE_MARKERS) {
			std::wstring path = Join(staging, marker);
			if (Exists(path) && !Exists(Join(backup, marker)) &&
			    std::find(carried.begin(), carried.end(), std::wstring(marker)) == carried.end()) {
				if (!RemoveTree(path)) {
					return Fail(Fmt(L"Could not remove %s from the new version.", marker));
				}
				Log(L"Removed the new version's %s: this install is not portable", marker);
			}
		}

		SetStatus(L"Putting the new version in place...");
		if (!Move(staging, install)) {
			return Fail(Fmt(L"Could not move the new version to %s: %s", install.c_str(),
					ErrorText(GetLastError()).c_str()));
		}
		stagingIsInstall = true;

		if (!Exists(AppExe(install))) {
			return Fail(Fmt(L"%s is missing after the update.", AppExe(install).c_str()));
		}
		for (const std::wstring &name : carried) {
			if (!Exists(Join(install, name))) {
				return Fail(Fmt(L"%s is missing after the update.", Join(install, name).c_str()));
			}
		}

		installMoved = false;
		Log(L"%s now has %s", install.c_str(),
		    opts.version.empty() ? L"the new version" : opts.version.c_str());
		return Result::Updated;
	}
};

bool Relaunch()
{
	std::wstring exe = AppExe(opts.install);
	std::wstring cmd = L"\"" + exe + L"\"";
	if (!opts.relaunchArgs.empty()) {
		cmd += L" " + opts.relaunchArgs;
	}
	std::wstring dir = ParentOf(exe);
	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
		Log(L"Could not start %s: %s", exe.c_str(), ErrorText(GetLastError()).c_str());
		return false;
	}
	AllowSetForegroundWindow(pi.dwProcessId);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	Log(L"Started %s", cmd.c_str());
	return true;
}

bool AppRunning()
{
	for (const Process &p : RunningFrom(opts.install)) {
		if (SameText(p.name, APP_EXE)) {
			return true;
		}
	}
	return false;
}

void CleanUp()
{
	std::wstring work = WorkDir(opts.install);
	SetStatus(L"Deleting the previous version...");
	if (!RemoveTree(BackupDir(opts.install))) {
		Log(L"Could not delete all of %s; OBS-Spectra deletes it next time", BackupDir(opts.install).c_str());
	}
	/* everything else in the work folder: the download, and this program unless it runs from there */
	if (Exists(Join(work, WORK_MARKER))) {
		for (const std::wstring &name : ListDir(work)) {
			if (!SameText(name, WORK_MARKER)) {
				RemoveTree(Join(work, name));
			}
		}
		if (ListDir(work).size() == 1) {
			RemoveTree(work);
		}
	}
}

int Update()
{
	Log(L"=== Updating %s to %s (from process %lu)", opts.install.c_str(),
	    opts.version.empty() ? L"a new version" : opts.version.c_str(), opts.waitPid);

	if (!WaitForInstallToClose()) {
		if (opts.relaunch && !AppRunning()) {
			Relaunch();
		}
		return EXIT_UNCHANGED;
	}

	Swap swap;
	Result result = swap.Run();

	if (result == Result::Updated) {
		if (opts.relaunch) {
			SetStatus(L"Starting OBS-Spectra...");
			if (!Relaunch()) {
				Ask(Fmt(L"OBS-Spectra was updated but could not be started. Start it from\n%s",
					AppExe(opts.install).c_str()),
				    MB_OK | MB_ICONWARNING);
			}
		}
		CleanUp();
		Log(L"Done");
		return EXIT_UPDATED;
	}

	if (result == Result::Broken) {
		Ask(Fmt(L"The update failed and the previous version could not be put back automatically.\n\n%s\n\n"
			L"Nothing was deleted. The log says how to put it back by hand:\n%s",
			swap.error.c_str(), LogPath(opts.install).c_str()),
		    MB_OK | MB_ICONERROR);
		return EXIT_BROKEN;
	}

	if (opts.relaunch && !AppRunning()) {
		Relaunch();
	}
	Ask(Fmt(L"OBS-Spectra could not be updated:\n\n%s\n\n%s\n\nDetails are in %s", swap.error.c_str(),
		result == Result::RolledBack ? L"The previous version was put back and still works."
					     : L"Nothing was changed.",
		LogPath(opts.install).c_str()),
	    MB_OK | MB_ICONWARNING);
	return result == Result::RolledBack ? EXIT_ROLLED_BACK : EXIT_UNCHANGED;
}

/* --- arguments ---------------------------------------------------------------- */

std::wstring ParseArgs()
{
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) {
		return L"Could not read the command line.";
	}
	std::wstring error;
	for (int i = 1; i < argc && error.empty(); i++) {
		std::wstring arg = argv[i];
		auto value = [&]() -> std::wstring {
			if (i + 1 >= argc) {
				error = arg + L" needs a value.";
				return {};
			}
			return argv[++i];
		};
		if (arg == L"--install") {
			opts.install = FullPath(value());
		} else if (arg == L"--staging") {
			opts.staging = FullPath(value());
		} else if (arg == L"--version") {
			opts.version = value();
		} else if (arg == L"--relaunch-args") {
			opts.relaunchArgs = value();
		} else if (arg == L"--wait-pid") {
			opts.waitPid = (DWORD)wcstoul(value().c_str(), nullptr, 10);
		} else if (arg == L"--timeout") {
			opts.timeoutSec = (DWORD)wcstoul(value().c_str(), nullptr, 10);
		} else if (arg == L"--no-relaunch") {
			opts.relaunch = false;
		} else if (arg == L"--quiet") {
			opts.ui = false;
		} else {
			error = L"Unknown argument: " + arg;
		}
	}
	LocalFree(argv);
	if (!error.empty()) {
		return error;
	}

	if (opts.install.empty() || opts.staging.empty()) {
		return L"Usage: spectra-updater --install <folder> --staging <folder>.update\\new [--wait-pid <pid>] "
		       L"[--version <tag>] [--timeout <seconds>] [--relaunch-args <arguments>] [--no-relaunch] "
		       L"[--quiet]";
	}
	if (opts.timeoutSec == 0) {
		opts.timeoutSec = 60;
	}
	/* never a drive's root: the install must be a folder with siblings */
	if (ParentOf(opts.install).size() < 3 || NameOf(opts.install).empty()) {
		return L"The install folder must not be a drive's root: " + opts.install;
	}
	if (!Exists(AppExe(opts.install))) {
		return L"Not an OBS-Spectra folder (no bin\\64bit\\obs-spectra.exe): " + opts.install;
	}
	if (!SameText(opts.staging, StagingDir(opts.install))) {
		return L"The new version must be in " + StagingDir(opts.install) + L", not " + opts.staging;
	}
	if (!Exists(Join(WorkDir(opts.install), WORK_MARKER))) {
		return L"Not an OBS-Spectra update folder (no " + std::wstring(WORK_MARKER) + L"): " +
		       WorkDir(opts.install);
	}
	if (!Exists(AppExe(opts.staging))) {
		return L"The new version is incomplete (no bin\\64bit\\obs-spectra.exe): " + opts.staging;
	}
	return {};
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	std::wstring error = ParseArgs();
	if (!error.empty()) {
		if (!opts.install.empty() && IsDir(ParentOf(opts.install))) {
			OpenLog();
			Log(L"Not updating: %s", error.c_str());
		}
		Ask(L"OBS-Spectra can't be updated:\n\n" + error, MB_OK | MB_ICONERROR);
		return EXIT_BAD_ARGS;
	}

	OpenLog();
	HANDLE mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);

	if (!opts.ui) {
		exitCode = Update();
	} else {
		CreateUi(instance);
		std::thread worker([]() {
			exitCode = Update();
			PostMessageW(window, WM_APP_DONE, 0, 0);
		});
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		worker.join();
	}

	Log(L"Exit code %d", exitCode.load());
	if (logFile) {
		fclose(logFile);
	}
	if (mutex) {
		CloseHandle(mutex);
	}
	return exitCode;
}
