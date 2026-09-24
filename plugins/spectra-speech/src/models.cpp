#include <spectra-speech/models.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace spectra::speech {

namespace {

/* huggingface.co/ggerganov/whisper.cpp and ggml-org/whisper-vad, pinned to
 * a revision so the hashes below stay valid */
#define WHISPER_MODELS "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/"
#define VAD_MODELS "https://huggingface.co/ggml-org/whisper-vad/resolve/9ffd54a1e1ee413ddf265af9913beaf518d1639b/"

ModelInfo Whisper(const char *id, const char *title, qint64 size, const char *sha256, bool english = false)
{
	ModelInfo m;
	m.id = QString::fromLatin1(id);
	m.file = QStringLiteral("ggml-%1.bin").arg(m.id);
	m.title = QString::fromUtf8(title);
	m.size = size;
	m.sha256 = QByteArray(sha256);
	m.url = QStringLiteral(WHISPER_MODELS) + m.file;
	m.englishOnly = english;
	return m;
}

QMutex directoryMutex;
QString directory;

} // namespace

const std::vector<ModelInfo> &WhisperModels()
{
	static const std::vector<ModelInfo> models = {
		Whisper("large-v3-turbo-q5_0", "Large v3 Turbo, compact: best accuracy for the size (GPU)", 574041195,
			"394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2"),
		Whisper("large-v3-turbo-q8_0", "Large v3 Turbo: slightly more accurate (GPU)", 874188075,
			"317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1"),
		Whisper("large-v3-turbo", "Large v3 Turbo, full precision (GPU)", 1624555275,
			"1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"),
		Whisper("medium.en-q5_0", "Medium, English only", 539225533,
			"76733e26ad8fe1c7a5bf7531a9d41917b2adc0f20f2e4f5531688a8c6cd88eb0", true),
		Whisper("small.en-q8_0", "Small, English only: good on a CPU", 264477561,
			"67a179f608ea6114bd3fdb9060e762b588a3fb3bd00c4387971be4d177958067", true),
		Whisper("small-q8_0", "Small: good on a CPU", 264464607,
			"49c8fb02b65e6049d5fa6c04f81f53b867b5ec9540406812c643f177317f779f"),
		Whisper("base.en-q8_0", "Base, English only: fastest, least accurate", 81781811,
			"a4d4a0768075e13cfd7e19df3ae2dbc4a68d37d36a7dad45e8410c9a34f8c87e", true),
		Whisper("base-q8_0", "Base: fastest, least accurate", 81768585,
			"c577b9a86e7e048a0b7eada054f4dd79a56bbfa911fbdacf900ac5b567cbb7d9"),
	};
	return models;
}

const ModelInfo *FindWhisperModel(const QString &id)
{
	for (const ModelInfo &model : WhisperModels()) {
		if (model.id == id) {
			return &model;
		}
	}
	return nullptr;
}

QString DefaultWhisperModel()
{
	return QStringLiteral("large-v3-turbo-q5_0");
}

const ModelInfo &VadModel()
{
	static const ModelInfo vad = []() {
		ModelInfo m;
		m.id = QStringLiteral("silero-v6.2.0");
		m.file = QStringLiteral("ggml-silero-v6.2.0.bin");
		m.title = QStringLiteral("Silero voice activity detection");
		m.size = 885098;
		m.sha256 = QByteArray("2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987");
		m.url = QStringLiteral(VAD_MODELS) + m.file;
		return m;
	}();
	return vad;
}

void SetModelDirectory(const QString &dir)
{
	QMutexLocker lock(&directoryMutex);
	directory = QDir::cleanPath(dir);
}

QString ModelDirectory()
{
	QMutexLocker lock(&directoryMutex);
	return directory;
}

QString ModelPath(const ModelInfo &model)
{
	const QString dir = ModelDirectory();
	return dir.isEmpty() ? QString() : QDir(dir).filePath(model.file);
}

bool ModelInstalled(const ModelInfo &model)
{
	const QString path = ModelPath(model);
	return !path.isEmpty() && QFileInfo(path).size() == model.size;
}

#ifdef _WIN32
bool DownloadModel(const ModelInfo &model, const DownloadProgress &progress, QString *error)
{
	auto fail = [&](const QString &message) {
		if (error) {
			*error = message;
		}
		return false;
	};

	const QString target = ModelPath(model);
	if (target.isEmpty()) {
		return fail(QStringLiteral("No folder for speech models"));
	}
	QDir().mkpath(QFileInfo(target).absolutePath());
	const QString partial = target + QStringLiteral(".part");

	std::wstring wurl = model.url.toStdWString();
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256] = {}, path[2048] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
		return fail(QStringLiteral("Not an https URL: %1").arg(model.url));
	}

	/* follows redirects (Hugging Face serves files from a CDN) */
	HINTERNET session = WinHttpOpen(L"OBS-Spectra", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
				    : nullptr;
	auto close = [&]() {
		if (request) {
			WinHttpCloseHandle(request);
		}
		if (connect) {
			WinHttpCloseHandle(connect);
		}
		if (session) {
			WinHttpCloseHandle(session);
		}
	};

	DWORD status = 0;
	bool ok = request && WinHttpSetTimeouts(request, 30000, 30000, 30000, 60000) &&
		  WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		  WinHttpReceiveResponse(request, nullptr);
	if (ok) {
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
	}
	if (!ok || status != 200) {
		const DWORD lastError = GetLastError();
		close();
		return fail(ok ? QStringLiteral("HTTP %1 from %2").arg(status).arg(QString::fromWCharArray(host))
			       : QStringLiteral("Could not reach %1 (error %2)")
					    .arg(QString::fromWCharArray(host))
					    .arg(lastError));
	}

	QFile file(partial);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		close();
		return fail(QStringLiteral("Could not write %1").arg(partial));
	}

	QCryptographicHash hash(QCryptographicHash::Sha256);
	QByteArray chunk(1 << 20, Qt::Uninitialized);
	qint64 received = 0;
	bool cancelled = false;
	bool readFailed = false;
	for (;;) {
		DWORD read = 0;
		if (!WinHttpReadData(request, chunk.data(), (DWORD)chunk.size(), &read)) {
			readFailed = true;
			break;
		}
		if (read == 0) {
			break;
		}
		if (file.write(chunk.constData(), read) != (qint64)read) {
			readFailed = true;
			break;
		}
		hash.addData(QByteArrayView(chunk.constData(), read));
		received += read;
		if (progress && !progress(received, model.size)) {
			cancelled = true;
			break;
		}
	}
	close();
	file.close();

	if (cancelled || readFailed || received != model.size) {
		QFile::remove(partial);
		if (cancelled) {
			return fail(QStringLiteral("Cancelled"));
		}
		return fail(QStringLiteral("The download of %1 was incomplete (%2 of %3 bytes)")
				    .arg(model.file)
				    .arg(received)
				    .arg(model.size));
	}
	if (hash.result().toHex() != model.sha256) {
		QFile::remove(partial);
		return fail(QStringLiteral("%1 didn't match its expected checksum").arg(model.file));
	}

	QFile::remove(target);
	if (!QFile::rename(partial, target)) {
		QFile::remove(partial);
		return fail(QStringLiteral("Could not move %1 into place").arg(model.file));
	}
	return true;
}
#else
bool DownloadModel(const ModelInfo &model, const DownloadProgress &, QString *error)
{
	if (error) {
		*error = QStringLiteral("Downloading %1 isn't supported on this platform").arg(model.file);
	}
	return false;
}
#endif

} // namespace spectra::speech
