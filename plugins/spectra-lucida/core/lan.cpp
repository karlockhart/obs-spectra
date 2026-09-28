#include "lan.hpp"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lucida::lan {

namespace {

constexpr int kConnectMs = 3000;
constexpr int kHandshakeMs = 5000;
constexpr int kRequestMs = 10000;
constexpr int kStatusMs = 4000;
constexpr int kMaxHandlers = 16;
constexpr int kMaxLines = 500;
constexpr int kMaxName = 64;
constexpr int kMaxBeacon = 1024;
constexpr qint64 kTargetsEveryMs = 30000;
constexpr int kMaxGossip = 64;
constexpr size_t kMaxAdded = 64;
/* Announcements also go to this group: some routers pass multicast between
 * Wi-Fi clients but drop broadcasts (administratively scoped, so it stays
 * on the local network) */
const QHostAddress kMulticastGroup(QStringLiteral("239.255.76.51"));
const QStringList kClipPatterns{QStringLiteral("*.mp4"), QStringLiteral("*.mkv"), QStringLiteral("*.mov")};

qint64 SteadyMs()
{
	static QElapsedTimer clock = [] {
		QElapsedTimer t;
		t.start();
		return t;
	}();
	return clock.elapsed();
}

double Now()
{
	return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

/* A name from the network: one line, printable, not too long */
QString CleanName(const QString &text)
{
	QString out;
	for (const QChar c : text) {
		if (c.isPrint()) {
			out += c;
		}
	}
	return out.trimmed().left(kMaxName);
}

QString B64(const QByteArray &data)
{
	return QString::fromLatin1(data.toBase64());
}

QByteArray FromB64(const QJsonValue &v)
{
	return QByteArray::fromBase64(v.toString().toLatin1());
}

QByteArray Bytes(const std::array<uint8_t, 32> &a)
{
	return QByteArray((const char *)a.data(), (int)a.size());
}

/* A reply's error code, for people */
QString ErrorText(const QString &code)
{
	if (code == QLatin1String("not_paired")) {
		return QStringLiteral("it is not paired with this PC");
	}
	if (code == QLatin1String("busy")) {
		return QStringLiteral("it is busy with another pairing request");
	}
	if (code == QLatin1String("refused")) {
		return QStringLiteral("it said no");
	}
	if (code == QLatin1String("no_shot")) {
		return QStringLiteral("no screenshot was kept for this line");
	}
	if (code == QLatin1String("no_such_clip")) {
		return QStringLiteral("the clip is not in its clips folder any more");
	}
	if (code == QLatin1String("missing_file")) {
		return QStringLiteral("the file is gone");
	}
	if (code == QLatin1String("no_storage")) {
		return QStringLiteral("it does not store other PCs' recordings");
	}
	if (code == QLatin1String("bad_request")) {
		return QStringLiteral("it did not understand what was sent");
	}
	if (code == QLatin1String("write_failed")) {
		return QStringLiteral("it could not save what was sent");
	}
	return code;
}

void ShutdownHandle(qintptr handle)
{
#ifdef _WIN32
	::shutdown((SOCKET)handle, SD_BOTH);
#else
	::shutdown((int)handle, SHUT_RDWR);
#endif
}

void CloseHandle(qintptr handle)
{
#ifdef _WIN32
	::closesocket((SOCKET)handle);
#else
	::close((int)handle);
#endif
}

/* Hands each accepted connection's handle over instead of a QTcpSocket, so
 * it can be answered on its own thread */
class Listener : public QTcpServer {
public:
	std::function<void(qintptr)> onConnection;

protected:
	void incomingConnection(qintptr handle) override { onConnection(handle); }
};

/* The networks multicast can go out on */
QList<QNetworkInterface> MulticastInterfaces()
{
	QList<QNetworkInterface> out;
	for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
		const auto flags = i.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
		    !(flags & QNetworkInterface::CanMulticast) || (flags & QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const QNetworkAddressEntry &e : i.addressEntries()) {
			if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) {
				out << i;
				break;
			}
		}
	}
	return out;
}

QList<QHostAddress> BroadcastAddresses()
{
	QList<QHostAddress> out;
	for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
		const auto flags = i.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
		    !(flags & QNetworkInterface::CanBroadcast) || (flags & QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const QNetworkAddressEntry &e : i.addressEntries()) {
			if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && !e.broadcast().isNull() &&
			    !out.contains(e.broadcast())) {
				out << e.broadcast();
			}
		}
	}
	return out;
}

} // namespace

QString Peer::Fingerprint() const
{
	std::optional<Key> key = KeyFromId(id);
	return key ? lan::Fingerprint(*key) : QString();
}

/* --- what is shared ---------------------------------------------------------------- */

std::vector<ClipFile> ListClips(const QString &folder, int limit)
{
	std::vector<ClipFile> out;
	if (folder.isEmpty()) {
		return out;
	}
	const QFileInfoList files =
		QDir(folder).entryInfoList(kClipPatterns, QDir::Files | QDir::NoDotAndDotDot, QDir::Time);
	for (const QFileInfo &f : files) {
		if ((int)out.size() >= limit) {
			break;
		}
		if (f.isSymLink()) {
			continue;
		}
		out.push_back(ClipFile{f.fileName(), f.size(), f.lastModified().toSecsSinceEpoch()});
	}
	return out;
}

std::optional<QString> ClipPath(const QString &folder, const QString &name)
{
	if (folder.isEmpty() || name.isEmpty() || name.startsWith('.') || name.contains('/') || name.contains('\\') ||
	    name.contains(':')) {
		return std::nullopt;
	}
	const QFileInfo f(QDir(folder).filePath(name));
	const QString suffix = f.suffix().toLower();
	if (!f.exists() || !f.isFile() || f.isSymLink() ||
	    (suffix != QLatin1String("mp4") && suffix != QLatin1String("mkv") && suffix != QLatin1String("mov")) ||
	    f.absoluteDir().canonicalPath() != QDir(folder).canonicalPath()) {
		return std::nullopt;
	}
	return f.absoluteFilePath();
}

QJsonObject LineToJson(const LogLine &l, const QString &source)
{
	QJsonArray labels;
	for (const QString &label : l.labels) {
		labels.append(label);
	}
	QJsonObject o{{"source", source},
		      {"line_id", QString::number(l.id)},
		      {"sort_ts", l.sortTs},
		      {"seq", l.seq},
		      {"channel", l.channel},
		      {"region", l.region},
		      {"labels", labels},
		      {"body", l.body},
		      {"has_frame", l.frameId.has_value()}};
	if (l.clock) {
		o.insert("clock", *l.clock);
	}
	return o;
}

/* --- the service ------------------------------------------------------------------ */

Service::Service(Identity self_, QString pairedFile_, Provider provider_)
	: self(self_),
	  pairedFile(std::move(pairedFile_)),
	  provider(std::move(provider_))
{
	LoadPaired();
}

Service::~Service()
{
	Stop();
}

QString Service::Name() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return name;
}

bool Service::Start(const QString &name_, QString *error, quint16 wantedPort, quint16 discoveryPort_)
{
	if (running) {
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		name = CleanName(name_);
		seen.clear();
		reported.clear();
	}
	stopping = false;
	discoveryPort = discoveryPort_;

	auto serverStarted = std::make_shared<std::promise<QString>>();
	std::future<QString> serverResult = serverStarted->get_future();
	serverThread = std::thread(&Service::RunServer, this, serverStarted, wantedPort);
	QString failure = serverResult.get();
	if (failure.isEmpty()) {
		auto discoveryStarted = std::make_shared<std::promise<QString>>();
		std::future<QString> discoveryResult = discoveryStarted->get_future();
		discoveryThread = std::thread(&Service::RunDiscovery, this, discoveryStarted);
		failure = discoveryResult.get();
		if (!failure.isEmpty()) {
			failure = QStringLiteral("cannot listen for other PCs on UDP port %1: %2")
					  .arg(discoveryPort)
					  .arg(failure);
		}
	} else {
		failure = QStringLiteral("cannot accept connections: %1").arg(failure);
	}
	if (!failure.isEmpty()) {
		stopping = true;
		wake.notify_all();
		if (serverThread.joinable()) {
			serverThread.join();
		}
		if (discoveryThread.joinable()) {
			discoveryThread.join();
		}
		if (error) {
			*error = failure;
		}
		return false;
	}
	statusThread = std::thread(&Service::RunStatus, this);
	running = true;
	return true;
}

void Service::Stop()
{
	if (!running) {
		return;
	}
	stopping = true;
	wake.notify_all();
	for (std::thread *t : {&discoveryThread, &serverThread, &statusThread}) {
		if (t->joinable()) {
			t->join();
		}
	}
	{
		/* connections still being answered: cut them so they finish now */
		std::unique_lock<std::mutex> lock(mutex);
		for (qintptr h : handlers) {
			ShutdownHandle(h);
		}
		handlersDone.wait(lock, [this] { return handlers.empty(); });
		seen.clear();
		reported.clear();
	}
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		store.reset();
	}
	running = false;
	Changed();
}

void Service::Changed()
{
	if (peersChanged) {
		peersChanged();
	}
}

/* --- paired installs --------------------------------------------------------------- */

void Service::LoadPaired()
{
	QFile f(pairedFile);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	const QJsonArray list = QJsonDocument::fromJson(f.readAll()).object().value("peers").toArray();
	std::lock_guard<std::mutex> lock(mutex);
	for (const QJsonValue &v : list) {
		const QJsonObject o = v.toObject();
		const QString id = o.value("id").toString();
		if (KeyFromId(id)) {
			PairedPeer p{id, CleanName(o.value("name").toString()), o.value("since").toDouble()};
			const QHostAddress address(o.value("address").toString());
			const int port = o.value("port").toInt();
			if (!address.isNull() && port > 0 && port <= 65535) {
				p.address = address;
				p.port = (quint16)port;
			}
			paired[id] = p;
		}
	}
}

void Service::SavePaired()
{
	QJsonArray list;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto &[id, p] : paired) {
			QJsonObject o{{"id", id}, {"name", p.name}, {"since", p.since}};
			if (!p.address.isNull() && p.port) {
				o.insert("address", p.address.toString());
				o.insert("port", (int)p.port);
			}
			list.append(o);
		}
	}
	QDir().mkpath(QFileInfo(pairedFile).absolutePath());
	QSaveFile f(pairedFile);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"peers", list}}).toJson());
		f.commit();
	}
}

void Service::AddPaired(const QString &id, const QString &peerName, const QHostAddress &address, quint16 peerPort)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		PairedPeer p{id, peerName, Now(), address, peerPort};
		auto s = seen.find(id);
		if (s != seen.end() && (p.address.isNull() || !p.port)) {
			p.address = s->second.address;
			p.port = s->second.port;
		}
		paired[id] = p;
		reported.erase(id); /* ask it again straight away */
	}
	SavePaired();
	wake.notify_all();
	Changed();
}

std::vector<PairedPeer> Service::Paired() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<PairedPeer> out;
	for (const auto &[id, p] : paired) {
		out.push_back(p);
	}
	std::sort(out.begin(), out.end(), [](const PairedPeer &a, const PairedPeer &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return out;
}

bool Service::IsPaired(const QString &id) const
{
	std::lock_guard<std::mutex> lock(mutex);
	return paired.count(id) > 0;
}

void Service::Forget(const QString &id)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		paired.erase(id);
		reported.erase(id);
	}
	SavePaired();
	Changed();
}

std::vector<Peer> Service::Peers() const
{
	std::vector<Peer> out;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto &[id, s] : seen) {
			Peer p;
			p.id = id;
			p.name = s.name;
			p.address = s.address;
			p.port = s.port;
			p.paired = paired.count(id) > 0;
			auto r = reported.find(id);
			if (r != reported.end()) {
				p.reachable = r->second.reachable;
				p.pairedThere = r->second.pairedThere;
				p.game = r->second.game;
				p.recording = r->second.recording;
				p.storage = r->second.storage;
				p.version = r->second.version;
				p.error = r->second.error;
			}
			out.push_back(p);
		}
	}
	std::sort(out.begin(), out.end(),
		  [](const Peer &a, const Peer &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
	return out;
}

std::optional<Peer> Service::FindPeer(const QString &id) const
{
	for (const Peer &p : Peers()) {
		if (p.id == id) {
			return p;
		}
	}
	return std::nullopt;
}

/* --- finding each other --------------------------------------------------------------- */

bool Service::Saw(const QString &id, const QString &peerName, const QHostAddress &address, quint16 peerPort)
{
	Seen &s = seen[id];
	const bool changed = s.at == 0 || s.name != peerName || s.address != address || s.port != peerPort;
	s.name = peerName;
	s.address = address;
	s.port = peerPort;
	s.at = std::max<qint64>(SteadyMs(), 1);
	auto p = paired.find(id);
	if (p != paired.end() && (p->second.address != address || p->second.port != peerPort)) {
		p->second.address = address;
		p->second.port = peerPort;
		pairedMoved = true;
	}
	return changed;
}

void Service::SaveIfMoved()
{
	bool moved;
	{
		std::lock_guard<std::mutex> lock(mutex);
		moved = pairedMoved;
		pairedMoved = false;
	}
	if (moved) {
		SavePaired();
	}
}

QList<QPair<QHostAddress, quint16>> Service::UnicastTargets() const
{
	QList<QPair<QHostAddress, quint16>> out = extraTargets;
	std::lock_guard<std::mutex> lock(mutex);
	auto add = [&](const QHostAddress &a) {
		if (!a.isNull() && !a.isLoopback() && !out.contains({a, discoveryPort})) {
			out.append({a, discoveryPort});
		}
	};
	for (const auto &[address, tcpPort] : added) {
		add(QHostAddress(address));
	}
	/* paired PCs where they were last seen, in case broadcasts miss them */
	for (const auto &[id, p] : paired) {
		add(p.address);
	}
	return out;
}

QStringList Service::LocalAddresses()
{
	QStringList out;
	for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
		const auto flags = i.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
		    (flags & QNetworkInterface::IsLoopBack) || i.type() == QNetworkInterface::Virtual ||
		    i.humanReadableName().startsWith(QLatin1String("vEthernet"))) {
			continue;
		}
		for (const QNetworkAddressEntry &e : i.addressEntries()) {
			const QHostAddress ip = e.ip();
			if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLinkLocal() &&
			    !out.contains(ip.toString())) {
				out << ip.toString();
			}
		}
	}
	return out;
}

QByteArray Service::Beacon() const
{
	QJsonObject o{{"spectra-lan", 1}, {"id", self.Id()}, {"port", (int)port}};
	{
		std::lock_guard<std::mutex> lock(mutex);
		o.insert("name", name);
	}
	if (stopping) {
		o.insert("bye", true);
	}
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void Service::RunDiscovery(std::shared_ptr<std::promise<QString>> started)
{
	QUdpSocket udp;
	auto bind = [&]() {
		return udp.bind(QHostAddress::AnyIPv4, discoveryPort,
				QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint);
	};
	if (!bind()) {
		started->set_value(udp.errorString().isEmpty() ? QStringLiteral("unknown error") : udp.errorString());
		return;
	}
	started->set_value(QString());
	qint64 boundAt = SteadyMs();

	QList<QHostAddress> targets;
	QList<QNetworkInterface> groups;
	qint64 targetsAt = -kTargetsEveryMs;
	qint64 nextBeacon = 0;
	auto announce = [&]() {
		const QByteArray beacon = Beacon();
		if (broadcast) {
			if (SteadyMs() - targetsAt >= kTargetsEveryMs) {
				targets = BroadcastAddresses();
				/* networks come and go: join the group on each one there is now
				 * (joining one already joined just fails) */
				groups = MulticastInterfaces();
				for (const QNetworkInterface &i : groups) {
					udp.joinMulticastGroup(kMulticastGroup, i);
				}
				udp.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
				targetsAt = SteadyMs();
			}
			for (const QHostAddress &a : targets) {
				udp.writeDatagram(beacon, a, discoveryPort);
			}
			for (const QNetworkInterface &i : groups) {
				udp.setMulticastInterface(i);
				udp.writeDatagram(beacon, kMulticastGroup, discoveryPort);
			}
		}
		for (const auto &[address, targetPort] : UnicastTargets()) {
			udp.writeDatagram(beacon, address, targetPort);
		}
	};

	while (!stopping) {
		const qint64 now = SteadyMs();
		if (now >= nextBeacon) {
			announce();
			nextBeacon = now + beaconMs;
			/* installs that went quiet are gone */
			bool expired = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				for (auto it = seen.begin(); it != seen.end();) {
					if (now - it->second.at > expireMs) {
						reported.erase(it->first);
						it = seen.erase(it);
						expired = true;
					} else {
						++it;
					}
				}
			}
			if (expired) {
				Changed();
			}
		}
		if (!udp.waitForReadyRead(200)) {
			/* Windows reports an ICMP "port unreachable" for an earlier
			 * datagram as a reset, and Qt then closes the socket: open it
			 * again */
			if (udp.state() != QAbstractSocket::BoundState && SteadyMs() - boundAt >= 1000) {
				udp.close();
				bind();
				boundAt = SteadyMs();
				targetsAt = -kTargetsEveryMs; /* join the group again */
			}
			continue;
		}
		bool changed = false;
		while (udp.hasPendingDatagrams()) {
			const QNetworkDatagram d = udp.receiveDatagram(kMaxBeacon);
			const QJsonObject o = QJsonDocument::fromJson(d.data()).object();
			const QString id = o.value("id").toString();
			const int peerPort = o.value("port").toInt();
			if (o.value("spectra-lan").toInt() != 1 || id == self.Id() || !KeyFromId(id) || peerPort <= 0 ||
			    peerPort > 65535) {
				continue;
			}
			QHostAddress from = d.senderAddress();
			bool ok = false;
			const quint32 v4 = from.toIPv4Address(&ok);
			if (ok) {
				from = QHostAddress(v4);
			}
			bool fresh = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (o.value("bye").toBool()) {
					changed |= seen.erase(id) > 0;
					reported.erase(id);
					continue;
				}
				fresh = seen.count(id) == 0;
				changed |= Saw(id, CleanName(o.value("name").toString()), from, (quint16)peerPort);
			}
			/* answer a newcomer directly: its broadcasts may reach this PC
			 * while this PC's do not reach it */
			if (fresh) {
				udp.writeDatagram(Beacon(), d.senderAddress(), (quint16)d.senderPort());
			}
		}
		SaveIfMoved();
		if (changed) {
			wake.notify_all(); /* new paired installs are asked how they are */
			Changed();
		}
	}
	/* say goodbye so the others drop this install now */
	announce();
}

/* Gossip: where this PC sees the PCs it is paired with, for another paired
 * PC to find those that moved (a new address from DHCP, another network).
 * Nothing in it is trusted: an address is only used to connect, and the
 * connection proves the key. */
QJsonArray Service::Gossip(const QString &asker) const
{
	QJsonArray out;
	std::lock_guard<std::mutex> lock(mutex);
	for (const auto &[id, s] : seen) {
		if (id != asker && paired.count(id) && !s.address.isNull() && s.port) {
			out.append(QJsonObject{{"id", id}, {"address", s.address.toString()}, {"port", (int)s.port}});
		}
		if (out.size() >= kMaxGossip) {
			break;
		}
	}
	return out;
}

void Service::TakeGossip(const QString &from, const QJsonArray &peers)
{
	std::lock_guard<std::mutex> lock(mutex);
	for (const QJsonValue &v : peers) {
		const QJsonObject o = v.toObject();
		const QString id = o.value("id").toString();
		const QHostAddress address(o.value("address").toString());
		const int peerPort = o.value("port").toInt();
		/* only about PCs this one is paired with and does not see itself */
		if (id == self.Id() || id == from || !paired.count(id) || seen.count(id) || address.isNull() ||
		    address.protocol() != QAbstractSocket::IPv4Protocol || peerPort <= 0 || peerPort > 65535) {
			continue;
		}
		gossip[id] = {address, (quint16)peerPort};
	}
}

void Service::ProbeQuiet(const std::set<QString> &online, std::map<QString, qint64> &probed)
{
	/* paired PCs not heard from: try where they were last, and where the
	 * others say they are now */
	std::vector<Peer> tries;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const qint64 now = SteadyMs();
		for (const auto &[id, p] : paired) {
			if (online.count(id) || seen.count(id)) {
				continue;
			}
			auto last = probed.find(id);
			if (last != probed.end() && now - last->second < std::max(statusMs, 1000)) {
				continue;
			}
			probed[id] = now;
			QList<QPair<QHostAddress, quint16>> where;
			auto g = gossip.find(id);
			if (g != gossip.end()) {
				where.append(g->second);
			}
			if (!p.address.isNull() && p.port && !where.contains({p.address, p.port})) {
				where.append({p.address, p.port});
			}
			for (const auto &[address, peerPort] : where) {
				Peer candidate;
				candidate.id = id;
				candidate.name = p.name;
				candidate.address = address;
				candidate.port = peerPort;
				candidate.paired = true;
				tries.push_back(candidate);
			}
		}
	}
	for (const Peer &p : tries) {
		if (stopping) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (seen.count(p.id)) {
				continue; /* found at the first address */
			}
		}
		const Result r =
			Request(p, QJsonObject{{"op", "hello"}, {"name", Name()}, {"port", (int)port}}, {}, kStatusMs);
		if (!r.ok) {
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			const QString peerName = CleanName(r.body.value("name").toString());
			Saw(p.id, peerName.isEmpty() ? p.name : peerName, p.address, p.port);
			gossip.erase(p.id);
			reported.erase(p.id); /* ask it how it is straight away */
		}
		TakeGossip(p.id, r.body.value("peers").toArray());
		SaveIfMoved();
		Changed();
	}
}

/* Asks each paired install how it is: what it plays, whether it still
 * knows this one */
void Service::RunStatus()
{
	std::map<QString, qint64> asked, probed;
	while (!stopping) {
		const qint64 now = SteadyMs();
		std::vector<Peer> due;
		std::set<QString> online;
		for (const Peer &p : Peers()) {
			online.insert(p.id);
			auto a = asked.find(p.id);
			bool known, wanted;
			{
				std::lock_guard<std::mutex> lock(mutex);
				known = reported.count(p.id) > 0;
				/* PCs found by address are asked too: nothing else may keep
				 * them listed */
				wanted = p.paired || addedIds.count(p.id) > 0;
			}
			if (wanted && (!known || a == asked.end() || now - a->second >= statusMs)) {
				due.push_back(p);
			}
		}
		ProbeQuiet(online, probed);
		for (const Peer &p : due) {
			if (stopping) {
				break;
			}
			asked[p.id] = SteadyMs();
			const Result r = Request(p, QJsonObject{{"op", "hello"}, {"name", Name()}, {"port", (int)port}},
						 {}, kStatusMs);
			Reported rep;
			rep.reachable = r.ok;
			rep.error = r.ok ? QString() : r.error;
			if (r.ok) {
				TakeGossip(p.id, r.body.value("peers").toArray());
				rep.pairedThere = r.body.value("paired").toBool();
				rep.game = r.body.value("game").toString().left(kMaxName);
				rep.recording = r.body.value("recording").toBool();
				rep.storage = r.body.value("storage").toBool();
				rep.version = r.body.value("version").toString().left(kMaxName);
			}
			bool changed = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (seen.count(p.id)) {
					/* it answered, so it is there even if its beacons get lost */
					if (r.ok) {
						seen[p.id].at = std::max<qint64>(SteadyMs(), 1);
					}
					const Reported old = reported.count(p.id) ? reported[p.id] : Reported{};
					changed = !reported.count(p.id) || old.reachable != rep.reachable ||
						  old.pairedThere != rep.pairedThere || old.game != rep.game ||
						  old.recording != rep.recording || old.storage != rep.storage ||
						  old.error != rep.error;
					reported[p.id] = rep;
				}
			}
			if (changed) {
				Changed();
			}
		}
		std::unique_lock<std::mutex> lock(mutex);
		wake.wait_for(lock, std::chrono::milliseconds(1000), [this] { return stopping.load(); });
	}
}

/* --- answering -------------------------------------------------------------------- */

void Service::RunServer(std::shared_ptr<std::promise<QString>> started, quint16 wanted)
{
	Listener server;
	server.onConnection = [this](qintptr handle) {
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping || handlers.size() >= kMaxHandlers) {
			CloseHandle(handle);
			return;
		}
		handlers.insert(handle);
		std::thread(&Service::Handle, this, handle).detach();
	};
	if (!server.listen(QHostAddress::AnyIPv4, wanted) && !server.listen(QHostAddress::AnyIPv4, 0)) {
		started->set_value(server.errorString().isEmpty() ? QStringLiteral("unknown error")
								  : server.errorString());
		return;
	}
	port = server.serverPort();
	started->set_value(QString());
	while (!stopping) {
		server.waitForNewConnection(250);
	}
	server.close();
}

void Service::Handle(qintptr handle)
{
	auto finished = [this, handle] {
		std::lock_guard<std::mutex> lock(mutex);
		handlers.erase(handle);
		handlersDone.notify_all();
	};
	QTcpSocket socket;
	if (!socket.setSocketDescriptor(handle)) {
		finished();
		CloseHandle(handle);
		return;
	}
	QString error;
	std::unique_ptr<Channel> channel = Channel::Accept(socket, self, kHandshakeMs, &error);
	if (channel) {
		QHostAddress from = socket.peerAddress();
		bool ok = false;
		const quint32 v4 = from.toIPv4Address(&ok);
		if (ok) {
			from = QHostAddress(v4);
		}
		Answer(*channel, from);
	}
	/* forget the handle before the socket closes it and it can be reused */
	finished();
	socket.disconnectFromHost();
	if (socket.state() != QAbstractSocket::UnconnectedState) {
		socket.waitForDisconnected(1000);
	}
}

void Service::Answer(Channel &channel, const QHostAddress &from)
{
	std::optional<QJsonObject> request = channel.ReceiveJson(kRequestMs);
	if (!request) {
		return;
	}
	const QString op = request->value("op").toString();
	const bool trusted = IsPaired(channel.PeerId());
	if (op == QLatin1String("hello")) {
		/* a hello says where the asker answers: it is listed even if its
		 * beacons never arrive (e.g. it added this PC by address) */
		const int callerPort = request->value("port").toInt();
		if (callerPort > 0 && callerPort <= 65535 && channel.PeerId() != self.Id() && !from.isNull()) {
			bool changed;
			{
				std::lock_guard<std::mutex> lock(mutex);
				changed = Saw(channel.PeerId(), CleanName(request->value("name").toString()), from,
					      (quint16)callerPort);
				/* it reached this PC directly: announce to it directly too */
				if (added.size() < kMaxAdded) {
					added.insert({from.toIPv4Address(), (quint16)callerPort});
				}
			}
			SaveIfMoved();
			if (changed) {
				wake.notify_all();
				Changed();
			}
		}
		QJsonObject reply{{"id", self.Id()}, {"name", Name()}, {"paired", trusted}, {"version", version}};
		if (trusted) {
			reply.insert("peers", Gossip(channel.PeerId()));
		}
		if (trusted && provider.status) {
			const QJsonObject status = provider.status();
			for (auto it = status.begin(); it != status.end(); ++it) {
				reply.insert(it.key(), it.value());
			}
		}
		channel.SendJson(reply);
	} else if (op == QLatin1String("pair")) {
		AnswerPair(channel, *request);
	} else if (!trusted) {
		channel.SendJson(QJsonObject{{"error", "not_paired"}});
	} else if (op == QLatin1String("lines")) {
		AnswerLines(channel, *request);
	} else if (op == QLatin1String("shot")) {
		AnswerShot(channel, *request);
	} else if (op == QLatin1String("clips")) {
		AnswerClips(channel);
	} else if (op == QLatin1String("clip")) {
		AnswerClip(channel, *request);
	} else if (op.startsWith(QLatin1String("store_"))) {
		if (provider.store) {
			QString peerName;
			{
				std::lock_guard<std::mutex> lock(mutex);
				auto p = paired.find(channel.PeerId());
				peerName = p != paired.end() ? p->second.name : QString();
			}
			provider.store(channel, channel.PeerId(), peerName, *request);
		} else {
			channel.SendJson(QJsonObject{{"error", "no_storage"}});
		}
	} else {
		channel.SendJson(QJsonObject{{"error", "unknown_request"}});
	}
}

void Service::AnswerPair(Channel &channel, const QJsonObject &request)
{
	if (!provider.askPair) {
		channel.SendJson(QJsonObject{{"error", "refused"}});
		return;
	}
	if (pairing.exchange(true)) {
		channel.SendJson(QJsonObject{{"error", "busy"}});
		return;
	}
	struct Done {
		std::atomic<bool> &flag;
		~Done() { flag = false; }
	} done{pairing};

	const QString peerName = CleanName(request.value("name").toString());
	const QByteArray mine = Bytes(PairNonce());
	if (!channel.SendJson(QJsonObject{{"commit", B64(PairCommit(mine))}})) {
		return;
	}
	std::optional<QJsonObject> theirsMsg = channel.ReceiveJson(kRequestMs);
	const QByteArray theirs = theirsMsg ? FromB64(theirsMsg->value("nonce")) : QByteArray();
	if (theirs.size() != 32 || !channel.SendJson(QJsonObject{{"nonce", B64(mine)}})) {
		return;
	}

	PairPrompt prompt;
	prompt.id = channel.PeerId();
	prompt.name = peerName;
	prompt.fingerprint = lan::Fingerprint(channel.Peer());
	prompt.code = FormatPairCode(PairCode(channel, theirs, mine));
	prompt.cancelled = std::make_shared<std::atomic<bool>>(false);
	std::future<bool> answer = provider.askPair(prompt);

	/* wait for the person here, noticing if the other side gives up first */
	std::optional<bool> ours, confirmed;
	QDeadlineTimer deadline(kPairTimeoutMs);
	while (!stopping && !deadline.hasExpired()) {
		if (answer.wait_for(std::chrono::milliseconds(confirmed ? 200 : 0)) == std::future_status::ready) {
			try {
				ours = answer.get();
			} catch (...) {
				ours = false;
			}
			break;
		}
		if (!confirmed) {
			if (channel.Poll(200)) {
				std::optional<QJsonObject> m = channel.ReceiveJson(kRequestMs);
				confirmed = m && m->value("confirm").toBool();
				if (!*confirmed) {
					break;
				}
			} else if (!channel.Alive()) {
				break;
			}
		}
	}
	if (!ours) {
		prompt.cancelled->store(true);
		return;
	}
	if (!confirmed) {
		std::optional<QJsonObject> m = channel.ReceiveJson(kPairTimeoutMs);
		confirmed = m && m->value("confirm").toBool();
	}
	if (*ours && *confirmed) {
		AddPaired(prompt.id, peerName, QHostAddress(), 0);
	}
	channel.SendJson(QJsonObject{{"accepted", *ours}});
}

Store *Service::OpenStore()
{
	const QString path = provider.dbPath ? provider.dbPath() : QString();
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		store.reset();
		return nullptr;
	}
	if (!store || store->Path() != path) {
		store = std::make_unique<Store>(path);
		if (!store->Open() || !store->IsOpen()) {
			store.reset();
		}
	}
	return store.get();
}

void Service::AnswerLines(Channel &channel, const QJsonObject &request)
{
	Query q;
	q.text = request.value("text").toString().left(256);
	q.channel = request.value("channel").toString().left(kMaxName);
	q.label = request.value("label").toString().left(kMaxName);
	q.region = request.value("region").toString().left(kMaxName);
	if (request.value("from").isDouble()) {
		q.from = request.value("from").toInteger();
	}
	if (request.value("to").isDouble()) {
		q.to = request.value("to").toInteger();
	}
	q.withShot = request.value("with_shot").toBool();
	q.limit = std::clamp(request.value("limit").toInt(kMaxLines), 1, kMaxLines);

	std::vector<LogLine> lines;
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		if (Store *s = OpenStore()) {
			lines = s->Find(q);
		}
	}
	/* newest last; drop the oldest if it would not fit one message */
	size_t first = 0;
	QByteArray reply;
	while (true) {
		QJsonArray list;
		for (size_t i = first; i < lines.size(); i++) {
			list.append(LineToJson(lines[i], self.Id()));
		}
		reply = QJsonDocument(QJsonObject{{"lines", list}}).toJson(QJsonDocument::Compact);
		if (reply.size() <= Channel::kMaxMessage || first >= lines.size()) {
			break;
		}
		first += std::max<size_t>(1, (lines.size() - first) / 10);
	}
	channel.Send(reply);
}

void Service::AnswerShot(Channel &channel, const QJsonObject &request)
{
	const long long id = request.value("line").toString().toLongLong();
	std::optional<Frame> frame;
	std::optional<spectra::Rect> rect;
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		Store *s = OpenStore();
		std::optional<LogLine> line = s ? s->Line(id) : std::nullopt;
		if (line && line->frameId) {
			frame = s->GetFrame(*line->frameId);
			for (const LogLine &l : s->FrameLines(*line->frameId)) {
				if (l.id == id) {
					rect = l.rect;
				}
			}
			if (!rect) {
				rect = line->rect;
			}
		}
	}
	if (!frame) {
		channel.SendJson(QJsonObject{{"error", "no_shot"}});
		return;
	}
	QJsonObject header{{"sort_ts", frame->sortTs}, {"width", frame->width}, {"height", frame->height}};
	if (rect) {
		header.insert("rect", QJsonArray{rect->x0, rect->y0, rect->x1, rect->y1});
	}
	SendFile(channel, frame->path, 0, header);
}

void Service::AnswerClips(Channel &channel)
{
	QJsonArray list;
	for (const ClipFile &c : ListClips(provider.clipsDir ? provider.clipsDir() : QString())) {
		list.append(QJsonObject{{"name", c.name}, {"size", c.size}, {"modified", c.modified}});
	}
	channel.SendJson(QJsonObject{{"clips", list}});
}

void Service::AnswerClip(Channel &channel, const QJsonObject &request)
{
	std::optional<QString> path =
		ClipPath(provider.clipsDir ? provider.clipsDir() : QString(), request.value("name").toString());
	if (!path) {
		channel.SendJson(QJsonObject{{"error", "no_such_clip"}});
		return;
	}
	SendFile(channel, *path, std::max<qint64>(0, request.value("offset").toInteger()), QJsonObject());
}

bool Service::SendFile(Channel &channel, const QString &path, qint64 offset, QJsonObject header)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		channel.SendJson(QJsonObject{{"error", "missing_file"}});
		return false;
	}
	offset = std::min(offset, f.size());
	header.insert("data", true);
	header.insert("size", f.size());
	header.insert("offset", offset);
	if (!channel.SendJson(header) || !f.seek(offset)) {
		return false;
	}
	while (!f.atEnd()) {
		if (stopping) {
			return false;
		}
		const QByteArray chunk = f.read(kChunk);
		if (chunk.isEmpty() || !channel.Send(chunk)) {
			return false;
		}
	}
	return channel.Send(QByteArray()); /* the end */
}

/* --- asking ----------------------------------------------------------------------- */

std::unique_ptr<Channel> Service::Open(const Peer &peer, QTcpSocket &socket, int timeoutMs, QString *error)
{
	socket.connectToHost(peer.address, peer.port);
	if (!socket.waitForConnected(kConnectMs)) {
		if (error) {
			*error = socket.errorString();
		}
		return nullptr;
	}
	return Channel::Connect(socket, self, KeyFromId(peer.id), timeoutMs, error);
}

Result Service::Request(const Peer &peer, const QJsonObject &request,
			const std::function<bool(const QByteArray &)> &onData, int timeoutMs)
{
	Result r;
	QTcpSocket socket;
	std::unique_ptr<Channel> channel = Open(peer, socket, kHandshakeMs, &r.error);
	if (!channel) {
		return r;
	}
	std::optional<QJsonObject> header;
	if (!channel->SendJson(request) || !(header = channel->ReceiveJson(timeoutMs))) {
		r.error = channel->Error();
		return r;
	}
	r.body = *header;
	if (header->contains("error")) {
		const QString code = header->value("error").toString();
		r.notPaired = code == QLatin1String("not_paired");
		r.error = ErrorText(code);
		return r;
	}
	if (header->value("data").toBool()) {
		while (true) {
			std::optional<QByteArray> chunk = channel->Receive(timeoutMs);
			if (!chunk) {
				r.error = channel->Error();
				return r;
			}
			if (chunk->isEmpty()) {
				break;
			}
			if (onData && !onData(*chunk)) {
				socket.abort();
				r.error = QStringLiteral("stopped");
				return r;
			}
		}
	}
	r.ok = true;
	return r;
}

Result Service::Pair(const Peer &peer, const std::function<bool(const QString &code)> &confirm)
{
	Result r;
	QTcpSocket socket;
	std::unique_ptr<Channel> channel = Open(peer, socket, kHandshakeMs, &r.error);
	if (!channel) {
		return r;
	}
	auto fail = [&](const QString &why) {
		r.error = why.isEmpty() ? channel->Error() : why;
		return r;
	};
	if (!channel->SendJson(QJsonObject{{"op", "pair"}, {"name", Name()}})) {
		return fail(QString());
	}
	std::optional<QJsonObject> commit = channel->ReceiveJson(kRequestMs);
	if (!commit) {
		return fail(QString());
	}
	if (commit->contains("error")) {
		return fail(ErrorText(commit->value("error").toString()));
	}
	const QByteArray mine = Bytes(PairNonce());
	std::optional<QJsonObject> reveal;
	if (!channel->SendJson(QJsonObject{{"nonce", B64(mine)}}) || !(reveal = channel->ReceiveJson(kRequestMs))) {
		return fail(QString());
	}
	const QByteArray theirs = FromB64(reveal->value("nonce"));
	if (theirs.size() != 32 || PairCommit(theirs) != FromB64(commit->value("commit"))) {
		return fail(QStringLiteral("its answer did not match what it promised"));
	}
	const bool ok = confirm(FormatPairCode(PairCode(*channel, mine, theirs)));
	if (!channel->SendJson(QJsonObject{{"confirm", ok}}) || !ok) {
		return fail(ok ? QString() : QStringLiteral("cancelled"));
	}
	std::optional<QJsonObject> answer = channel->ReceiveJson(kPairTimeoutMs + kRequestMs);
	if (!answer) {
		return fail(QStringLiteral("no answer from it"));
	}
	if (!answer->value("accepted").toBool()) {
		return fail(ErrorText(QStringLiteral("refused")));
	}
	AddPaired(peer.id, peer.name, peer.address, peer.port);
	r.ok = true;
	return r;
}

Result Service::Upload(const Peer &peer, const QJsonObject &request, QIODevice &data,
		       const std::function<bool(qint64 sent)> &progress, int timeoutMs)
{
	Result r;
	QTcpSocket socket;
	std::unique_ptr<Channel> channel = Open(peer, socket, kHandshakeMs, &r.error);
	if (!channel) {
		return r;
	}
	std::optional<QJsonObject> header;
	if (!channel->SendJson(request) || !(header = channel->ReceiveJson(timeoutMs))) {
		r.error = channel->Error();
		return r;
	}
	auto failed = [&](const QJsonObject &answer) {
		const QString code = answer.value("error").toString();
		r.body = answer;
		r.notPaired = code == QLatin1String("not_paired");
		r.error = ErrorText(code);
		return r;
	};
	if (header->contains("error")) {
		return failed(*header);
	}
	if (!header->value("offset").isDouble()) {
		r.body = *header;
		r.ok = true;
		return r;
	}
	const qint64 offset = header->value("offset").toInteger();
	if (offset < 0 || offset > data.size() || !data.seek(offset)) {
		r.error = QStringLiteral("it asked for data past the end of the file");
		return r;
	}
	qint64 sent = offset;
	while (!data.atEnd()) {
		if (stopping) {
			r.error = QStringLiteral("stopped");
			return r;
		}
		const QByteArray chunk = data.read(kChunk);
		if (chunk.isEmpty()) {
			r.error = data.errorString();
			return r;
		}
		if (!channel->Send(chunk)) {
			r.error = channel->Error();
			return r;
		}
		sent += chunk.size();
		if (progress && !progress(sent)) {
			socket.abort();
			r.error = QStringLiteral("stopped");
			return r;
		}
	}
	std::optional<QJsonObject> answer;
	if (!channel->Send(QByteArray()) || !(answer = channel->ReceiveJson(timeoutMs))) {
		r.error = channel->Error();
		return r;
	}
	if (answer->contains("error")) {
		return failed(*answer);
	}
	r.body = *answer;
	r.ok = true;
	return r;
}

Result Service::AddAddress(const QString &host, quint16 peerPort)
{
	Result r;
	QHostAddress address(host.trimmed());
	if (address.isNull()) {
		const QHostInfo info = QHostInfo::fromName(host.trimmed());
		for (const QHostAddress &a : info.addresses()) {
			if (a.protocol() == QAbstractSocket::IPv4Protocol) {
				address = a;
				break;
			}
		}
		if (address.isNull()) {
			r.error = info.errorString().isEmpty() ? QStringLiteral("no such PC") : info.errorString();
			return r;
		}
	}
	QTcpSocket socket;
	socket.connectToHost(address, peerPort);
	if (!socket.waitForConnected(kConnectMs)) {
		r.error = socket.errorString();
		return r;
	}
	/* whoever answers: the handshake proves which key it has */
	std::unique_ptr<Channel> channel = Channel::Connect(socket, self, std::nullopt, kHandshakeMs, &r.error);
	if (!channel) {
		return r;
	}
	std::optional<QJsonObject> reply;
	if (!channel->SendJson(QJsonObject{{"op", "hello"}, {"name", Name()}, {"port", (int)port}}) ||
	    !(reply = channel->ReceiveJson(kStatusMs))) {
		r.error = channel->Error();
		return r;
	}
	const QString id = channel->PeerId();
	if (id == self.Id()) {
		r.error = QStringLiteral("that is this PC");
		return r;
	}
	const QString peerName = CleanName(reply->value("name").toString());
	{
		std::lock_guard<std::mutex> lock(mutex);
		Saw(id, peerName, address, peerPort);
		added.insert({address.toIPv4Address(), peerPort});
		addedIds.insert(id);
	}
	SaveIfMoved();
	wake.notify_all();
	Changed();
	r.body = QJsonObject{{"id", id}, {"name", peerName}};
	r.ok = true;
	return r;
}

} // namespace lucida::lan
