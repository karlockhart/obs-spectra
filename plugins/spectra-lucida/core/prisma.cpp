#include "prisma.hpp"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QUrl>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <algorithm>
#include <cstring>

namespace lucida {

namespace {

constexpr double kRenewBefore = 60.0; /* s: renew the token this long before it expires */
constexpr int kUploadTimeoutMs = 120000;
const char *kAssertionType = "urn:ietf:params:oauth:client-assertion-type:jwt-bearer";

/* PKCS#8 for Ed25519: ... OID 1.3.101.112, then OCTET STRING { OCTET STRING (32) } */
const uint8_t kEd25519Oid[] = {0x06, 0x03, 0x2b, 0x65, 0x70};
const uint8_t kSeedPrefix[] = {0x04, 0x22, 0x04, 0x20};

double Now()
{
	return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

QJsonValue ParseBody(const QByteArray &raw)
{
	if (raw.trimmed().isEmpty()) {
		return QJsonValue();
	}
	QJsonParseError error;
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
	if (error.error != QJsonParseError::NoError) {
		return QString::fromUtf8(raw);
	}
	return doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
}

PrismaResult ToResult(const HttpResponse &r)
{
	PrismaResult out;
	out.status = r.status;
	out.error = r.error;
	out.body = ParseBody(r.body);
	return out;
}

QByteArray Json(const QJsonValue &v)
{
	if (v.isArray()) {
		return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
	}
	return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Credentials and signing */

QByteArray Base64Url(const QByteArray &data)
{
	return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

std::optional<std::array<uint8_t, 32>> Ed25519SeedFromPem(const QString &pem)
{
	QString b64;
	for (const QString &line : pem.split('\n')) {
		const QString l = line.trimmed();
		if (!l.isEmpty() && !l.startsWith(QLatin1String("-----"))) {
			b64 += l;
		}
	}
	const QByteArray der = QByteArray::fromBase64(b64.toLatin1());
	const auto oid = std::search(der.begin(), der.end(), std::begin(kEd25519Oid), std::end(kEd25519Oid),
				     [](char a, uint8_t b) { return (uint8_t)a == b; });
	if (oid == der.end()) {
		return std::nullopt;
	}
	const auto prefix = std::search(oid, der.end(), std::begin(kSeedPrefix), std::end(kSeedPrefix),
					[](char a, uint8_t b) { return (uint8_t)a == b; });
	if (prefix == der.end() || der.end() - prefix < (qsizetype)sizeof(kSeedPrefix) + 32) {
		return std::nullopt;
	}
	std::array<uint8_t, 32> seed{};
	std::memcpy(seed.data(), &*(prefix + sizeof(kSeedPrefix)), 32);
	return seed;
}

std::array<uint8_t, 32> Ed25519PublicKey(const std::array<uint8_t, 32> &seed)
{
	std::array<uint8_t, 32> copy = seed, pub{};
	uint8_t secret[64];
	crypto_ed25519_key_pair(secret, pub.data(), copy.data()); /* wipes copy */
	crypto_wipe(secret, sizeof(secret));
	return pub;
}

std::optional<PrismaCredentials> PrismaCredentials::FromJson(const QByteArray &json, QString *error)
{
	auto fail = [&](const QString &msg) -> std::optional<PrismaCredentials> {
		if (error) {
			*error = msg;
		}
		return std::nullopt;
	};
	const QJsonObject o = QJsonDocument::fromJson(json).object();
	PrismaCredentials c;
	c.url = o.value("url").toString().trimmed();
	while (c.url.endsWith('/')) {
		c.url.chop(1);
	}
	c.clientId = o.value("client_id").toString();
	c.kid = o.value("kid").toString();
	c.audience = o.value("audience").toString();
	if (c.url.isEmpty() || c.clientId.isEmpty() || c.kid.isEmpty() || c.audience.isEmpty()) {
		return fail(QStringLiteral("the credentials need url, client_id, kid and audience"));
	}
	const std::optional<std::array<uint8_t, 32>> seed = Ed25519SeedFromPem(o.value("private_key").toString());
	if (!seed) {
		return fail(QStringLiteral("private_key is not an Ed25519 key in PEM form"));
	}
	c.seed = *seed;
	return c;
}

std::optional<PrismaCredentials> PrismaCredentials::FromFile(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error) {
			*error = QStringLiteral("cannot read %1: %2")
					 .arg(QDir::toNativeSeparators(path), f.errorString());
		}
		return std::nullopt;
	}
	return FromJson(f.readAll(), error);
}

QString FindCredentials(const QStringList &folders)
{
	for (const QString &folder : folders) {
		if (folder.isEmpty()) {
			continue;
		}
		const QStringList found =
			QDir(folder).entryList({QStringLiteral("prisma-*.json")}, QDir::Files, QDir::Name);
		if (!found.isEmpty()) {
			return QDir(folder).filePath(found.first());
		}
	}
	return QString();
}

QByteArray BuildAssertion(const PrismaCredentials &creds, long long now, int lifetime, const QByteArray &jti_)
{
	QByteArray jti = jti_;
	if (jti.isEmpty()) {
		QByteArray random(16, Qt::Uninitialized);
		for (char &c : random) {
			c = (char)QRandomGenerator::system()->bounded(256);
		}
		jti = Base64Url(random);
	}
	const QJsonObject header{{"alg", "EdDSA"}, {"typ", "JWT"}, {"kid", creds.kid}};
	const QJsonObject claims{{"iss", creds.clientId}, {"sub", creds.clientId},
				 {"aud", creds.audience}, {"iat", now},
				 {"exp", now + lifetime}, {"jti", QString::fromLatin1(jti)}};
	const QByteArray input = Base64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.' +
				 Base64Url(QJsonDocument(claims).toJson(QJsonDocument::Compact));

	std::array<uint8_t, 32> seed = creds.seed;
	uint8_t secret[64], pub[32], signature[64];
	crypto_ed25519_key_pair(secret, pub, seed.data());
	crypto_ed25519_sign(signature, secret, reinterpret_cast<const uint8_t *>(input.constData()),
			    (size_t)input.size());
	crypto_wipe(secret, sizeof(secret));
	return input + '.' + Base64Url(QByteArray(reinterpret_cast<const char *>(signature), 64));
}

/* ------------------------------------------------------------------------- */
/* HTTP */

HttpResponse WinHttpSend(const HttpRequest &req)
{
	HttpResponse out;
#ifdef _WIN32
	const std::wstring wurl = req.url.toStdWString();
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256] = {}, path[4096] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 4096;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
	    (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
		out.error = QStringLiteral("not an http(s) URL: %1").arg(req.url);
		return out;
	}
	std::wstring object = path;
	if (parts.dwExtraInfoLength) {
		object += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	}
	const std::wstring method = QString::fromLatin1(req.method).toStdWString();
	std::wstring headers;
	for (const auto &[name, value] : req.headers) {
		headers += QString::fromLatin1(name + ": " + value + "\r\n").toStdWString();
	}

	HINTERNET session = WinHttpOpen(L"OBS-Spectra", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET request =
		connect ? WinHttpOpenRequest(connect, method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
					     WINHTTP_DEFAULT_ACCEPT_TYPES,
					     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
			: nullptr;
	bool ok = false;
	if (request) {
		WinHttpSetTimeouts(request, req.timeoutMs, req.timeoutMs, req.timeoutMs, req.timeoutMs);
		ok = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
					headers.empty() ? 0 : (DWORD)-1L,
					req.body.isEmpty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.constData(),
					(DWORD)req.body.size(), (DWORD)req.body.size(), 0) &&
		     WinHttpReceiveResponse(request, nullptr);
		if (ok) {
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			out.status = (int)status;
			DWORD available = 0;
			while (WinHttpQueryDataAvailable(request, &available) && available) {
				QByteArray chunk(available, Qt::Uninitialized);
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) {
					break;
				}
				out.body.append(chunk.constData(), read);
				if (out.body.size() > 64 * 1024 * 1024) {
					break;
				}
			}
		}
	}
	const DWORD lastError = GetLastError();
	if (request) {
		WinHttpCloseHandle(request);
	}
	if (connect) {
		WinHttpCloseHandle(connect);
	}
	if (session) {
		WinHttpCloseHandle(session);
	}
	if (!ok) {
		out.status = 0;
		out.error = QStringLiteral("could not reach %1 (error %2)")
				    .arg(QString::fromWCharArray(host))
				    .arg(lastError);
	}
#else
	out.error = QStringLiteral("HTTP is only implemented on Windows");
#endif
	return out;
}

/* ------------------------------------------------------------------------- */
/* Client */

QString PrismaResult::Describe() const
{
	if (status == 0) {
		return error.isEmpty() ? QStringLiteral("no response") : error;
	}
	QString detail;
	if (body.isObject()) {
		/* FastAPI: {"detail": "..."} or a 422's [{loc, msg}, ...]; /v1/token: OAuth's
		 * {"error", "error_description"}; API Gateway itself: {"message"} */
		const QJsonObject o = body.toObject();
		const QJsonValue d = o.value("detail");
		if (d.isString()) {
			detail = d.toString();
		} else if (d.isArray() && !d.toArray().isEmpty()) {
			const QJsonObject first = d.toArray().first().toObject();
			QStringList where;
			for (const QJsonValue &part : first.value("loc").toArray()) {
				where << (part.isDouble() ? QString::number(part.toInt()) : part.toString());
			}
			detail = QStringLiteral("%1 (%2)").arg(first.value("msg").toString(), where.join('.'));
		} else {
			detail = o.value("error_description")
					 .toString(o.value("error").toString(o.value("message").toString()));
		}
	} else if (body.isString()) {
		detail = body.toString().left(300);
	}
	return detail.isEmpty() ? QStringLiteral("HTTP %1").arg(status)
				: QStringLiteral("HTTP %1: %2").arg(status).arg(detail);
}

PrismaClient::PrismaClient(PrismaCredentials creds_, HttpTransport transport_)
	: clock(Now),
	  creds(std::move(creds_)),
	  transport(std::move(transport_))
{
}

PrismaResult PrismaClient::Send(const HttpRequest &request)
{
	return ToResult(transport(request));
}

bool PrismaClient::Token(QByteArray *out, PrismaResult *failure)
{
	std::lock_guard<std::mutex> lock(mutex);
	const double now = clock();
	if (token.isEmpty() || now > tokenExpires - kRenewBefore) {
		QUrlQuery form;
		form.addQueryItem("grant_type", "client_credentials");
		form.addQueryItem("client_assertion_type", kAssertionType);
		form.addQueryItem("client_assertion", QString::fromLatin1(BuildAssertion(creds, (long long)now)));
		HttpRequest req;
		req.method = "POST";
		req.url = creds.url + QStringLiteral("/v1/token");
		req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
		req.body = form.query(QUrl::FullyEncoded).toLatin1();
		PrismaResult r = Send(req);
		const QJsonObject o = r.body.toObject();
		if (!r.Ok() || !o.value("access_token").isString()) {
			token.clear();
			*failure = r;
			if (r.Ok()) {
				failure->status = 0;
				failure->error = QStringLiteral("the token response had no access_token");
			}
			return false;
		}
		token = o.value("access_token").toString().toLatin1();
		tokenExpires = now + o.value("expires_in").toDouble(900);
	}
	*out = token;
	return true;
}

PrismaResult PrismaClient::Request(const QByteArray &method, const QString &path, const std::optional<QJsonValue> &body,
				   const QUrlQuery &query)
{
	HttpRequest req;
	req.method = method;
	req.url = creds.url + path;
	if (!query.isEmpty()) {
		req.url += '?' + query.query(QUrl::FullyEncoded);
	}
	req.headers = {{"Content-Type", "application/json"}};
	if (body) {
		req.body = Json(*body);
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		QByteArray bearer;
		PrismaResult failure;
		if (!Token(&bearer, &failure)) {
			return failure;
		}
		req.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + bearer}};
		PrismaResult r = Send(req);
		if (r.status != 401 || attempt) {
			return r;
		}
		/* revoked or expired early: get a fresh token once */
		std::lock_guard<std::mutex> lock(mutex);
		token.clear();
	}
	return PrismaResult();
}

PrismaResult PrismaClient::Get(const QString &path, const QUrlQuery &query)
{
	return Request("GET", path, std::nullopt, query);
}

PrismaResult PrismaClient::Put(const QString &path, const QJsonValue &body)
{
	return Request("PUT", path, body);
}

PrismaResult PrismaClient::Post(const QString &path, const QJsonValue &body)
{
	return Request("POST", path, body);
}

PrismaResult PrismaClient::Delete(const QString &path)
{
	return Request("DELETE", path);
}

PrismaResult PrismaClient::Upload(const QJsonObject &presigned, const QByteArray &data, const QString &filename)
{
	QByteArray boundary = "----prisma";
	for (int i = 0; i < 16; i++) {
		boundary += QByteArray::number(QRandomGenerator::global()->bounded(16), 16);
	}
	const QJsonObject fields = presigned.value("fields").toObject();
	QByteArray body;
	for (auto it = fields.begin(); it != fields.end(); ++it) {
		body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + it.key().toUtf8() +
			"\"\r\n\r\n" + it.value().toString().toUtf8() + "\r\n";
	}
	/* storage ignores every field after the file, so it goes last */
	const QByteArray type =
		fields.value("Content-Type").toString(QStringLiteral("application/octet-stream")).toUtf8();
	body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename.toUtf8() +
		"\"\r\nContent-Type: " + type + "\r\n\r\n";
	body += data;
	body += "\r\n--" + boundary + "--\r\n";

	HttpRequest req;
	req.method = "POST";
	req.url = presigned.value("url").toString();
	req.headers = {{"Content-Type", "multipart/form-data; boundary=" + boundary}};
	req.body = body;
	req.timeoutMs = kUploadTimeoutMs;
	return Send(req);
}

PrismaResult PrismaClient::UploadImage(const QString &path, const QByteArray &data, const QJsonObject &info)
{
	PrismaResult put = Put(path, info);
	if (!put.Ok()) {
		return put;
	}
	PrismaResult upload = Upload(put.body.toObject().value("upload").toObject(), data, QFileInfo(path).fileName());
	if (!upload.Ok()) {
		return upload;
	}
	return Post(path + QStringLiteral("/uploaded"), QJsonObject());
}

HttpResponse PrismaClient::Fetch(const QString &url)
{
	HttpRequest req;
	req.method = "GET";
	req.url = url;
	req.timeoutMs = kUploadTimeoutMs;
	return transport(req);
}

} // namespace lucida
