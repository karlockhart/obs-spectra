#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

class QTcpSocket;

namespace lucida::lan {

/* Talking to other Spectra installs on the local network: who this install
 * is, and an authenticated, encrypted connection to another one.
 *
 * An install is its X25519 key pair; its id is the public key. A connection
 * starts with a handshake in which each side proves it holds its key (three
 * Diffie-Hellmans over the static and ephemeral keys, as in Noise's XX
 * pattern without identity hiding), then every message is sealed with
 * ChaCha20-Poly1305 under a key per direction. Everything is Monocypher. */

using Key = std::array<uint8_t, 32>;

/* A public key as text: base64url without padding (43 characters) */
QString KeyId(const Key &pub);
std::optional<Key> KeyFromId(const QString &id);
/* A short form for people to compare, e.g. "K3QF-7XAB-P2MC-9D4T" */
QString Fingerprint(const Key &pub);

struct Identity {
	Key secret{};
	Key pub{};

	static Identity Generate();
	/* The key file: {"secret": ...}, protected for the Windows user with
	 * DPAPI. Missing or unreadable: nullopt and error. */
	static std::optional<Identity> Load(const QString &path, QString *error = nullptr);
	bool Save(const QString &path, QString *error = nullptr) const;
	/* Loads the key, or makes and saves a new one */
	static std::optional<Identity> LoadOrCreate(const QString &path, QString *error = nullptr);

	QString Id() const { return KeyId(pub); }
};

/* One connection after the handshake. Blocking: use it on one thread, the
 * one that owns the socket. */
class Channel {
public:
	/* The largest message; files go in several */
	static constexpr int kMaxMessage = 256 * 1024;

	/* Runs the handshake on a connected socket. As the client, expected is
	 * the key the server must have (else anyone answering is accepted). */
	static std::unique_ptr<Channel> Connect(QTcpSocket &socket, const Identity &self,
						const std::optional<Key> &expected, int timeoutMs,
						QString *error = nullptr);
	static std::unique_ptr<Channel> Accept(QTcpSocket &socket, const Identity &self, int timeoutMs,
					       QString *error = nullptr);
	~Channel();

	/* The other side's key, proven by the handshake */
	const Key &Peer() const { return peer; }
	QString PeerId() const { return KeyId(peer); }
	/* Unique to this connection, the same on both sides (for pairing codes) */
	const std::array<uint8_t, 64> &Transcript() const { return transcript; }

	bool Send(const QByteArray &message);
	/* nullopt on timeout, a closed socket or a message that fails to open */
	std::optional<QByteArray> Receive(int timeoutMs);
	bool SendJson(const QJsonObject &message);
	std::optional<QJsonObject> ReceiveJson(int timeoutMs);

	/* Waits up to timeoutMs for the other side to send something */
	bool Poll(int timeoutMs);
	/* The connection is still up */
	bool Alive() const;

	QString Error() const { return error; }

private:
	Channel(QTcpSocket &socket) : socket(socket) {}

	QTcpSocket &socket;
	Key peer{};
	std::array<uint8_t, 64> transcript{};
	std::array<uint8_t, 32> sendKey{}, receiveKey{};
	uint64_t sent = 0, received = 0;
	QString error;

	static std::unique_ptr<Channel> Handshake(QTcpSocket &socket, const Identity &self, bool client,
						  const std::optional<Key> &expected, int timeoutMs, QString *error);
};

/* Reads exactly size bytes, waiting up to timeoutMs overall */
std::optional<QByteArray> ReadExactly(QTcpSocket &socket, qint64 size, int timeoutMs);
bool WriteAll(QTcpSocket &socket, const QByteArray &data, int timeoutMs);

/* Pairing: each side picks a nonce; the code both people compare comes from
 * the connection and both nonces. The side answering commits to its nonce
 * before it sees the other's, so a machine in the middle cannot steer the
 * codes of its two connections to match. */
std::array<uint8_t, 32> PairNonce();
QByteArray PairCommit(const QByteArray &nonce);
/* A six-digit code, 000000-999999 */
int PairCode(const Channel &channel, const QByteArray &initiatorNonce, const QByteArray &responderNonce);
QString FormatPairCode(int code);

} // namespace lucida::lan
