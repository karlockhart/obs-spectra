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
	int retryAfter = 0; /* s, from a Retry-After header */
};

using HttpTransport = std::function<HttpResponse(const HttpRequest &)>;

/* HTTPS (or plain HTTP) with WinHTTP, through the system proxy */
HttpResponse WinHttpSend(const HttpRequest &request);

struct PrismaResult {
	int status = 0; /* 0: no response */
	QJsonValue body;
	QString error;
	int retryAfter = 0;

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
	/* Any request as it is, without a token (a presigned part URL) */
	HttpResponse SendRaw(const HttpRequest &request) { return transport(request); }

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

/* ------------------------------------------------------------------------- */
/* Pairing: connecting to Prisma without a credentials file (pair() in
 * prisma_client.py). A new Ed25519 key is made here; Prisma registers its
 * public half and the private key never leaves the PC. */

struct PairingInfo {
	QString name;    /* "Spectra on DESKTOP-1" */
	QString app;     /* "spectra" */
	QString machine; /* the PC's name */
	QString version; /* the app's version */
	QStringList scopes;
};

struct PairingResult {
	enum class Outcome {
		Paired,          /* credentialsJson is ready to save */
		WrongPassphrase, /* 401: let the user try again */
		NoPassphrase,    /* 403: none is set; pair with a code */
		Locked,          /* 429: retryAfter s; a code still works */
		Pending,         /* code pairing: keep polling */
		Denied,          /* code pairing: the admin said no */
		Expired,         /* code pairing: start again */
		Failed,          /* anything else: error */
	};
	Outcome outcome = Outcome::Failed;
	QByteArray credentialsJson; /* {url, client_id, kid, audience, private_key} */
	QStringList scopes;         /* what was granted */
	QString tenant;
	int retryAfter = 0;
	QString error;
	/* code pairing */
	QString deviceCode; /* secret: kept in memory only */
	QString userCode;   /* "BCDF-GHJK", shown to the user */
	QString link;       /* verification_uri_complete */
	int interval = 5;
	int expiresIn = 600;
};

/* A fresh Ed25519 key for pairing; its PKCS#8 PEM */
std::array<uint8_t, 32> NewEd25519Seed();
QString Ed25519PemFromSeed(const std::array<uint8_t, 32> &seed);

/* POST /v1/pairings/passphrase: paired at once, or why not */
PairingResult PairWithPassphrase(const QString &url, const QString &passphrase, const PairingInfo &info,
				 const std::array<uint8_t, 32> &seed, const HttpTransport &transport = WinHttpSend);
/* POST /v1/pairings: the code to show (Outcome::Pending) */
PairingResult StartCodePairing(const QString &url, const PairingInfo &info, const std::array<uint8_t, 32> &seed,
			       const HttpTransport &transport = WinHttpSend);
/* POST /v1/pairings/poll, every `interval` s until it isn't Pending */
PairingResult PollCodePairing(const QString &url, const QString &deviceCode, const std::array<uint8_t, 32> &seed,
			      const HttpTransport &transport = WinHttpSend);

/* ------------------------------------------------------------------------- */
/* Exporting videos to Prisma's spectra plugin (export_video()): a resumable
 * multipart upload straight to storage, at /v1/spectra/sources/me/{kind}/{key} */

struct VideoExport {
	QString kind; /* clips | loops */
	QString key;  /* [A-Za-z0-9_-]{1,96}, stable for the file */
	QString file;
	QJsonObject info; /* title, game, started, duration, width, height, fps, tags */
};

/* The key for a file: its start time (yyyyMMdd-HHmmss) when the name has one,
 * else its name, cut to the allowed characters */
QString ExportKey(const QString &file);
/* video/mp4, video/x-matroska, ... from the extension; empty when Prisma takes none */
QString VideoContentType(const QString &file);

/* progress(done, total) in bytes; cancelled() stops between parts. Returns
 * the completed video record, or why it failed (a later call resumes). */
PrismaResult ExportVideo(PrismaClient &client, const VideoExport &video,
			 const std::function<void(qint64, qint64)> &progress = nullptr,
			 const std::function<bool()> &cancelled = nullptr);

} // namespace lucida
