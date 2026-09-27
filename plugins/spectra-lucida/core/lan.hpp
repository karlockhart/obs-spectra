#pragma once

#include "lan-channel.hpp"
#include "store.hpp"

#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

class QTcpServer;
class QUdpSocket;

namespace lucida::lan {

/* Sharing with other Spectra installs on the local network.
 *
 * Every install that has it on announces itself with a small UDP broadcast
 * every few seconds (its key, name and port; nothing else) and answers
 * requests on a TCP port. Only paired installs get anything: their log's
 * lines and screenshots and the clips folder, read-only. Pairing is done
 * once, by both people comparing a six-digit code; each side then keeps
 * the other's key. Nothing is copied: a peer's data is there while its
 * Spectra runs. */

constexpr quint16 kDiscoveryPort = 47650;
constexpr quint16 kDefaultPort = 47651;
/* Chunks of a file sent in reply */
constexpr int kChunk = 128 * 1024;
/* How long each person has to compare the codes */
constexpr int kPairTimeoutMs = 120000;

/* Another install, as its broadcasts and its answers describe it */
struct Peer {
	QString id; /* its key (KeyId) */
	QString name;
	QHostAddress address;
	quint16 port = 0;
	bool paired = false; /* by this install */

	/* From asking it, when paired */
	bool reachable = false;
	bool pairedThere = true; /* false: it forgot this install; pair again */
	QString game;            /* the game its Lucida is reading, if any */
	bool recording = false;  /* its loop recording is running */
	QString version;
	QString error; /* why the last request failed */

	QString Fingerprint() const;
};

struct PairedPeer {
	QString id;
	QString name;
	double since = 0.0;
};

/* Someone asks to pair with this install */
struct PairPrompt {
	QString id;
	QString name;
	QString fingerprint;
	QString code;
	/* set when the other side gives up; the prompt should close */
	std::shared_ptr<std::atomic<bool>> cancelled;
};

/* What this install shares, asked on the service's threads */
struct Provider {
	std::function<QString()> dbPath;
	std::function<QString()> clipsDir;
	/* {"game": exe name or "", "recording": bool} */
	std::function<QJsonObject()> status;
	/* Asks the person at this PC; resolves true to pair. Unset: refused. */
	std::function<std::future<bool>(const PairPrompt &)> askPair;
};

struct Result {
	bool ok = false;
	QJsonObject body;
	QString error;
	/* the peer does not know this install (any more) */
	bool notPaired = false;
};

class Service {
public:
	Service(Identity self, QString pairedFile, Provider provider);
	~Service();
	Service(const Service &) = delete;
	Service &operator=(const Service &) = delete;

	/* Starts announcing and serving; name is what peers see. The port is
	 * the first choice (another is picked if it is taken). */
	bool Start(const QString &name, QString *error = nullptr, quint16 port = kDefaultPort,
		   quint16 discoveryPort = kDiscoveryPort);
	void Stop();
	bool Running() const { return running; }
	quint16 Port() const { return port; }
	const Identity &Self() const { return self; }
	QString Name() const;
	QString version;

	/* Installs seen recently, by name */
	std::vector<Peer> Peers() const;
	std::optional<Peer> FindPeer(const QString &id) const;
	std::vector<PairedPeer> Paired() const;
	bool IsPaired(const QString &id) const;
	void Forget(const QString &id);

	/* One request to a peer; blocking, so call it off the UI thread. A
	 * reply with data hands each chunk to onData, which returns false to
	 * stop. */
	Result Request(const Peer &peer, const QJsonObject &request,
		       const std::function<bool(const QByteArray &)> &onData = {}, int timeoutMs = 15000);
	/* Pairs with a peer: confirm gets the code to compare and blocks until
	 * the person answers. Blocking. */
	Result Pair(const Peer &peer, const std::function<bool(const QString &code)> &confirm);

	/* Called on one of the service's threads when the peers or what they
	 * report change */
	std::function<void()> peersChanged;

	/* Timings (tests shorten them) */
	int beaconMs = 3000;
	int statusMs = 10000;
	int expireMs = 12000;
	/* Broadcast on every network (tests: off, only the extra targets) */
	bool broadcast = true;
	/* Beacons also go here (tests: another service on 127.0.0.1) */
	QList<QPair<QHostAddress, quint16>> extraTargets;

private:
	struct Seen {
		QString name;
		QHostAddress address;
		quint16 port = 0;
		qint64 at = 0; /* ms, steady */
	};
	struct Reported {
		bool reachable = false;
		bool pairedThere = true;
		QString game;
		bool recording = false;
		QString version;
		QString error;
	};

	Identity self;
	QString pairedFile;
	Provider provider;

	mutable std::mutex mutex;
	std::condition_variable wake;
	std::atomic<bool> stopping{false};
	bool running = false;
	QString name;
	quint16 port = 0;
	quint16 discoveryPort = 0;
	std::map<QString, Seen> seen;
	std::map<QString, Reported> reported;
	std::map<QString, PairedPeer> paired;

	std::thread discoveryThread, serverThread, statusThread;
	/* connections being answered, by socket handle (shut down on Stop) */
	std::set<qintptr> handlers;
	std::condition_variable handlersDone;
	std::atomic<bool> pairing{false};

	void LoadPaired();
	void SavePaired();
	void AddPaired(const QString &id, const QString &name);
	void Changed();

	void RunDiscovery(std::shared_ptr<std::promise<QString>> started);
	void RunServer(std::shared_ptr<std::promise<QString>> started, quint16 wanted);
	void RunStatus();
	QByteArray Beacon() const;
	void Handle(qintptr handle);
	void Answer(Channel &channel);
	void AnswerPair(Channel &channel, const QJsonObject &request);
	void AnswerLines(Channel &channel, const QJsonObject &request);
	void AnswerShot(Channel &channel, const QJsonObject &request);
	void AnswerClips(Channel &channel);
	void AnswerClip(Channel &channel, const QJsonObject &request);
	bool SendFile(Channel &channel, const QString &path, qint64 offset, QJsonObject header);

	std::mutex storeMutex;
	std::unique_ptr<Store> store;
	Store *OpenStore();

	std::unique_ptr<Channel> Open(const Peer &peer, QTcpSocket &socket, int timeoutMs, QString *error);
};

/* The clip files a peer may download from a folder, newest first */
struct ClipFile {
	QString name;
	qint64 size = 0;
	qint64 modified = 0; /* s since the epoch */
};
std::vector<ClipFile> ListClips(const QString &folder, int limit = 1000);
/* The file for a clip name, if it is one of ListClips' (no paths, no other
 * files) */
std::optional<QString> ClipPath(const QString &folder, const QString &name);

/* A log line as it is sent to a peer (the fields Prisma uses, so the
 * viewer shows both alike) */
QJsonObject LineToJson(const LogLine &line, const QString &source);

} // namespace lucida::lan
