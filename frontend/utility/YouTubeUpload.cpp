#include "YouTubeUpload.hpp"

#include <utility/obf.h>

#include <ui-config.h>

#include <util/base.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>

#include <qt-wrappers.hpp>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace YouTubeUpload {

namespace {

constexpr const char *AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char *TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char *REVOKE_URL = "https://oauth2.googleapis.com/revoke";
constexpr const char *CHANNELS_URL = "https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true";
constexpr const char *UPLOAD_URL =
	"https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status";
/* Uploading needs the upload scope; showing which channel is signed in
 * needs read access */
constexpr const char *SCOPES =
	"https://www.googleapis.com/auth/youtube.upload https://www.googleapis.com/auth/youtube.readonly";
constexpr const char *SECTION = "SpectraYouTube";
/* A multiple of 256 KiB, as Google requires for all but the last chunk */
constexpr int64_t CHUNK_SIZE = 32LL * 1024 * 1024;
constexpr int MAX_RETRIES = 5;
/* Refresh an access token that expires within this many seconds */
constexpr qint64 EXPIRY_MARGIN_SEC = 120;

/* ------------------------------------------------------------------------- */
/* HTTP                                                                      */

struct Response {
	long status = 0;
	std::string body;
	std::vector<std::string> headers;
};

/* Part of a file being sent as a request body */
struct FileChunk {
	FILE *file = nullptr;
	int64_t remaining = 0;
};

struct Transfer {
	const Progress *progress = nullptr;
	int64_t done = 0;
	int64_t total = 0;
	bool cancelled = false;
};

size_t WriteBody(char *ptr, size_t size, size_t nmemb, void *user)
{
	static_cast<std::string *>(user)->append(ptr, size * nmemb);
	return size * nmemb;
}

size_t WriteHeader(char *ptr, size_t size, size_t nmemb, void *user)
{
	std::string line(ptr, size * nmemb);
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
		line.pop_back();
	}
	static_cast<std::vector<std::string> *>(user)->push_back(line);
	return size * nmemb;
}

size_t ReadChunk(char *ptr, size_t size, size_t nmemb, void *user)
{
	FileChunk *chunk = static_cast<FileChunk *>(user);
	size_t want = (size_t)std::min<int64_t>((int64_t)(size * nmemb), chunk->remaining);
	if (want == 0) {
		return 0;
	}
	size_t got = fread(ptr, 1, want, chunk->file);
	chunk->remaining -= (int64_t)got;
	return got;
}

int TransferInfo(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t uploaded)
{
	Transfer *t = static_cast<Transfer *>(user);
	if (t->progress && *t->progress && t->total > 0) {
		double fraction = std::min((double)(t->done + uploaded) / (double)t->total, 1.0);
		if (!(*t->progress)((float)fraction)) {
			t->cancelled = true;
			return 1;
		}
	}
	return 0;
}

/*
 * One request. `chunk` sends part of a file as the body (a PUT), else `body`
 * is sent for POST/PUT. `timeoutSec` 0 means no overall limit, though a
 * stalled transfer still fails.
 */
bool Request(const char *method, const std::string &url, const std::vector<std::string> &headers,
	     const std::string &body, FileChunk *chunk, Transfer *transfer, Response &out, std::string &error,
	     long timeoutSec)
{
	out = Response();
	CURL *curl = curl_easy_init();
	if (!curl) {
		error = "Could not initialize HTTP";
		return false;
	}
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curlGuard(curl, curl_easy_cleanup);

	curl_slist *list = curl_slist_append(nullptr, "User-Agent: obs-spectra");
	/* No 100-continue round trip before every chunk */
	list = curl_slist_append(list, "Expect:");
	for (const std::string &h : headers) {
		list = curl_slist_append(list, h.c_str());
	}
	std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> listGuard(list, curl_slist_free_all);

	char errbuf[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	if (timeoutSec > 0) {
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
	}
	curl_obs_set_revoke_setting(curl);

	if (chunk) {
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadChunk);
		curl_easy_setopt(curl, CURLOPT_READDATA, chunk);
		curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)chunk->remaining);
	} else if (strcmp(method, "GET") != 0) {
		if (strcmp(method, "POST") == 0) {
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
		} else {
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
		}
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
	}
	if (transfer) {
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferInfo);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, transfer);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	}

	CURLcode code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
	if (code != CURLE_OK) {
		error = errbuf[0] ? errbuf : curl_easy_strerror(code);
		return false;
	}
	return true;
}

std::string Header(const Response &r, const char *name)
{
	const size_t n = strlen(name);
	auto sameName = [&](const std::string &line) {
		if (line.size() <= n + 1 || line[n] != ':') {
			return false;
		}
		for (size_t i = 0; i < n; i++) {
			if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)name[i])) {
				return false;
			}
		}
		return true;
	};
	for (const std::string &line : r.headers) {
		if (sameName(line)) {
			size_t start = line.find_first_not_of(" \t", n + 1);
			return start == std::string::npos ? std::string() : line.substr(start);
		}
	}
	return std::string();
}

QJsonObject Json(const Response &r)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(r.body)).object();
}

/* Google's error message for a failed request */
QString ApiError(const Response &r)
{
	const QJsonObject json = Json(r);
	QString message = json.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
	if (message.isEmpty()) {
		message = json.value(QStringLiteral("error_description")).toString();
	}
	if (message.isEmpty()) {
		message = json.value(QStringLiteral("error")).toString();
	}
	if (message.isEmpty()) {
		message = QStringLiteral("HTTP %1").arg(r.status);
	}
	return message;
}

std::vector<std::string> AuthHeaders(const Session &session, std::initializer_list<std::string> more = {})
{
	std::vector<std::string> headers{"Authorization: Bearer " + session.accessToken.toStdString()};
	headers.insert(headers.end(), more.begin(), more.end());
	return headers;
}

/* ------------------------------------------------------------------------- */
/* Credentials and tokens                                                    */

bool ClientCredentials(config_t *config, QString &id, QString &secret)
{
	std::string cid = YOUTUBE_CLIENTID;
	std::string sec = YOUTUBE_SECRET;
	if (!cid.empty()) {
		deobfuscate_str(&cid[0], YOUTUBE_CLIENTID_HASH);
	}
	if (!sec.empty()) {
		deobfuscate_str(&sec[0], YOUTUBE_SECRET_HASH);
	}
	id = QString::fromStdString(cid);
	secret = QString::fromStdString(sec);
	/* A development build without a compiled-in client can use one set in
	 * the profile config by hand */
	if (id.isEmpty() && config) {
		id = QString::fromUtf8(config_get_string(config, SECTION, "ClientId"));
		secret = QString::fromUtf8(config_get_string(config, SECTION, "ClientSecret"));
	}
	return !id.isEmpty() && !secret.isEmpty();
}

QString RandomToken(int bytes)
{
	QByteArray data(bytes, Qt::Uninitialized);
	for (int i = 0; i < bytes; i++) {
		data[i] = (char)QRandomGenerator::system()->bounded(256);
	}
	return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/* application/x-www-form-urlencoded, also used for the sign-in URL's query */
std::string Form(const QList<std::pair<QString, QString>> &fields)
{
	QByteArray out;
	for (const auto &field : fields) {
		if (!out.isEmpty()) {
			out += '&';
		}
		out += QUrl::toPercentEncoding(field.first) + '=' + QUrl::toPercentEncoding(field.second);
	}
	return out.toStdString();
}

/* Applies a token response to the session; false with an error otherwise */
bool ApplyTokens(const Response &r, Session &session, QString &error)
{
	if (r.status != 200) {
		error = ApiError(r);
		return false;
	}
	const QJsonObject json = Json(r);
	const QString access = json.value(QStringLiteral("access_token")).toString();
	if (access.isEmpty()) {
		error = QStringLiteral("No access token in Google's reply");
		return false;
	}
	session.accessToken = access;
	const QString refresh = json.value(QStringLiteral("refresh_token")).toString();
	if (!refresh.isEmpty()) {
		session.refreshToken = refresh;
	}
	const qint64 expiresIn = (qint64)json.value(QStringLiteral("expires_in")).toDouble(3600.0);
	session.expiresAt = QDateTime::currentSecsSinceEpoch() + expiresIn;
	return true;
}

bool EnsureAccessToken(Session &session, QString &error)
{
	if (!session.accessToken.isEmpty() &&
	    QDateTime::currentSecsSinceEpoch() + EXPIRY_MARGIN_SEC < session.expiresAt) {
		return true;
	}
	if (session.refreshToken.isEmpty()) {
		error = QStringLiteral("Not signed in to YouTube");
		return false;
	}
	const std::string body = Form({{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
				       {QStringLiteral("refresh_token"), session.refreshToken},
				       {QStringLiteral("client_id"), session.clientId},
				       {QStringLiteral("client_secret"), session.clientSecret}});
	Response r;
	std::string err;
	if (!Request("POST", TOKEN_URL, {"Content-Type: application/x-www-form-urlencoded"}, body, nullptr, nullptr, r,
		     err, 60)) {
		error = QString::fromStdString(err);
		return false;
	}
	if (!ApplyTokens(r, session, error)) {
		if (r.status == 400 || r.status == 401) {
			/* The refresh token was revoked or expired */
			session.refreshToken.clear();
		}
		return false;
	}
	return true;
}

bool FetchChannel(const Session &session, Account &account, QString &error)
{
	Response r;
	std::string err;
	if (!Request("GET", CHANNELS_URL, AuthHeaders(session), {}, nullptr, nullptr, r, err, 60)) {
		error = QString::fromStdString(err);
		return false;
	}
	if (r.status != 200) {
		error = ApiError(r);
		return false;
	}
	const QJsonArray items = Json(r).value(QStringLiteral("items")).toArray();
	if (items.isEmpty()) {
		error = QStringLiteral("This Google account has no YouTube channel");
		return false;
	}
	const QJsonObject channel = items.first().toObject();
	account.channelId = channel.value(QStringLiteral("id")).toString();
	account.channelTitle =
		channel.value(QStringLiteral("snippet")).toObject().value(QStringLiteral("title")).toString();
	return true;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Public                                                                    */

bool Available(config_t *config)
{
	QString id, secret;
	return ClientCredentials(config, id, secret);
}

bool LoadAccount(config_t *config, Account &account)
{
	/* config_get_string is null for a key that was never written */
	const char *refresh = config ? config_get_string(config, SECTION, "RefreshToken") : nullptr;
	if (!refresh || !*refresh) {
		return false;
	}
	account.channelId = QString::fromUtf8(config_get_string(config, SECTION, "ChannelId"));
	account.channelTitle = QString::fromUtf8(config_get_string(config, SECTION, "ChannelTitle"));
	return true;
}

bool LoadSession(config_t *config, Session &session)
{
	if (!ClientCredentials(config, session.clientId, session.clientSecret)) {
		return false;
	}
	session.accessToken = QString::fromUtf8(config_get_string(config, SECTION, "AccessToken"));
	session.refreshToken = QString::fromUtf8(config_get_string(config, SECTION, "RefreshToken"));
	session.expiresAt = config_get_int(config, SECTION, "ExpiresAt");
	return !session.refreshToken.isEmpty();
}

void SaveSession(config_t *config, const Session &session)
{
	config_set_string(config, SECTION, "AccessToken", QT_TO_UTF8(session.accessToken));
	config_set_string(config, SECTION, "RefreshToken", QT_TO_UTF8(session.refreshToken));
	config_set_int(config, SECTION, "ExpiresAt", session.expiresAt);
	config_save_safe(config, "tmp", nullptr);
}

void SignOut(config_t *config)
{
	Session session;
	if (LoadSession(config, session)) {
		/* Best effort: Google forgets the grant so it doesn't linger in
		 * the account's list of connected apps */
		Response r;
		std::string err;
		Request("POST", REVOKE_URL, {"Content-Type: application/x-www-form-urlencoded"},
			Form({{QStringLiteral("token"), session.refreshToken}}), nullptr, nullptr, r, err, 15);
	}
	for (const char *key : {"AccessToken", "RefreshToken", "ExpiresAt", "ChannelId", "ChannelTitle"}) {
		config_remove_value(config, SECTION, key);
	}
	config_save_safe(config, "tmp", nullptr);
}

bool BeginSignIn(config_t *config, quint16 loopbackPort, SignIn &signIn, QString &error)
{
	QString id, secret;
	if (!ClientCredentials(config, id, secret)) {
		error = QStringLiteral("This build has no YouTube API client");
		return false;
	}
	if (loopbackPort == 0) {
		error = QStringLiteral("Could not listen for the sign-in on this computer");
		return false;
	}
	signIn.state = RandomToken(24);
	signIn.codeVerifier = RandomToken(48);
	signIn.redirectUri = QStringLiteral("http://127.0.0.1:%1").arg(loopbackPort);
	const QByteArray challenge =
		QCryptographicHash::hash(signIn.codeVerifier.toLatin1(), QCryptographicHash::Sha256)
			.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

	/* prompt=consent asks every time, so a refresh token comes back even
	 * if the user granted access before (Google only sends one on the
	 * first consent) */
	const std::string query = Form({{QStringLiteral("client_id"), id},
					{QStringLiteral("redirect_uri"), signIn.redirectUri},
					{QStringLiteral("response_type"), QStringLiteral("code")},
					{QStringLiteral("scope"), QString::fromLatin1(SCOPES)},
					{QStringLiteral("access_type"), QStringLiteral("offline")},
					{QStringLiteral("prompt"), QStringLiteral("consent")},
					{QStringLiteral("state"), signIn.state},
					{QStringLiteral("code_challenge"), QString::fromLatin1(challenge)},
					{QStringLiteral("code_challenge_method"), QStringLiteral("S256")}});
	signIn.url = QString::fromLatin1(AUTH_URL) + QStringLiteral("?") + QString::fromStdString(query);
	return true;
}

bool FinishSignIn(config_t *config, const SignIn &signIn, const QString &code, Account &account, QString &error)
{
	Session session;
	if (!ClientCredentials(config, session.clientId, session.clientSecret)) {
		error = QStringLiteral("This build has no YouTube API client");
		return false;
	}
	/* The code arrives percent-encoded in the redirect's query string */
	const QString rawCode = QUrl::fromPercentEncoding(code.toLatin1());
	const std::string body = Form({{QStringLiteral("code"), rawCode},
				       {QStringLiteral("client_id"), session.clientId},
				       {QStringLiteral("client_secret"), session.clientSecret},
				       {QStringLiteral("redirect_uri"), signIn.redirectUri},
				       {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
				       {QStringLiteral("code_verifier"), signIn.codeVerifier}});
	Response r;
	std::string err;
	if (!Request("POST", TOKEN_URL, {"Content-Type: application/x-www-form-urlencoded"}, body, nullptr, nullptr, r,
		     err, 60)) {
		error = QString::fromStdString(err);
		return false;
	}
	if (!ApplyTokens(r, session, error)) {
		return false;
	}
	if (session.refreshToken.isEmpty()) {
		error = QStringLiteral("Google did not grant lasting access. Remove OBS-Spectra from the account's "
				       "connected apps and sign in again.");
		return false;
	}
	if (!FetchChannel(session, account, error)) {
		return false;
	}
	SaveSession(config, session);
	config_set_string(config, SECTION, "ChannelId", QT_TO_UTF8(account.channelId));
	config_set_string(config, SECTION, "ChannelTitle", QT_TO_UTF8(account.channelTitle));
	config_save_safe(config, "tmp", nullptr);
	blog(LOG_INFO, "[Spectra] YouTube: signed in as '%s'", QT_TO_UTF8(account.channelTitle));
	return true;
}

QString VideoUrl(const QString &videoId)
{
	return QStringLiteral("https://youtu.be/%1").arg(videoId);
}

/* ------------------------------------------------------------------------- */
/* Upload                                                                    */

namespace {

/* Waits between retries, giving up if the user cancels */
bool Backoff(int attempt, const Progress &progress, int64_t done, int64_t total)
{
	const int seconds = 1 << std::min(attempt, 5);
	for (int i = 0; i < seconds; i++) {
		if (progress && !progress(total > 0 ? (float)done / (float)total : 0.0f)) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return true;
}

/* The next byte to send, from a 308 reply's Range header */
int64_t ResumeOffset(const Response &r)
{
	const std::string range = Header(r, "Range");
	const size_t dash = range.rfind('-');
	if (dash == std::string::npos) {
		return 0;
	}
	return std::strtoll(range.c_str() + dash + 1, nullptr, 10) + 1;
}

} // namespace

bool Upload(Session &session, const QString &path, const Video &video, QString &videoId, QString &error,
	    const Progress &progress)
{
	const std::string file = path.toStdString();
	const int64_t size = os_get_file_size(file.c_str());
	if (size <= 0) {
		error = QStringLiteral("The clip file is missing or empty");
		return false;
	}
	if (!EnsureAccessToken(session, error)) {
		return false;
	}
	const std::string mime = QFileInfo(path).suffix().compare(QStringLiteral("mkv"), Qt::CaseInsensitive) == 0
					 ? "video/x-matroska"
					 : "video/mp4";

	/* Open a resumable upload session with the video's details */
	QJsonObject snippet{{QStringLiteral("title"), video.title},
			    {QStringLiteral("description"), video.description},
			    {QStringLiteral("categoryId"), video.categoryId}};
	if (!video.tags.isEmpty()) {
		snippet.insert(QStringLiteral("tags"), QJsonArray::fromStringList(video.tags));
	}
	QJsonObject status{{QStringLiteral("privacyStatus"), video.privacy},
			   {QStringLiteral("selfDeclaredMadeForKids"), false}};
	const std::string metadata =
		QJsonDocument(QJsonObject{{QStringLiteral("snippet"), snippet}, {QStringLiteral("status"), status}})
			.toJson(QJsonDocument::Compact)
			.toStdString();

	std::string location;
	for (int attempt = 0;; attempt++) {
		Response r;
		std::string err;
		const bool sent = Request("POST", UPLOAD_URL,
					  AuthHeaders(session, {"Content-Type: application/json; charset=UTF-8",
								"X-Upload-Content-Length: " + std::to_string(size),
								"X-Upload-Content-Type: " + mime}),
					  metadata, nullptr, nullptr, r, err, 60);
		if (sent && r.status == 200) {
			location = Header(r, "Location");
			if (location.empty()) {
				error = QStringLiteral("Google did not return an upload address");
				return false;
			}
			break;
		}
		if (sent && r.status == 401 && attempt == 0) {
			session.accessToken.clear();
			if (!EnsureAccessToken(session, error)) {
				return false;
			}
			continue;
		}
		if (sent && r.status < 500) {
			error = ApiError(r);
			return false;
		}
		if (attempt >= MAX_RETRIES) {
			error = sent ? ApiError(r) : QString::fromStdString(err);
			return false;
		}
		if (!Backoff(attempt, progress, 0, size)) {
			error = QStringLiteral("Cancelled");
			return false;
		}
	}
	blog(LOG_INFO, "[Spectra] YouTube: uploading '%s' (%lld bytes)", file.c_str(), (long long)size);

	FILE *f = os_fopen(file.c_str(), "rb");
	if (!f) {
		error = QStringLiteral("Could not open the clip file");
		return false;
	}
	std::unique_ptr<FILE, decltype(&fclose)> fileGuard(f, fclose);

	int64_t offset = 0;
	int retries = 0;
	while (true) {
		if (progress && !progress((float)((double)offset / (double)size))) {
			error = QStringLiteral("Cancelled");
			return false;
		}
		const int64_t length = std::min(CHUNK_SIZE, size - offset);
		if (os_fseeki64(f, offset, SEEK_SET) != 0) {
			error = QStringLiteral("Could not read the clip file");
			return false;
		}
		FileChunk chunk{f, length};
		Transfer transfer{&progress, offset, size, false};
		const std::string range = "Content-Range: bytes " + std::to_string(offset) + "-" +
					  std::to_string(offset + length - 1) + "/" + std::to_string(size);
		Response r;
		std::string err;
		const bool sent = Request("PUT", location, AuthHeaders(session, {"Content-Type: " + mime, range}), {},
					  &chunk, &transfer, r, err, 0);
		if (transfer.cancelled) {
			error = QStringLiteral("Cancelled");
			return false;
		}
		if (sent && (r.status == 200 || r.status == 201)) {
			videoId = Json(r).value(QStringLiteral("id")).toString();
			if (videoId.isEmpty()) {
				error = QStringLiteral("Google did not return the video's id");
				return false;
			}
			if (progress) {
				progress(1.0f);
			}
			blog(LOG_INFO, "[Spectra] YouTube: uploaded as %s", QT_TO_UTF8(videoId));
			return true;
		}
		if (sent && r.status == 308) {
			/* This chunk arrived; Google says how far it got */
			offset = std::clamp<int64_t>(ResumeOffset(r), 0, size);
			retries = 0;
			continue;
		}
		if (sent && r.status == 401) {
			session.accessToken.clear();
			if (!EnsureAccessToken(session, error) || ++retries > MAX_RETRIES) {
				return false;
			}
			continue;
		}
		if (sent && r.status >= 400 && r.status < 500) {
			error = ApiError(r);
			return false;
		}

		/* A network problem or a server error: wait, then ask the
		 * upload session how much it has before going on */
		if (++retries > MAX_RETRIES) {
			error = sent ? ApiError(r) : QString::fromStdString(err);
			return false;
		}
		blog(LOG_WARNING, "[Spectra] YouTube: chunk at %lld failed (%s), retrying", (long long)offset,
		     sent ? std::to_string(r.status).c_str() : err.c_str());
		if (!Backoff(retries, progress, offset, size)) {
			error = QStringLiteral("Cancelled");
			return false;
		}
		Response q;
		std::string qerr;
		if (Request("PUT", location, AuthHeaders(session, {"Content-Range: bytes */" + std::to_string(size)}),
			    {}, nullptr, nullptr, q, qerr, 60)) {
			if (q.status == 308) {
				offset = std::clamp<int64_t>(ResumeOffset(q), 0, size);
			} else if (q.status == 200 || q.status == 201) {
				videoId = Json(q).value(QStringLiteral("id")).toString();
				if (!videoId.isEmpty()) {
					return true;
				}
			} else if (q.status == 404) {
				error = QStringLiteral("The upload session expired; please upload again");
				return false;
			}
		}
	}
}

} // namespace YouTubeUpload
