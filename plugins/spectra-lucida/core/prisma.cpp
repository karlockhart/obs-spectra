#include "prisma.hpp"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <QDateTime>
#include <QThread>
#include <QSet>
#include <QRegularExpression>
#include <QCryptographicHash>
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
	out.retryAfter = r.retryAfter;
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
			DWORD retry = 0;
			size = sizeof(retry);
			if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &retry, &size, WINHTTP_NO_HEADER_INDEX)) {
				out.retryAfter = (int)retry;
			}
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

/* ------------------------------------------------------------------------- */
/* Pairing */

std::array<uint8_t, 32> NewEd25519Seed()
{
	std::array<uint8_t, 32> seed{};
	/* the operating system's cryptographic generator */
	QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(seed.data()), seed.size() / 4);
	return seed;
}

QString Ed25519PemFromSeed(const std::array<uint8_t, 32> &seed)
{
	/* PKCS#8: SEQUENCE { INTEGER 0, SEQUENCE { OID 1.3.101.112 }, OCTET STRING { OCTET STRING (32) } } */
	static const uint8_t prefix[] = {0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
					 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20};
	QByteArray der(reinterpret_cast<const char *>(prefix), sizeof(prefix));
	der.append(reinterpret_cast<const char *>(seed.data()), (qsizetype)seed.size());
	const QString b64 = QString::fromLatin1(der.toBase64());
	QString pem = QStringLiteral("-----BEGIN PRIVATE KEY-----\n");
	for (qsizetype i = 0; i < b64.size(); i += 64) {
		pem += b64.mid(i, 64) + QLatin1Char('\n');
	}
	pem += QStringLiteral("-----END PRIVATE KEY-----\n");
	return pem;
}

namespace {

/* Text as Prisma takes it: no control characters, at most `max` characters */
QString Plain(const QString &text, int max)
{
	QString out;
	for (const QChar c : text) {
		if (!c.isNull() && c.category() != QChar::Other_Control && c.category() != QChar::Other_Format) {
			out += c;
		}
	}
	return out.trimmed().left(max).trimmed();
}

QJsonObject PairingBody(const PairingInfo &info, const std::array<uint8_t, 32> &seed)
{
	const std::array<uint8_t, 32> pub = Ed25519PublicKey(seed);
	QJsonObject jwk;
	jwk["kty"] = "OKP";
	jwk["crv"] = "Ed25519";
	jwk["x"] = QString::fromLatin1(Base64Url(QByteArray(reinterpret_cast<const char *>(pub.data()), 32)));
	QJsonObject body;
	body["public_key"] = jwk;
	body["name"] = Plain(info.name, 64);
	body["app"] = info.app;
	body["machine"] = Plain(info.machine, 128);
	body["version"] = Plain(info.version, 32);
	body["scopes"] = QJsonArray::fromStringList(info.scopes);
	return body;
}

QString TrimUrl(QString url)
{
	url = url.trimmed();
	while (url.endsWith('/')) {
		url.chop(1);
	}
	return url;
}

PrismaResult PostJson(const QString &url, const QJsonObject &body, const HttpTransport &transport)
{
	HttpRequest req;
	req.method = "POST";
	req.url = url;
	req.headers = {{"Content-Type", "application/json"}};
	req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
	return ToResult(transport(req));
}

/* An approved pairing: the credentials file to write */
void Approved(PairingResult &r, const QJsonObject &o, const std::array<uint8_t, 32> &seed)
{
	QJsonObject creds;
	creds["url"] = TrimUrl(o.value("url").toString());
	creds["client_id"] = o.value("client_id").toString();
	creds["kid"] = o.value("kid").toString();
	creds["audience"] = o.value("audience").toString();
	creds["private_key"] = Ed25519PemFromSeed(seed);
	r.outcome = PairingResult::Outcome::Paired;
	r.credentialsJson = QJsonDocument(creds).toJson(QJsonDocument::Indented);
	r.tenant = o.value("tenant").toString();
	for (const QJsonValue &v : o.value("scopes").toArray()) {
		r.scopes << v.toString();
	}
	if (creds["client_id"].toString().isEmpty() || creds["kid"].toString().isEmpty() ||
	    creds["url"].toString().isEmpty()) {
		r.outcome = PairingResult::Outcome::Failed;
		r.error = QStringLiteral("Prisma's answer has no client_id, kid or url");
	}
}

} // namespace

PairingResult PairWithPassphrase(const QString &url, const QString &passphrase, const PairingInfo &info,
				 const std::array<uint8_t, 32> &seed, const HttpTransport &transport)
{
	QJsonObject body = PairingBody(info, seed);
	body["passphrase"] = passphrase;
	const PrismaResult res = PostJson(TrimUrl(url) + "/v1/pairings/passphrase", body, transport);
	PairingResult r;
	r.retryAfter = res.retryAfter;
	if (res.Ok()) {
		Approved(r, res.body.toObject(), seed);
	} else if (res.status == 401) {
		r.outcome = PairingResult::Outcome::WrongPassphrase;
	} else if (res.status == 403) {
		r.outcome = PairingResult::Outcome::NoPassphrase;
	} else if (res.status == 429) {
		r.outcome = PairingResult::Outcome::Locked;
	} else {
		r.error = res.Describe();
	}
	return r;
}

PairingResult StartCodePairing(const QString &url, const PairingInfo &info, const std::array<uint8_t, 32> &seed,
			       const HttpTransport &transport)
{
	const PrismaResult res = PostJson(TrimUrl(url) + "/v1/pairings", PairingBody(info, seed), transport);
	PairingResult r;
	if (!res.Ok()) {
		r.error = res.Describe();
		return r;
	}
	const QJsonObject o = res.body.toObject();
	r.outcome = PairingResult::Outcome::Pending;
	r.deviceCode = o.value("device_code").toString();
	r.userCode = o.value("user_code").toString();
	r.link = o.value("verification_uri_complete").toString();
	r.interval = std::max(1, o.value("interval").toInt(5));
	r.expiresIn = std::max(1, o.value("expires_in").toInt(600));
	if (r.deviceCode.isEmpty() || r.userCode.isEmpty()) {
		r.outcome = PairingResult::Outcome::Failed;
		r.error = QStringLiteral("Prisma's answer has no code");
	}
	return r;
}

PairingResult PollCodePairing(const QString &url, const QString &deviceCode, const std::array<uint8_t, 32> &seed,
			      const HttpTransport &transport)
{
	QJsonObject body;
	body["device_code"] = deviceCode;
	const PrismaResult res = PostJson(TrimUrl(url) + "/v1/pairings/poll", body, transport);
	PairingResult r;
	if (res.status == 404) {
		r.outcome = PairingResult::Outcome::Expired;
		return r;
	}
	if (!res.Ok()) {
		/* a network hiccup: keep polling until it expires */
		r.outcome = res.status == 0 || res.status >= 500 || res.status == 429 ? PairingResult::Outcome::Pending
										      : PairingResult::Outcome::Failed;
		r.error = res.Describe();
		return r;
	}
	const QJsonObject o = res.body.toObject();
	const QString status = o.value("status").toString();
	if (status == "approved") {
		Approved(r, o, seed);
	} else if (status == "pending") {
		r.outcome = PairingResult::Outcome::Pending;
	} else if (status == "denied") {
		r.outcome = PairingResult::Outcome::Denied;
	} else {
		r.outcome = PairingResult::Outcome::Expired;
	}
	return r;
}

/* ------------------------------------------------------------------------- */
/* Exporting videos */

QString ExportKey(const QString &file)
{
	const QString base = QFileInfo(file).completeBaseName();
	static const QRegularExpression stamp(QStringLiteral("(\\d{4})-(\\d{2})-(\\d{2}) (\\d{2})-(\\d{2})-(\\d{2})"));
	const QRegularExpressionMatch m = stamp.match(base);
	QString key;
	if (m.hasMatch()) {
		key = m.captured(1) + m.captured(2) + m.captured(3) + "-" + m.captured(4) + m.captured(5) +
		      m.captured(6);
		/* what comes before the time tells clips of the same second apart */
		QString rest = base.left(m.capturedStart()) + base.mid(m.capturedEnd());
		rest.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
		rest = rest.trimmed();
		while (rest.startsWith('_')) {
			rest.remove(0, 1);
		}
		while (rest.endsWith('_')) {
			rest.chop(1);
		}
		if (!rest.isEmpty()) {
			key += "-" + rest;
		}
	} else {
		key = base;
		key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
	}
	key = key.left(96);
	return key.isEmpty() ? QStringLiteral("video") : key;
}

QString VideoContentType(const QString &file)
{
	const QString ext = QFileInfo(file).suffix().toLower();
	if (ext == "mp4" || ext == "m4v") {
		return QStringLiteral("video/mp4");
	}
	if (ext == "mkv") {
		return QStringLiteral("video/x-matroska");
	}
	if (ext == "mov") {
		return QStringLiteral("video/quicktime");
	}
	if (ext == "webm") {
		return QStringLiteral("video/webm");
	}
	if (ext == "ts") {
		return QStringLiteral("video/mp2t");
	}
	if (ext == "flv") {
		return QStringLiteral("video/x-flv");
	}
	return QString();
}

namespace {

bool Retryable(int status)
{
	return status == 0 || status >= 500 || status == 408 || status == 429;
}

void Backoff(int attempt)
{
	QThread::msleep((unsigned long)std::min(30000, 1000 << std::min(attempt, 5)));
}

} // namespace

PrismaResult ExportVideo(PrismaClient &client, const VideoExport &video,
			 const std::function<void(qint64, qint64)> &progress, const std::function<bool()> &cancelled)
{
	auto stopped = [&]() {
		return cancelled && cancelled();
	};
	PrismaResult fail;
	const QString type = VideoContentType(video.file);
	if (type.isEmpty()) {
		fail.status = -1;
		fail.error = QStringLiteral("Prisma doesn't take %1 files").arg(QFileInfo(video.file).suffix());
		return fail;
	}
	QFile file(video.file);
	if (!file.open(QIODevice::ReadOnly)) {
		fail.error = file.errorString();
		return fail;
	}
	const qint64 bytes = file.size();
	if (bytes < 1 || bytes > 64ll * 1024 * 1024 * 1024) {
		fail.status = -1; /* never accepted: not worth retrying */
		fail.error = QStringLiteral("Prisma takes videos of 1 byte to 64 GiB");
		return fail;
	}

	/* the checksum Prisma resumes and deduplicates by */
	QCryptographicHash hash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		if (stopped()) {
			fail.error = QStringLiteral("cancelled");
			return fail;
		}
		hash.addData(file.read(4 * 1024 * 1024));
	}

	const QString path = QStringLiteral("/v1/spectra/sources/me/%1/%2").arg(video.kind, video.key);
	QJsonObject info = video.info;
	info["bytes"] = bytes;
	info["sha256"] = QString::fromLatin1(hash.result().toHex());
	info["content_type"] = type;
	if (!info.contains("file_name")) {
		info["file_name"] = QFileInfo(video.file).fileName();
	}
	info["file_name"] = Plain(info.value("file_name").toString(), 255);
	for (const auto &[field, max] : {std::pair{"title", 200}, std::pair{"game", 128}}) {
		if (info.contains(field)) {
			const QString text = Plain(info.value(field).toString(), max);
			if (text.isEmpty()) {
				info.remove(field);
			} else {
				info[field] = text;
			}
		}
	}
	if (info.contains("tags")) {
		/* up to 32 tags of 1 to 64 characters, none twice */
		QJsonArray tags;
		QStringList seen;
		for (const QJsonValue &t : info.value("tags").toArray()) {
			const QString tag = Plain(t.toString(), 64);
			if (!tag.isEmpty() && !seen.contains(tag) && tags.size() < 32) {
				seen << tag;
				tags.append(tag);
			}
		}
		if (tags.isEmpty()) {
			info.remove("tags");
		} else {
			info["tags"] = tags;
		}
	}
	if (info.value("started").isString()) {
		/* seconds since the epoch */
		const QDateTime started = QDateTime::fromString(info.value("started").toString(), Qt::ISODate);
		if (started.isValid()) {
			info["started"] = started.toMSecsSinceEpoch() / 1000.0;
		} else {
			info.remove("started");
		}
	}
	if (info.contains("duration") && info.value("duration").toDouble() <= 0.0) {
		info.remove("duration");
	}

	for (int round = 0; round < 4; round++) {
		PrismaResult put;
		for (int attempt = 0;; attempt++) {
			put = client.Put(path, info);
			if (put.Ok() || !Retryable(put.status) || attempt >= 5 || stopped()) {
				break;
			}
			Backoff(attempt);
		}
		if (!put.Ok()) {
			return put;
		}
		const QJsonObject answer = put.body.toObject();
		const QJsonValue upload = answer.value("upload");
		if (upload.isNull() || upload.isUndefined()) {
			/* this exact file is already there */
			if (progress) {
				progress(bytes, bytes);
			}
			PrismaResult done;
			done.status = put.status;
			done.body = answer.value("item");
			return done;
		}
		const QJsonObject plan = upload.toObject();
		const int parts = plan.value("parts").toInt();
		const qint64 partSize = (qint64)plan.value("part_size").toDouble();
		QSet<int> have;
		for (const QJsonValue &n : plan.value("uploaded_parts").toArray()) {
			have.insert(n.toInt());
		}
		std::vector<int> todo;
		for (int n = 1; n <= parts; n++) {
			if (!have.contains(n)) {
				todo.push_back(n);
			}
		}
		qint64 done = std::min<qint64>(bytes, (qint64)have.size() * partSize);
		if (progress) {
			progress(done, bytes);
		}

		bool restart = false;
		for (int pass = 0; pass < 4 && !todo.empty() && !restart; pass++) {
			std::vector<int> again;
			for (size_t i = 0; i < todo.size() && !restart; i += 100) {
				QJsonArray numbers;
				for (size_t j = i; j < std::min(todo.size(), i + 100); j++) {
					numbers.append(todo[j]);
				}
				QJsonObject ask;
				ask["numbers"] = numbers;
				PrismaResult urls;
				for (int attempt = 0;; attempt++) {
					urls = client.Post(path + "/parts", ask);
					if (urls.Ok() || !(Retryable(urls.status) || urls.status == 409) ||
					    attempt >= 5 || stopped()) {
						break;
					}
					Backoff(attempt);
				}
				if (urls.status == 409 || urls.status == 404) {
					/* the upload expired: PUT the file again */
					restart = true;
					break;
				}
				if (!urls.Ok()) {
					return urls;
				}
				for (const QJsonValue &v : urls.body.toObject().value("parts").toArray()) {
					if (stopped()) {
						fail.error = QStringLiteral("cancelled");
						return fail;
					}
					const QJsonObject part = v.toObject();
					const int number = part.value("number").toInt();
					const qint64 offset = (qint64)part.value("offset").toDouble();
					const qint64 size = (qint64)part.value("size").toDouble();
					if (!file.seek(offset)) {
						fail.error = file.errorString();
						return fail;
					}
					HttpRequest req;
					req.method = "PUT";
					req.url = part.value("url").toString();
					req.headers = {{"Content-Type", "application/octet-stream"}};
					req.body = file.read(size);
					req.timeoutMs = 600000;
					HttpResponse sent;
					for (int attempt = 0;; attempt++) {
						sent = client.SendRaw(req);
						if ((sent.status >= 200 && sent.status < 300) || attempt >= 3 ||
						    !(Retryable(sent.status)) || stopped()) {
							break;
						}
						Backoff(attempt);
					}
					if (sent.status >= 200 && sent.status < 300) {
						done += size;
						if (progress) {
							progress(std::min(done, bytes), bytes);
						}
					} else {
						/* 403: the URL expired; anything else: try the part again */
						again.push_back(number);
					}
				}
			}
			todo = again;
		}
		if (restart) {
			continue;
		}
		if (!todo.empty()) {
			fail.error = QStringLiteral("%1 part(s) of the upload kept failing").arg(todo.size());
			return fail;
		}

		for (int attempt = 0;; attempt++) {
			PrismaResult complete = client.Post(path + "/complete", QJsonObject());
			if (complete.Ok()) {
				if (progress) {
					progress(bytes, bytes);
				}
				return complete;
			}
			if (complete.status == 409) {
				/* parts missing or the wrong size: the next round's PUT says which */
				break;
			}
			if (!Retryable(complete.status) || attempt >= 5 || stopped()) {
				return complete;
			}
			Backoff(attempt);
		}
	}
	fail.error = QStringLiteral("Prisma kept finding parts of the upload missing");
	return fail;
}

} // namespace lucida
