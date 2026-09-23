#include "capture-win.hpp"

#include <QFileInfo>
#include <QRegularExpression>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace obscura {

#ifdef _WIN32

namespace {

QString ExeName(DWORD pid)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) {
		return {};
	}
	wchar_t buf[1024];
	DWORD size = 1024;
	QString name;
	if (QueryFullProcessImageNameW(process, 0, buf, &size)) {
		name = QFileInfo(QString::fromWCharArray(buf, (int)size)).fileName();
	}
	CloseHandle(process);
	return name;
}

struct Search {
	QRegularExpression process;
	QString title;
	HWND foreground;
	HWND best = nullptr;
	long long bestKey = -1;
};

BOOL CALLBACK Visit(HWND hwnd, LPARAM param)
{
	auto *s = reinterpret_cast<Search *>(param);
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
		return TRUE;
	}
	wchar_t titleBuf[512];
	const int len = GetWindowTextW(hwnd, titleBuf, 512);
	if (len <= 0) {
		return TRUE;
	}
	const QString title = QString::fromWCharArray(titleBuf, len);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	const bool byProcess = s->process.isValid() && !s->process.pattern().isEmpty() &&
			       s->process.match(ExeName(pid)).hasMatch();
	const bool byTitle = !s->title.isEmpty() && title.contains(s->title, Qt::CaseInsensitive);
	if (!byProcess && !byTitle) {
		return TRUE;
	}
	RECT r;
	GetClientRect(hwnd, &r);
	long long key = (long long)(r.right - r.left) * (r.bottom - r.top);
	if (hwnd == s->foreground) {
		key += 1LL << 40;
	}
	if (byProcess) {
		key += 1LL << 41;
	}
	if (key > s->bestKey) {
		s->bestKey = key;
		s->best = hwnd;
	}
	return TRUE;
}

std::optional<spectra::Image> FromBitmap(HDC dc, HBITMAP bmp, int w, int h)
{
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h; /* top-down */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	std::vector<uint8_t> bgra((size_t)w * h * 4);
	if (!GetDIBits(dc, bmp, 0, h, bgra.data(), &bi, DIB_RGB_COLORS)) {
		return std::nullopt;
	}
	return spectra::FromBGRA(bgra.data(), w, h, w * 4);
}

} // namespace

std::optional<spectra::Image> CaptureTargetWindow(const QString &processRegex, const QString &titleContains,
						  QString *error)
{
	Search s{QRegularExpression(processRegex, QRegularExpression::CaseInsensitiveOption), titleContains,
		 GetForegroundWindow()};
	EnumWindows(Visit, reinterpret_cast<LPARAM>(&s));
	if (!s.best) {
		if (error) {
			*error = QStringLiteral("Game window not found");
		}
		return std::nullopt;
	}
	RECT r;
	GetClientRect(s.best, &r);
	const int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) {
		if (error) {
			*error = QStringLiteral("The game window has no size");
		}
		return std::nullopt;
	}
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);
	/* PW_CLIENTONLY | PW_RENDERFULLCONTENT (reads DirectX content via DWM) */
	const BOOL printed = PrintWindow(s.best, mem, 0x1 | 0x2);
	SelectObject(mem, old);
	std::optional<spectra::Image> img = printed ? FromBitmap(mem, bmp, w, h) : std::nullopt;
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	if (!img && error) {
		*error = QStringLiteral("Could not capture the game window");
	}
	return img;
}

std::optional<spectra::Image> CaptureDesktop(QString *error)
{
	const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);
	const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
	SelectObject(mem, old);
	std::optional<spectra::Image> img = ok ? FromBitmap(mem, bmp, w, h) : std::nullopt;
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	if (!img && error) {
		*error = QStringLiteral("Could not capture the screen");
	}
	return img;
}

#else

std::optional<spectra::Image> CaptureTargetWindow(const QString &, const QString &, QString *error)
{
	if (error) {
		*error = QStringLiteral("Window capture is only available on Windows");
	}
	return std::nullopt;
}

std::optional<spectra::Image> CaptureDesktop(QString *error)
{
	if (error) {
		*error = QStringLiteral("Screen capture is only available on Windows");
	}
	return std::nullopt;
}

#endif

} // namespace obscura
