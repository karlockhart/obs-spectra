#include "definitions.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

#include <zlib.h>

extern "C" {
#include <monocypher-ed25519.h>
#include <monocypher.h>
}

namespace obscura {

const char *const kPublicKeyB64 = "FB97iaKwlrdiVy9zqzaKmvqkHOwdrLl/EAjafij/CA8=";

QJsonValue Definitions::Payload() const
{
	QJsonObject stats;
	for (const auto &[k, v] : channelStats) {
		stats[k] = QJsonArray{v[0], v[1]};
	}
	QStringList sorted = seeds;
	sorted.sort();
	QString when = created;
	if (when.isEmpty()) {
		/* datetime.now(timezone.utc).isoformat(timespec="seconds") */
		when = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) +
		       QStringLiteral("+00:00");
	}
	return QJsonObject{{"format", kFormat},
			   {"defs_version", defsVersion},
			   {"created", when},
			   {"app_version", appVersion},
			   {"seeds", QJsonArray::fromStringList(sorted)},
			   {"channel_stats", stats}};
}

/* --- canonical JSON ---------------------------------------------------------- */

namespace {

void Escape(QByteArray &out, const QString &s)
{
	/* ensure_ascii=True: non-ASCII as \uXXXX (UTF-16 units, so surrogate pairs) */
	out += '"';
	for (QChar c : s) {
		const ushort u = c.unicode();
		switch (u) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			if (u < 0x20 || u > 0x7e) {
				out += QStringLiteral("\\u%1").arg(u, 4, 16, QChar('0')).toLatin1();
			} else {
				out += (char)u;
			}
		}
	}
	out += '"';
}

void Write(QByteArray &out, const QJsonValue &v)
{
	switch (v.type()) {
	case QJsonValue::Null:
	case QJsonValue::Undefined:
		out += "null";
		break;
	case QJsonValue::Bool:
		out += v.toBool() ? "true" : "false";
		break;
	case QJsonValue::Double: {
		const double d = v.toDouble();
		if (std::floor(d) == d && std::abs(d) < 9e15) {
			out += QByteArray::number((qint64)d);
		} else {
			out += QByteArray::number(d, 'g', 17);
		}
		break;
	}
	case QJsonValue::String:
		Escape(out, v.toString());
		break;
	case QJsonValue::Array: {
		out += '[';
		bool first = true;
		for (const QJsonValue &e : v.toArray()) {
			if (!first) {
				out += ',';
			}
			first = false;
			Write(out, e);
		}
		out += ']';
		break;
	}
	case QJsonValue::Object: {
		/* sort_keys: Python orders by code point */
		QJsonObject o = v.toObject();
		QStringList keys = o.keys();
		std::sort(keys.begin(), keys.end(),
			  [](const QString &a, const QString &b) { return a.toUcs4() < b.toUcs4(); });
		out += '{';
		bool first = true;
		for (const QString &k : keys) {
			if (!first) {
				out += ',';
			}
			first = false;
			Escape(out, k);
			out += ':';
			Write(out, o[k]);
		}
		out += '}';
		break;
	}
	}
}

} // namespace

QByteArray CanonicalJson(const QJsonValue &value)
{
	QByteArray out;
	Write(out, value);
	return out;
}

/* --- gzip -------------------------------------------------------------------- */

QByteArray Gzip(const QByteArray &data)
{
	z_stream zs = {};
	if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	QByteArray out(deflateBound(&zs, (uLong)data.size()) + 32, Qt::Uninitialized);
	zs.next_in = (Bytef *)data.constData();
	zs.avail_in = (uInt)data.size();
	zs.next_out = (Bytef *)out.data();
	zs.avail_out = (uInt)out.size();
	const int rc = deflate(&zs, Z_FINISH);
	out.resize(rc == Z_STREAM_END ? (qsizetype)zs.total_out : 0);
	deflateEnd(&zs);
	return out;
}

std::optional<QByteArray> Gunzip(const QByteArray &data)
{
	z_stream zs = {};
	if (inflateInit2(&zs, 15 + 16) != Z_OK) {
		return std::nullopt;
	}
	zs.next_in = (Bytef *)data.constData();
	zs.avail_in = (uInt)data.size();
	QByteArray out;
	char buf[16384];
	int rc = Z_OK;
	while (rc == Z_OK) {
		zs.next_out = (Bytef *)buf;
		zs.avail_out = sizeof(buf);
		rc = inflate(&zs, Z_NO_FLUSH);
		out.append(buf, (qsizetype)(sizeof(buf) - zs.avail_out));
		if (out.size() > 64 * 1024 * 1024) {
			rc = Z_MEM_ERROR;
		}
	}
	inflateEnd(&zs);
	if (rc != Z_STREAM_END) {
		return std::nullopt;
	}
	return out;
}

/* --- Ed25519 ----------------------------------------------------------------- */

bool VerifySignature(const QByteArray &publicKeyB64, const QByteArray &message, const QByteArray &signatureB64)
{
	if (publicKeyB64.isEmpty() || signatureB64.isEmpty()) {
		return false;
	}
	auto key = QByteArray::fromBase64Encoding(publicKeyB64, QByteArray::AbortOnBase64DecodingErrors);
	auto sig = QByteArray::fromBase64Encoding(signatureB64, QByteArray::AbortOnBase64DecodingErrors);
	if (!key || !sig || key.decoded.size() != 32 || sig.decoded.size() != 64) {
		return false;
	}
	return crypto_ed25519_check((const uint8_t *)sig.decoded.constData(), (const uint8_t *)key.decoded.constData(),
				    (const uint8_t *)message.constData(), (size_t)message.size()) == 0;
}

std::optional<QByteArray> Sign(const QByteArray &privatePem, const QByteArray &message, QString *error)
{
	/* PKCS#8 Ed25519: 302e020100300506032b657004220420 || 32-byte seed */
	static const QByteArray prefix = QByteArray::fromHex("302e020100300506032b657004220420");
	QByteArray b64;
	for (const QByteArray &line : privatePem.split('\n')) {
		if (!line.trimmed().startsWith("-----")) {
			b64 += line.trimmed();
		}
	}
	QByteArray der = QByteArray::fromBase64(b64);
	if (der.size() != 48 || !der.startsWith(prefix)) {
		if (error) {
			*error = QStringLiteral("Not an Ed25519 private key");
		}
		return std::nullopt;
	}
	uint8_t seed[32], secret[64], pub[32], sig[64];
	memcpy(seed, der.constData() + 16, 32);
	crypto_ed25519_key_pair(secret, pub, seed);
	crypto_ed25519_sign(sig, secret, (const uint8_t *)message.constData(), (size_t)message.size());
	crypto_wipe(secret, sizeof(secret));
	return QByteArray((const char *)sig, 64).toBase64();
}

/* --- .obx -------------------------------------------------------------------- */

std::optional<Definitions> LoadsObx(const QByteArray &blob, QString *error, bool requireSignature,
				    const QByteArray &publicKeyB64)
{
	auto fail = [&](const QString &msg) -> std::optional<Definitions> {
		if (error) {
			*error = msg;
		}
		return std::nullopt;
	};
	std::optional<QByteArray> json = Gunzip(blob);
	if (!json) {
		return fail(QStringLiteral("Not a valid .obx file: not gzip data"));
	}
	QJsonParseError err;
	QJsonObject container = QJsonDocument::fromJson(*json, &err).object();
	if (err.error != QJsonParseError::NoError || !container["payload"].isObject()) {
		return fail(QStringLiteral("Not a valid .obx file: %1").arg(err.errorString()));
	}
	const QJsonObject p = container["payload"].toObject();
	if (requireSignature &&
	    !VerifySignature(publicKeyB64, CanonicalJson(p), container["signature"].toString().toLatin1())) {
		return fail(QStringLiteral("Definitions signature is missing or does not match the trusted key"));
	}
	if (p["format"].toString() != QLatin1String(Definitions::kFormat)) {
		return fail(QStringLiteral("Unsupported definitions format: '%1'").arg(p["format"].toString()));
	}
	Definitions d;
	d.defsVersion = p["defs_version"].toInt();
	const QJsonObject stats = p["channel_stats"].toObject();
	for (auto it = stats.begin(); it != stats.end(); ++it) {
		QJsonArray a = it.value().toArray();
		d.channelStats[it.key()] = {a.at(0).toInt(), a.at(1).toInt()};
	}
	for (const QJsonValue &s : p["seeds"].toArray()) {
		d.seeds << s.toString();
	}
	d.created = p["created"].toString();
	d.appVersion = p["app_version"].toString();
	return d;
}

std::optional<Definitions> ReadObx(const QString &path, QString *error, bool requireSignature)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		if (error) {
			*error = file.errorString();
		}
		return std::nullopt;
	}
	return LoadsObx(file.readAll(), error, requireSignature);
}

bool WriteObx(const QString &path, const Definitions &defs, const QByteArray &privatePem, QString *error)
{
	const QJsonValue payload = defs.Payload();
	QString signature;
	if (!privatePem.isEmpty()) {
		std::optional<QByteArray> sig = Sign(privatePem, CanonicalJson(payload), error);
		if (!sig) {
			return false;
		}
		signature = QString::fromLatin1(*sig);
	}
	QJsonObject container{{"payload", payload}, {"signature", signature}};
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		if (error) {
			*error = file.errorString();
		}
		return false;
	}
	file.write(Gzip(QJsonDocument(container).toJson(QJsonDocument::Compact)));
	if (!file.commit()) {
		if (error) {
			*error = file.errorString();
		}
		return false;
	}
	return true;
}

std::optional<DefsUpdate> FindDefsUpdate(const QByteArray &releasesJson, int currentVersion, bool includePrerelease)
{
	static const QRegularExpression re(QStringLiteral("obscura-defs-v(\\d+)\\.obx$"),
					   QRegularExpression::CaseInsensitiveOption);
	std::optional<DefsUpdate> best;
	for (const QJsonValue &rv : QJsonDocument::fromJson(releasesJson).array()) {
		QJsonObject release = rv.toObject();
		const bool prerelease = release["prerelease"].toBool();
		if (release["draft"].toBool() || (prerelease && !includePrerelease)) {
			continue;
		}
		for (const QJsonValue &av : release["assets"].toArray()) {
			QJsonObject asset = av.toObject();
			QRegularExpressionMatch m = re.match(asset["name"].toString());
			if (!m.hasMatch()) {
				continue;
			}
			const int version = m.captured(1).toInt();
			if (version > currentVersion && (!best || version > best->version)) {
				best = DefsUpdate{version, asset["browser_download_url"].toString(), prerelease};
			}
		}
	}
	return best;
}

} // namespace obscura
