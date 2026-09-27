#include "lan-channel.hpp"

#include <monocypher.h>

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTcpSocket>

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace lucida::lan {

namespace {

const QByteArray kMagic("SPLAN1\n", 8); /* with its terminating zero */
constexpr int kHelloSize = 8 + 32 + 32;
constexpr int kMacSize = 16;
const QByteArray kConfirm("spectra-lan confirm");

void Random(uint8_t *out, size_t size)
{
	QRandomGenerator *rng = QRandomGenerator::system();
	for (size_t i = 0; i < size; i++) {
		out[i] = (uint8_t)rng->bounded(256);
	}
}

template<size_t N> bool AllZero(const std::array<uint8_t, N> &a)
{
	uint8_t acc = 0;
	for (uint8_t b : a) {
		acc |= b;
	}
	return acc == 0;
}

void Fail(QString *out, const QString &message)
{
	if (out) {
		*out = message;
	}
}

#ifdef _WIN32
QByteArray Protect(const QByteArray &plain, bool protect)
{
	DATA_BLOB in{(DWORD)plain.size(), (BYTE *)plain.constData()};
	DATA_BLOB out{};
	const BOOL ok =
		protect ? CryptProtectData(&in, L"Spectra LAN key", nullptr, nullptr, nullptr,
					   CRYPTPROTECT_UI_FORBIDDEN, &out)
			: CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
	if (!ok) {
		return QByteArray();
	}
	QByteArray result((const char *)out.pbData, (int)out.cbData);
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return result;
}
#endif

} // namespace

/* --- keys ------------------------------------------------------------------------ */

QString KeyId(const Key &pub)
{
	return QString::fromLatin1(QByteArray((const char *)pub.data(), (int)pub.size())
					   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

std::optional<Key> KeyFromId(const QString &id)
{
	auto decoded = QByteArray::fromBase64Encoding(id.toLatin1(), QByteArray::Base64UrlEncoding |
									     QByteArray::OmitTrailingEquals |
									     QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded || decoded.decoded.size() != 32) {
		return std::nullopt;
	}
	Key k;
	std::memcpy(k.data(), decoded.decoded.constData(), 32);
	return k;
}

QString Fingerprint(const Key &pub)
{
	/* 80 bits of a hash, in Crockford's base32 */
	static const char *alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
	uint8_t hash[10];
	crypto_blake2b(hash, sizeof(hash), pub.data(), pub.size());
	QString out;
	uint32_t buffer = 0;
	int bits = 0;
	for (uint8_t b : hash) {
		buffer = (buffer << 8) | b;
		bits += 8;
		while (bits >= 5) {
			if (out.size() % 5 == 4) {
				out += QChar('-');
			}
			out += QChar(alphabet[(buffer >> (bits - 5)) & 31]);
			bits -= 5;
		}
	}
	return out;
}

Identity Identity::Generate()
{
	Identity id;
	Random(id.secret.data(), id.secret.size());
	crypto_x25519_public_key(id.pub.data(), id.secret.data());
	return id;
}

std::optional<Identity> Identity::Load(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		Fail(error, f.errorString());
		return std::nullopt;
	}
	const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
	QByteArray secret = QByteArray::fromBase64(o.value("secret").toString().toLatin1());
	if (o.value("protected").toBool()) {
#ifdef _WIN32
		secret = Protect(secret, false);
#else
		secret.clear();
#endif
	}
	if (secret.size() != 32) {
		Fail(error, QStringLiteral("the key in %1 cannot be read").arg(QDir::toNativeSeparators(path)));
		return std::nullopt;
	}
	Identity id;
	std::memcpy(id.secret.data(), secret.constData(), 32);
	crypto_wipe(secret.data(), secret.size());
	crypto_x25519_public_key(id.pub.data(), id.secret.data());
	return id;
}

bool Identity::Save(const QString &path, QString *error) const
{
	QByteArray secretBytes((const char *)secret.data(), (int)secret.size());
	bool isProtected = false;
#ifdef _WIN32
	const QByteArray sealed = Protect(secretBytes, true);
	if (!sealed.isEmpty()) {
		crypto_wipe(secretBytes.data(), secretBytes.size());
		secretBytes = sealed;
		isProtected = true;
	}
#endif
	QJsonObject o{{"id", Id()},
		      {"secret", QString::fromLatin1(secretBytes.toBase64())},
		      {"protected", isProtected}};
	crypto_wipe(secretBytes.data(), secretBytes.size());
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		Fail(error, f.errorString());
		return false;
	}
	f.write(QJsonDocument(o).toJson());
	if (!f.commit()) {
		Fail(error, f.errorString());
		return false;
	}
	return true;
}

std::optional<Identity> Identity::LoadOrCreate(const QString &path, QString *error)
{
	if (QFileInfo::exists(path)) {
		return Load(path, error);
	}
	Identity id = Generate();
	if (!id.Save(path, error)) {
		return std::nullopt;
	}
	return id;
}

/* --- sockets --------------------------------------------------------------------- */

std::optional<QByteArray> ReadExactly(QTcpSocket &socket, qint64 size, int timeoutMs)
{
	QDeadlineTimer deadline(timeoutMs);
	QByteArray out;
	out.reserve((int)size);
	while (out.size() < size) {
		if (socket.bytesAvailable() == 0) {
			if (socket.state() != QAbstractSocket::ConnectedState || deadline.hasExpired() ||
			    !socket.waitForReadyRead((int)std::max<qint64>(1, deadline.remainingTime()))) {
				/* a closing socket can still hold data */
				if (socket.bytesAvailable() == 0) {
					return std::nullopt;
				}
			}
		}
		out += socket.read(size - out.size());
	}
	return out;
}

bool WriteAll(QTcpSocket &socket, const QByteArray &data, int timeoutMs)
{
	QDeadlineTimer deadline(timeoutMs);
	if (socket.write(data) != data.size()) {
		return false;
	}
	while (socket.bytesToWrite() > 0) {
		if (deadline.hasExpired() ||
		    !socket.waitForBytesWritten((int)std::max<qint64>(1, deadline.remainingTime()))) {
			return false;
		}
	}
	return true;
}

/* --- the channel ----------------------------------------------------------------- */

std::unique_ptr<Channel> Channel::Connect(QTcpSocket &socket, const Identity &self, const std::optional<Key> &expected,
					  int timeoutMs, QString *error)
{
	return Handshake(socket, self, true, expected, timeoutMs, error);
}

std::unique_ptr<Channel> Channel::Accept(QTcpSocket &socket, const Identity &self, int timeoutMs, QString *error)
{
	return Handshake(socket, self, false, std::nullopt, timeoutMs, error);
}

Channel::~Channel()
{
	crypto_wipe(sendKey.data(), sendKey.size());
	crypto_wipe(receiveKey.data(), receiveKey.size());
}

std::unique_ptr<Channel> Channel::Handshake(QTcpSocket &socket, const Identity &self, bool client,
					    const std::optional<Key> &expected, int timeoutMs, QString *error)
{
	Identity ephemeral = Identity::Generate();
	QByteArray mine = kMagic;
	mine.append((const char *)self.pub.data(), 32);
	mine.append((const char *)ephemeral.pub.data(), 32);

	std::optional<QByteArray> theirs;
	if (client) {
		if (!WriteAll(socket, mine, timeoutMs)) {
			Fail(error, QStringLiteral("could not send the handshake"));
			return nullptr;
		}
		theirs = ReadExactly(socket, kHelloSize, timeoutMs);
	} else {
		theirs = ReadExactly(socket, kHelloSize, timeoutMs);
		if (theirs && theirs->startsWith(kMagic) && !WriteAll(socket, mine, timeoutMs)) {
			theirs.reset();
		}
	}
	if (!theirs || !theirs->startsWith(kMagic)) {
		Fail(error, theirs ? QStringLiteral("not a Spectra install") : QStringLiteral("no handshake"));
		return nullptr;
	}

	Key peerStatic, peerEphemeral;
	std::memcpy(peerStatic.data(), theirs->constData() + 8, 32);
	std::memcpy(peerEphemeral.data(), theirs->constData() + 40, 32);
	if (expected && *expected != peerStatic) {
		Fail(error, QStringLiteral("a different install answered (its key changed)"));
		return nullptr;
	}

	std::unique_ptr<Channel> ch(new Channel(socket));
	ch->peer = peerStatic;

	/* the transcript: the client's hello, then the server's */
	const QByteArray &first = client ? mine : *theirs;
	const QByteArray &second = client ? *theirs : mine;
	crypto_blake2b_ctx ctx;
	crypto_blake2b_init(&ctx, 64);
	crypto_blake2b_update(&ctx, (const uint8_t *)first.constData(), first.size());
	crypto_blake2b_update(&ctx, (const uint8_t *)second.constData(), second.size());
	crypto_blake2b_final(&ctx, ch->transcript.data());

	/* ee, then the client's static with the server's ephemeral, then the
	 * client's ephemeral with the server's static */
	std::array<uint8_t, 32> dh[3];
	if (client) {
		crypto_x25519(dh[0].data(), ephemeral.secret.data(), peerEphemeral.data());
		crypto_x25519(dh[1].data(), self.secret.data(), peerEphemeral.data());
		crypto_x25519(dh[2].data(), ephemeral.secret.data(), peerStatic.data());
	} else {
		crypto_x25519(dh[0].data(), ephemeral.secret.data(), peerEphemeral.data());
		crypto_x25519(dh[1].data(), ephemeral.secret.data(), peerStatic.data());
		crypto_x25519(dh[2].data(), self.secret.data(), peerEphemeral.data());
	}
	crypto_wipe(ephemeral.secret.data(), ephemeral.secret.size());
	const bool weak = AllZero(dh[0]) || AllZero(dh[1]) || AllZero(dh[2]);

	uint8_t shared[96], keys[64];
	for (int i = 0; i < 3; i++) {
		std::memcpy(shared + 32 * i, dh[i].data(), 32);
		crypto_wipe(dh[i].data(), 32);
	}
	crypto_blake2b_keyed(keys, sizeof(keys), ch->transcript.data(), ch->transcript.size(), shared, sizeof(shared));
	crypto_wipe(shared, sizeof(shared));
	std::memcpy((client ? ch->sendKey : ch->receiveKey).data(), keys, 32);
	std::memcpy((client ? ch->receiveKey : ch->sendKey).data(), keys + 32, 32);
	crypto_wipe(keys, sizeof(keys));
	if (weak) {
		Fail(error, QStringLiteral("the other side sent an unusable key"));
		return nullptr;
	}

	/* each side shows it derived the same keys, i.e. holds its secret key */
	auto confirm = [&]() {
		std::optional<QByteArray> m = ch->Receive(timeoutMs);
		return m && *m == kConfirm;
	};
	const bool ok = client ? ch->Send(kConfirm) && confirm() : confirm() && ch->Send(kConfirm);
	if (!ok) {
		Fail(error, QStringLiteral("the other side could not prove who it is"));
		return nullptr;
	}
	return ch;
}

bool Channel::Send(const QByteArray &message)
{
	if (message.size() > kMaxMessage) {
		error = QStringLiteral("message too large");
		return false;
	}
	uint8_t nonce[24] = {0};
	for (int i = 0; i < 8; i++) {
		nonce[i] = (uint8_t)(sent >> (8 * i));
	}
	sent++;
	const quint32 length = (quint32)(kMacSize + message.size());
	QByteArray frame(4 + (int)length, Qt::Uninitialized);
	uint8_t *out = (uint8_t *)frame.data();
	for (int i = 0; i < 4; i++) {
		out[i] = (uint8_t)(length >> (8 * i));
	}
	crypto_aead_lock(out + 4 + kMacSize, out + 4, sendKey.data(), nonce, nullptr, 0,
			 (const uint8_t *)message.constData(), message.size());
	if (!WriteAll(socket, frame, 30000)) {
		error = QStringLiteral("the connection was lost");
		return false;
	}
	return true;
}

std::optional<QByteArray> Channel::Receive(int timeoutMs)
{
	QDeadlineTimer deadline(timeoutMs);
	std::optional<QByteArray> header = ReadExactly(socket, 4, timeoutMs);
	if (!header) {
		error = QStringLiteral("no answer");
		return std::nullopt;
	}
	const uint8_t *h = (const uint8_t *)header->constData();
	const quint32 length = h[0] | (h[1] << 8) | (h[2] << 16) | ((quint32)h[3] << 24);
	if (length < (quint32)kMacSize || length > (quint32)(kMacSize + kMaxMessage)) {
		error = QStringLiteral("a malformed message");
		return std::nullopt;
	}
	std::optional<QByteArray> body =
		ReadExactly(socket, length, (int)std::max<qint64>(1, deadline.remainingTime()));
	if (!body) {
		error = QStringLiteral("the connection was lost");
		return std::nullopt;
	}
	uint8_t nonce[24] = {0};
	for (int i = 0; i < 8; i++) {
		nonce[i] = (uint8_t)(received >> (8 * i));
	}
	QByteArray plain((int)length - kMacSize, Qt::Uninitialized);
	if (crypto_aead_unlock((uint8_t *)plain.data(), (const uint8_t *)body->constData(), receiveKey.data(), nonce,
			       nullptr, 0, (const uint8_t *)body->constData() + kMacSize, plain.size()) != 0) {
		error = QStringLiteral("a message failed to decrypt");
		return std::nullopt;
	}
	received++;
	return plain;
}

bool Channel::Poll(int timeoutMs)
{
	return socket.bytesAvailable() > 0 ||
	       (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(timeoutMs));
}

bool Channel::Alive() const
{
	return socket.state() == QAbstractSocket::ConnectedState;
}

bool Channel::SendJson(const QJsonObject &message)
{
	return Send(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

std::optional<QJsonObject> Channel::ReceiveJson(int timeoutMs)
{
	std::optional<QByteArray> m = Receive(timeoutMs);
	if (!m) {
		return std::nullopt;
	}
	QJsonParseError parse;
	QJsonDocument doc = QJsonDocument::fromJson(*m, &parse);
	if (!doc.isObject()) {
		error = QStringLiteral("not a JSON message");
		return std::nullopt;
	}
	return doc.object();
}

/* --- pairing --------------------------------------------------------------------- */

std::array<uint8_t, 32> PairNonce()
{
	std::array<uint8_t, 32> n;
	Random(n.data(), n.size());
	return n;
}

QByteArray PairCommit(const QByteArray &nonce)
{
	uint8_t hash[32];
	crypto_blake2b(hash, sizeof(hash), (const uint8_t *)nonce.constData(), nonce.size());
	return QByteArray((const char *)hash, sizeof(hash));
}

int PairCode(const Channel &channel, const QByteArray &initiatorNonce, const QByteArray &responderNonce)
{
	crypto_blake2b_ctx ctx;
	uint8_t hash[8];
	crypto_blake2b_init(&ctx, sizeof(hash));
	crypto_blake2b_update(&ctx, (const uint8_t *)"spectra-lan pair", 16);
	crypto_blake2b_update(&ctx, channel.Transcript().data(), channel.Transcript().size());
	crypto_blake2b_update(&ctx, (const uint8_t *)initiatorNonce.constData(), initiatorNonce.size());
	crypto_blake2b_update(&ctx, (const uint8_t *)responderNonce.constData(), responderNonce.size());
	crypto_blake2b_final(&ctx, hash);
	uint64_t v = 0;
	for (uint8_t b : hash) {
		v = (v << 8) | b;
	}
	return (int)(v % 1000000);
}

QString FormatPairCode(int code)
{
	const QString digits = QStringLiteral("%1").arg(code, 6, 10, QChar('0'));
	return digits.left(3) + QChar(' ') + digits.mid(3);
}

} // namespace lucida::lan
