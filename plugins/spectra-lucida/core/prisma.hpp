#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace lucida {

/* Prisma, the gateway that keeps Lucida's cloud copy (port of Prisma's
 * clients/python/prisma_client.py). Lucida never holds cloud credentials:
 * it proves who it is with its own Ed25519 key (a signed JWT sent to
 * /v1/token) and gets a 15-minute access token for everything else. */

/* A credentials file: {"url", "client_id", "kid", "audience", "private_key"}
 * with the key as a PKCS#8 PEM, as Prisma's panel or keygen writes it */
struct PrismaCredentials {
	QString url;
	QString clientId;
	QString kid;
	QString audience;
	std::array<uint8_t, 32> seed{}; /* the Ed25519 private key */

	static std::optional<PrismaCredentials> FromJson(const QByteArray &json, QString *error = nullptr);
	static std::optional<PrismaCredentials> FromFile(const QString &path, QString *error = nullptr);
};

/* The first prisma-*.json in the folders, in order */
QString FindCredentials(const QStringList &folders);

/* The 32-byte seed of an Ed25519 private key in a PKCS#8 PEM */
std::optional<std::array<uint8_t, 32>> Ed25519SeedFromPem(const QString &pem);
std::array<uint8_t, 32> Ed25519PublicKey(const std::array<uint8_t, 32> &seed);
QByteArray Base64Url(const QByteArray &data);

/* A signed EdDSA JWT proving this app holds the client's key (build_assertion) */
QByteArray BuildAssertion(const PrismaCredentials &creds, long long now, int lifetime = 120,
			  const QByteArray &jti = QByteArray());

struct HttpRequest {
	QByteArray method;
	QString url;
	QList<QPair<QByteArray, QByteArray>> headers;
	QByteArray body;
	int timeoutMs = 20000;
};

struct HttpResponse {
	int status = 0; /* 0: no response (see error) */
	QByteArray body;
	QString error;
};

using HttpTransport = std::function<HttpResponse(const HttpRequest &)>;

/* HTTPS (or plain HTTP) with WinHTTP, through the system proxy */
HttpResponse WinHttpSend(const HttpRequest &request);

struct PrismaResult {
	int status = 0; /* 0: no response */
	QJsonValue body;
	QString error;

	bool Ok() const { return status >= 200 && status < 300; }
	/* A readable reason for a failure */
	QString Describe() const;
};

class PrismaClient {
public:
	explicit PrismaClient(PrismaCredentials creds, HttpTransport transport = WinHttpSend);

	/* path is e.g. "/v1/lucida/sources"; the token is fetched and renewed as
	 * needed, and a 401 is retried once with a fresh one */
	PrismaResult Request(const QByteArray &method, const QString &path,
			     const std::optional<QJsonValue> &body = std::nullopt,
			     const QUrlQuery &query = QUrlQuery());
	PrismaResult Get(const QString &path, const QUrlQuery &query = QUrlQuery());
	PrismaResult Put(const QString &path, const QJsonValue &body);
	PrismaResult Post(const QString &path, const QJsonValue &body);
	PrismaResult Delete(const QString &path);

	/* Sends bytes to storage with a presigned POST ({url, fields}); no token */
	PrismaResult Upload(const QJsonObject &presigned, const QByteArray &data, const QString &filename);
	/* PUT a frame record at path, upload the bytes, confirm; returns the frame */
	PrismaResult UploadImage(const QString &path, const QByteArray &data, const QJsonObject &info);
	/* GET an absolute URL (e.g. a frame's download url); no token */
	HttpResponse Fetch(const QString &url);

	const PrismaCredentials &Credentials() const { return creds; }
	/* Seconds since the epoch (tests override) */
	std::function<double()> clock;

private:
	PrismaCredentials creds;
	HttpTransport transport;
	std::mutex mutex;
	QByteArray token;
	double tokenExpires = 0.0;

	bool Token(QByteArray *out, PrismaResult *failure);
	PrismaResult Send(const HttpRequest &request);
};

} // namespace lucida
