#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <array>
#include <map>
#include <optional>

namespace obscura {

/* Obscura's embedded release-signing public key (obscura/keys.py) */
extern const char *const kPublicKeyB64;

/* The shareable learning file, .obx (port of obscura/definitions.py): gzipped
 * JSON {"payload": ..., "signature": base64 Ed25519 over the canonical
 * payload}. Holds channel statistics and seeds only, never message text. */
struct Definitions {
	static constexpr const char *kFormat = "obscura-defs/1";

	int defsVersion = 0;
	std::map<QString, std::array<int, 2>> channelStats;
	QStringList seeds;
	QString created;
	QString appVersion = QStringLiteral("spectra");

	QJsonValue Payload() const;
};

/* json.dumps(value, sort_keys=True, separators=(",", ":")) as UTF-8 */
QByteArray CanonicalJson(const QJsonValue &value);

QByteArray Gzip(const QByteArray &data);
std::optional<QByteArray> Gunzip(const QByteArray &data);

bool VerifySignature(const QByteArray &publicKeyB64, const QByteArray &message, const QByteArray &signatureB64);
/* Signs with an Ed25519 PKCS#8 PEM private key (Obscura's signing_key.pem) */
std::optional<QByteArray> Sign(const QByteArray &privatePem, const QByteArray &message, QString *error = nullptr);

std::optional<Definitions> LoadsObx(const QByteArray &blob, QString *error = nullptr, bool requireSignature = true,
				    const QByteArray &publicKeyB64 = kPublicKeyB64);
std::optional<Definitions> ReadObx(const QString &path, QString *error = nullptr, bool requireSignature = true);
bool WriteObx(const QString &path, const Definitions &defs, const QByteArray &privatePem, QString *error = nullptr);

/* Picks the newest obscura-defs-v<N>.obx newer than current from a GitHub
 * releases listing (api.github.com/repos/<repo>/releases) */
struct DefsUpdate {
	int version = 0;
	QString url;
	bool prerelease = false;
};
std::optional<DefsUpdate> FindDefsUpdate(const QByteArray &releasesJson, int currentVersion, bool includePrerelease);

} // namespace obscura
