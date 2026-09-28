#include "lan-storage.hpp"

#include "video.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace lucida::lan {

namespace {

constexpr int kReceiveMs = 30000;             /* per chunk */
constexpr qint64 kMaxSegment = 64ll << 30;    /* 64 GB */
constexpr qint64 kMaxFrame = 32ll << 20;      /* 32 MB */
constexpr qint64 kStalePartMs = 86400 * 1000; /* unfinished sends forgotten after a day */
constexpr qint64 kPruneEveryMs = 3600 * 1000;
constexpr int kBatchLines = 100;
constexpr int kMaxNameLength = 200;

const QString kPart = QStringLiteral(".part");

double Now()
{
	return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

/* A file name from the network: one plain name, nothing that climbs out */
bool IsPlainName(const QString &name)
{
	if (name.isEmpty() || name.size() > kMaxNameLength || name.startsWith('.') || name.endsWith('.') ||
	    name.endsWith(' ')) {
		return false;
	}
	for (const QChar c : name) {
		if (c.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(c)) {
			return false;
		}
	}
	return true;
}

/* The last part of a path from either kind of system */
QString BaseName(const QString &path)
{
	const qsizetype slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
	return slash < 0 ? path : path.mid(slash + 1);
}

/* A PC's name as a folder name */
QString SafeName(const QString &name)
{
	QString out;
	for (const QChar c : name) {
		out += QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 32 ? QChar('_') : c;
	}
	out = out.trimmed().left(48);
	while (out.endsWith('.')) {
		out.chop(1);
	}
	return out.isEmpty() ? QStringLiteral("PC") : out;
}

/* When a segment began, for putting every PC's segments in one order */
double SegmentTime(const QFileInfo &f)
{
	const double start = SegmentStart(f.fileName());
	return start > 0 ? start : f.lastModified().toMSecsSinceEpoch() / 1000.0;
}

QJsonArray StringsJson(const QStringList &list)
{
	QJsonArray a;
	for (const QString &s : list) {
		a.append(s);
	}
	return a;
}

QStringList JsonStrings(const QJsonValue &v)
{
	QStringList out;
	for (const QJsonValue &x : v.toArray()) {
		out << x.toString();
	}
	return out;
}

std::optional<long long> OptInt(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QLatin1String(key));
	return v.isDouble() ? std::optional<long long>(v.toInteger()) : std::nullopt;
}

QJsonValue OrNull(const std::optional<long long> &v)
{
	return v ? QJsonValue((qint64)*v) : QJsonValue();
}

} // namespace

bool IsSegmentName(const QString &name)
{
	return IsPlainName(name) && name.endsWith(QLatin1String(".mkv"), Qt::CaseInsensitive) && SegmentStart(name) > 0;
}

/* --- log lines ----------------------------------------------------------------------- */

QJsonObject StoredLineJson(const SyncLine &x)
{
	const LogLine &l = x.line;
	QJsonObject o{{"id", (qint64)l.id},          {"session_id", OrNull(x.sessionId)},
		      {"sort_ts", (qint64)l.sortTs}, {"seq", l.seq},
		      {"ts_source", l.tsSource},     {"channel", l.channel},
		      {"tags", StringsJson(l.tags)}, {"body", l.body},
		      {"dedup_key", x.dedupKey},     {"score", l.score},
		      {"frames", l.frames},          {"first_seen", l.firstSeen},
		      {"last_seen", l.lastSeen},     {"frame_id", OrNull(l.frameId)},
		      {"labels", x.labelsJson},      {"region", l.region},
		      {"source", x.source}};
	if (l.clock) {
		o.insert("clock", *l.clock);
	}
	if (l.rect) {
		o.insert("rect", QJsonArray{l.rect->x0, l.rect->y0, l.rect->x1, l.rect->y1});
	}
	if (l.colour) {
		QJsonArray c;
		for (double v : *l.colour) {
			c.append(v);
		}
		o.insert("colour", c);
	}
	if (l.video) {
		/* the storage PC has the segment under the same name */
		o.insert("video", BaseName(l.video->path));
		o.insert("video_offset", l.video->offset);
	}
	return o;
}

SyncLine StoredLineFromJson(const QJsonObject &o)
{
	SyncLine x;
	LogLine &l = x.line;
	l.id = o.value("id").toInteger();
	x.sessionId = OptInt(o, "session_id");
	l.sortTs = o.value("sort_ts").toInteger();
	l.seq = o.value("seq").toInt();
	l.tsSource = o.value("ts_source").toString().left(16);
	if (o.value("clock").isString()) {
		l.clock = o.value("clock").toString().left(32);
	}
	l.channel = o.value("channel").toString().left(64);
	l.tags = JsonStrings(o.value("tags"));
	l.body = o.value("body").toString().left(4000);
	x.dedupKey = o.value("dedup_key").toString().left(4000);
	l.score = o.value("score").toDouble();
	l.frames = o.value("frames").toInt(1);
	l.firstSeen = o.value("first_seen").toDouble();
	l.lastSeen = o.value("last_seen").toDouble();
	l.frameId = OptInt(o, "frame_id");
	x.labelsJson = o.value("labels").toString().left(4000);
	l.region = o.value("region").toString().left(64);
	x.source = o.value("source").toString().left(16);
	const QJsonArray rect = o.value("rect").toArray();
	if (rect.size() == 4) {
		l.rect = spectra::Rect{rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt()};
	}
	const QJsonArray colour = o.value("colour").toArray();
	if (colour.size() == 13) {
		std::array<double, 13> c{};
		for (int i = 0; i < 13; i++) {
			c[i] = colour[i].toDouble();
		}
		l.colour = c;
	}
	const QString video = BaseName(o.value("video").toString());
	if (IsPlainName(video)) {
		l.video = VideoSpot{video, o.value("video_offset").toDouble()};
	}
	return x;
}

/* --- the storage PC ---------------------------------------------------------------------- */

QString StoredSource::LoopFolder() const
{
	return QDir(folder).filePath(QStringLiteral("Loop"));
}

QString StoredSource::LogPath() const
{
	return QDir(folder).filePath(QStringLiteral("Lucida/chatlog.db"));
}

StorageNode::StorageNode(const QString &folder, quint64 quotaBytes)
	: root(QDir::cleanPath(folder)),
	  quota(std::max<quint64>(quotaBytes, 1))
{
	LoadSources();
}

void StorageNode::LoadSources()
{
	QFile f(QDir(root).filePath(QStringLiteral("sources.json")));
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value("sources").toArray()) {
		const QJsonObject o = v.toObject();
		const QString id = o.value("id").toString();
		const QString folder = o.value("folder").toString();
		if (KeyFromId(id) && IsPlainName(folder)) {
			folders[id] = {o.value("name").toString(), folder};
		}
	}
}

void StorageNode::SaveSources()
{
	QJsonArray list;
	for (const auto &[id, entry] : folders) {
		list.append(QJsonObject{{"id", id}, {"name", entry.first}, {"folder", entry.second}});
	}
	QDir().mkpath(root);
	QSaveFile f(QDir(root).filePath(QStringLiteral("sources.json")));
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"sources", list}}).toJson());
		f.commit();
	}
}

QString StorageNode::SourceFolder(const QString &id, const QString &name)
{
	std::lock_guard<std::mutex> lock(mutex);
	auto it = folders.find(id);
	if (it == folders.end()) {
		/* named after the PC, told apart by its key if two share a name */
		std::optional<Key> key = KeyFromId(id);
		const QString fingerprint = key ? Fingerprint(*key).left(4) : QStringLiteral("0000");
		QString folder = QStringLiteral("%1 (%2)").arg(SafeName(name), fingerprint);
		for (int n = 2; std::any_of(folders.begin(), folders.end(),
					    [&](const auto &e) { return e.second.second == folder; });
		     n++) {
			folder = QStringLiteral("%1 (%2-%3)").arg(SafeName(name), fingerprint).arg(n);
		}
		it = folders.emplace(id, std::make_pair(name, folder)).first;
		SaveSources();
	} else if (!name.isEmpty() && it->second.first != name) {
		it->second.first = name; /* renamed: the folder stays */
		SaveSources();
	}
	const QString path = QDir(root).filePath(it->second.second);
	QDir().mkpath(QDir(path).filePath(QStringLiteral("Loop")));
	QDir().mkpath(QDir(path).filePath(QStringLiteral("Lucida/frames")));
	return path;
}

std::vector<StoredSource> StorageNode::Sources()
{
	std::map<QString, std::pair<QString, QString>> known;
	{
		std::lock_guard<std::mutex> lock(mutex);
		known = folders;
	}
	std::vector<StoredSource> out;
	for (const auto &[id, entry] : known) {
		StoredSource s;
		s.id = id;
		s.name = entry.first;
		s.folder = QDir(root).filePath(entry.second);
		for (const QFileInfo &f : QDir(s.LoopFolder()).entryInfoList({QStringLiteral("*.mkv")}, QDir::Files)) {
			s.segments++;
			s.bytes += f.size();
			s.newest = std::max<qint64>(s.newest, f.lastModified().toSecsSinceEpoch());
		}
		out.push_back(s);
	}
	std::sort(out.begin(), out.end(), [](const StoredSource &a, const StoredSource &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return out;
}

quint64 StorageNode::UsedBytes()
{
	quint64 total = 0;
	for (const QFileInfo &source : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		const QDir loop(QDir(source.absoluteFilePath()).filePath(QStringLiteral("Loop")));
		for (const QFileInfo &f :
		     loop.entryInfoList({QStringLiteral("*.mkv"), QStringLiteral("*.mkv.part")}, QDir::Files)) {
			total += (quint64)f.size();
		}
	}
	return total;
}

void StorageNode::EnforceQuota(const QString &keep)
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<QFileInfo> segments;
	quint64 total = 0;
	const QDateTime now = QDateTime::currentDateTime();
	for (const QFileInfo &source : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		const QDir loop(QDir(source.absoluteFilePath()).filePath(QStringLiteral("Loop")));
		for (const QFileInfo &f : loop.entryInfoList(QDir::Files)) {
			if (f.fileName().endsWith(kPart)) {
				/* a send cut off long ago is not coming back */
				if (!receiving.count(f.absoluteFilePath()) &&
				    f.lastModified().msecsTo(now) > kStalePartMs &&
				    QFile::remove(f.absoluteFilePath())) {
					continue;
				}
				total += (quint64)f.size();
			} else if (f.suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0) {
				total += (quint64)f.size();
				segments.push_back(f);
			}
		}
	}
	/* oldest first, whichever PC recorded it, as the loop does with its own */
	std::sort(segments.begin(), segments.end(),
		  [](const QFileInfo &a, const QFileInfo &b) { return SegmentTime(a) < SegmentTime(b); });
	const QString kept = QDir::cleanPath(keep);
	for (const QFileInfo &f : segments) {
		if (total <= quota) {
			break;
		}
		const QString path = QDir::cleanPath(f.absoluteFilePath());
		if (path.compare(kept, Qt::CaseInsensitive) == 0) {
			continue;
		}
		const quint64 size = (quint64)f.size();
		if (QFile::remove(path)) {
			total -= size;
		}
	}
}

void StorageNode::Changed()
{
	if (changed) {
		changed();
	}
}

void StorageNode::Answer(Channel &channel, const QString &peerId, const QString &peerName, const QJsonObject &request)
{
	const QString op = request.value("op").toString();
	const QString name = peerName.isEmpty() ? request.value("name").toString().left(64) : peerName;
	const QString folder = SourceFolder(peerId, name);
	if (op == QLatin1String("store_segment")) {
		AnswerSegment(channel, folder, request);
	} else if (op == QLatin1String("store_log")) {
		AnswerLog(channel, folder, request);
	} else if (op == QLatin1String("store_frame")) {
		AnswerFrame(channel, folder, request);
	} else {
		channel.SendJson(QJsonObject{{"error", "unknown_request"}});
	}
}

bool StorageNode::Receive(Channel &channel, QFile &file, qint64 size, QString *error)
{
	while (true) {
		std::optional<QByteArray> chunk = channel.Receive(kReceiveMs);
		if (!chunk) {
			*error = QStringLiteral("cut off");
			return false;
		}
		if (chunk->isEmpty()) {
			return true;
		}
		if (file.pos() + chunk->size() > size) {
			*error = QStringLiteral("bad_request");
			return false;
		}
		if (file.write(*chunk) != chunk->size()) {
			*error = QStringLiteral("write_failed");
			return false;
		}
	}
}

void StorageNode::AnswerSegment(Channel &channel, const QString &folder, const QJsonObject &request)
{
	const QString name = request.value("segment").toString();
	const qint64 size = request.value("size").toInteger();
	if (!IsSegmentName(name) || size <= 0 || size > kMaxSegment) {
		channel.SendJson(QJsonObject{{"error", "bad_request"}});
		return;
	}
	const QString dest = QDir(QDir(folder).filePath(QStringLiteral("Loop"))).filePath(name);
	const QString partPath = dest + kPart;
	const QFileInfo existing(dest);
	if (existing.exists() && existing.size() == size) {
		channel.SendJson(QJsonObject{{"done", true}});
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (receiving.count(partPath)) {
			channel.SendJson(QJsonObject{{"error", "busy"}});
			return;
		}
		receiving.insert(partPath);
	}
	struct Done {
		StorageNode *node;
		QString path;
		~Done()
		{
			std::lock_guard<std::mutex> lock(node->mutex);
			node->receiving.erase(path);
		}
	} done{this, partPath};

	/* no room for it: it would be the oldest here, so the first to go */
	if ((quint64)size > quota) {
		channel.SendJson(QJsonObject{{"skip", "full"}});
		return;
	}
	if (UsedBytes() + (quint64)size > quota) {
		double oldest = 0;
		for (const QFileInfo &source : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
			const QDir loop(QDir(source.absoluteFilePath()).filePath(QStringLiteral("Loop")));
			for (const QFileInfo &f : loop.entryInfoList({QStringLiteral("*.mkv")}, QDir::Files)) {
				const double t = SegmentTime(f);
				oldest = oldest == 0 ? t : std::min(oldest, t);
			}
		}
		if (oldest > 0 && SegmentStart(name) < oldest) {
			channel.SendJson(QJsonObject{{"skip", "full"}});
			return;
		}
	}

	QFile part(partPath);
	if (!part.open(QIODevice::ReadWrite)) {
		channel.SendJson(QJsonObject{{"error", "write_failed"}});
		return;
	}
	/* a send that was cut off goes on from where it stopped */
	qint64 offset = part.size();
	if (offset > size) {
		part.resize(0);
		offset = 0;
	}
	part.seek(offset);
	if (!channel.SendJson(QJsonObject{{"offset", offset}})) {
		return;
	}
	QString error;
	const bool ok = Receive(channel, part, size, &error);
	const qint64 got = part.size();
	part.close();
	if (!ok) {
		if (error != QLatin1String("cut off")) {
			QFile::remove(partPath);
			channel.SendJson(QJsonObject{{"error", error}});
		}
		return;
	}
	if (got != size) {
		QFile::remove(partPath);
		channel.SendJson(QJsonObject{{"error", "bad_request"}});
		return;
	}
	QFile::remove(dest);
	if (!QFile::rename(partPath, dest)) {
		channel.SendJson(QJsonObject{{"error", "write_failed"}});
		return;
	}
	const qint64 modified = request.value("modified").toInteger();
	if (modified > 0) {
		QFile f(dest);
		if (f.open(QIODevice::ReadWrite)) {
			f.setFileTime(QDateTime::fromSecsSinceEpoch(modified), QFileDevice::FileModificationTime);
		}
	}
	EnforceQuota(dest);
	channel.SendJson(QJsonObject{{"stored", true}});
	Changed();
}

Store *StorageNode::OpenStore(const QString &path)
{
	auto it = stores.find(path);
	if (it != stores.end() && it->second->IsOpen()) {
		return it->second.get();
	}
	auto store = std::make_unique<Store>(path);
	if (!store->Open() || !store->IsOpen()) {
		return nullptr;
	}
	Store *s = store.get();
	stores[path] = std::move(store);
	return s;
}

void StorageNode::AnswerLog(Channel &channel, const QString &folder, const QJsonObject &request)
{
	std::vector<SyncSession> sessions;
	for (const QJsonValue &v : request.value("sessions").toArray()) {
		const QJsonObject o = v.toObject();
		SyncSession x;
		x.id = o.value("id").toInteger();
		x.started = o.value("started").toDouble();
		if (o.value("ended").isDouble()) {
			x.ended = o.value("ended").toDouble();
		}
		x.target = o.value("target").toString().left(256);
		x.width = o.value("width").toInt();
		x.height = o.value("height").toInt();
		if (x.id > 0) {
			sessions.push_back(x);
		}
	}
	const QString loop = QDir(folder).filePath(QStringLiteral("Loop"));
	std::vector<SyncLine> lines;
	for (const QJsonValue &v : request.value("lines").toArray()) {
		SyncLine x = StoredLineFromJson(v.toObject());
		if (x.line.id <= 0 || x.line.body.isEmpty()) {
			continue;
		}
		/* the segment as it is stored here */
		if (x.line.video) {
			x.line.video->path = QDir(loop).filePath(x.line.video->path);
		}
		lines.push_back(std::move(x));
	}
	const QString path = QDir(folder).filePath(QStringLiteral("Lucida/chatlog.db"));
	QStringList orphans;
	{
		std::lock_guard<std::mutex> lock(storesMutex);
		Store *store = OpenStore(path);
		if (!store) {
			channel.SendJson(QJsonObject{{"error", "write_failed"}});
			return;
		}
		store->ImportSessions(sessions);
		store->ImportLines(lines);
		/* the stored logs keep what this PC's own log keeps */
		const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
		if (nowMs - pruned[path] >= kPruneEveryMs) {
			pruned[path] = nowMs;
			if (retentionDays > 0) {
				store->Prune(retentionDays, &orphans);
			}
			if (frameRetentionDays > 0) {
				orphans << store->PruneFrames(frameRetentionDays);
			}
		}
	}
	const QString frames = QDir(QDir(folder).filePath(QStringLiteral("Lucida/frames"))).absolutePath();
	for (const QString &f : orphans) {
		/* only files this storage saved */
		if (QFileInfo(f).absoluteFilePath().startsWith(frames + '/', Qt::CaseInsensitive)) {
			QFile::remove(f);
		}
	}
	channel.SendJson(QJsonObject{{"stored", true}, {"lines", (int)lines.size()}});
	Changed();
}

void StorageNode::AnswerFrame(Channel &channel, const QString &folder, const QJsonObject &request)
{
	const QJsonObject f = request.value("frame").toObject();
	const QString name = f.value("file").toString();
	const qint64 size = request.value("size").toInteger();
	const QString suffix = QFileInfo(name).suffix().toLower();
	Frame frame;
	frame.id = f.value("id").toInteger();
	frame.sortTs = f.value("sort_ts").toInteger();
	frame.width = f.value("width").toInt();
	frame.height = f.value("height").toInt();
	if (!IsPlainName(name) || frame.id <= 0 || size <= 0 || size > kMaxFrame ||
	    (suffix != QLatin1String("jpg") && suffix != QLatin1String("jpeg") && suffix != QLatin1String("png") &&
	     suffix != QLatin1String("webp"))) {
		channel.SendJson(QJsonObject{{"error", "bad_request"}});
		return;
	}
	const QString day = QDateTime::fromSecsSinceEpoch(frame.sortTs).toString(QStringLiteral("yyyy-MM-dd"));
	const QString dir = QDir(folder).filePath(QStringLiteral("Lucida/frames/") + day);
	frame.path = QDir(dir).filePath(name);
	const QString logPath = QDir(folder).filePath(QStringLiteral("Lucida/chatlog.db"));
	std::vector<Sighting> sightings;
	for (const QJsonValue &v : request.value("lines").toArray()) {
		const QJsonObject o = v.toObject();
		Sighting s;
		s.lineId = o.value("line").toInteger();
		s.seq = o.value("seq").toInt();
		const QJsonArray rect = o.value("rect").toArray();
		if (rect.size() == 4) {
			s.rect = spectra::Rect{rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt()};
		}
		if (s.lineId > 0) {
			sightings.push_back(s);
		}
	}
	const std::optional<long long> sessionId = f.value("session_id").isDouble()
							   ? std::optional<long long>(f.value("session_id").toInteger())
							   : std::nullopt;
	{
		std::lock_guard<std::mutex> lock(storesMutex);
		Store *store = OpenStore(logPath);
		std::optional<Frame> have = store ? store->GetFrame(frame.id) : std::nullopt;
		if (have && QFileInfo(have->path).size() == size) {
			/* sent before: its lines may have changed */
			store->ImportFrame(*have, sessionId, f.value("created").toDouble(), sightings);
			channel.SendJson(QJsonObject{{"done", true}});
			return;
		}
	}
	QDir().mkpath(dir);
	QFile part(frame.path + kPart);
	if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate) || !channel.SendJson(QJsonObject{{"offset", 0}})) {
		if (!part.isOpen()) {
			channel.SendJson(QJsonObject{{"error", "write_failed"}});
		}
		return;
	}
	QString error;
	const bool ok = Receive(channel, part, size, &error);
	const qint64 got = part.size();
	part.close();
	if (!ok || got != size) {
		part.remove();
		if (error != QLatin1String("cut off")) {
			channel.SendJson(QJsonObject{{"error", ok ? QStringLiteral("bad_request") : error}});
		}
		return;
	}
	QFile::remove(frame.path);
	if (!QFile::rename(part.fileName(), frame.path)) {
		channel.SendJson(QJsonObject{{"error", "write_failed"}});
		return;
	}
	{
		std::lock_guard<std::mutex> lock(storesMutex);
		Store *store = OpenStore(logPath);
		if (!store) {
			channel.SendJson(QJsonObject{{"error", "write_failed"}});
			return;
		}
		store->ImportFrame(frame, sessionId, f.value("created").toDouble(), sightings);
	}
	channel.SendJson(QJsonObject{{"stored", true}});
	Changed();
}

/* --- sending to it ------------------------------------------------------------------------ */

StorageClient::StorageClient(Service &service_, const QString &stateFile_) : service(service_), stateFile(stateFile_) {}

void StorageClient::Load()
{
	if (loaded) {
		return;
	}
	loaded = true;
	QFile f(stateFile);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
	node = o.value("node").toString();
	for (const QJsonValue &v : o.value("sent").toArray()) {
		sent.insert(v.toString());
	}
}

void StorageClient::Save()
{
	QJsonArray list;
	for (const QString &s : sent) {
		list.append(s);
	}
	QDir().mkpath(QFileInfo(stateFile).absolutePath());
	QSaveFile f(stateFile);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"node", node}, {"sent", list}}).toJson(QJsonDocument::Compact));
		f.commit();
	}
}

void StorageClient::UseNode(const QString &id, Store *store)
{
	Load();
	if (node != id) {
		node = id;
		sent.clear();
		Save();
	}
	if (store && store->Meta("lan_storage_node") != id) {
		store->ResetSync(SyncTarget::Lan);
		store->SetMeta("lan_storage_node", id);
	}
}

QStringList StorageClient::Waiting(const QString &loopDir, bool recording)
{
	Load();
	QFileInfoList files = QDir(loopDir).entryInfoList({QStringLiteral("*.mkv")}, QDir::Files, QDir::Name);
	/* the newest is still being written while the loop records */
	if (recording && !files.isEmpty()) {
		files.removeLast();
	}
	QStringList out;
	for (qsizetype i = files.size() - 1; i >= 0; i--) {
		const QFileInfo &f = files[i];
		if (IsSegmentName(f.fileName()) && f.size() > 0 && !sent.count(f.fileName())) {
			out << f.absoluteFilePath();
		}
	}
	/* forget segments the loop deleted, so the list does not grow forever */
	bool pruned = false;
	for (auto it = sent.begin(); it != sent.end();) {
		if (!QFileInfo::exists(QDir(loopDir).filePath(*it))) {
			it = sent.erase(it);
			pruned = true;
		} else {
			++it;
		}
	}
	if (pruned) {
		Save();
	}
	return out;
}

PushReport StorageClient::Step(const Peer &peer, Store *store, const QString &loopDir, bool recording,
			       const std::function<bool(const QString &, qint64, qint64)> &progress, int maxFrames,
			       int maxBatches)
{
	PushReport report;
	UseNode(peer.id, store);
	if (store && store->IsOpen() && !SendLog(peer, *store, report, maxFrames, maxBatches)) {
		return report;
	}
	const QStringList waiting = loopDir.isEmpty() ? QStringList() : Waiting(loopDir, recording);
	if (!waiting.isEmpty()) {
		if (!SendSegment(peer, waiting.first(), report, progress)) {
			return report;
		}
		report.more = report.more || waiting.size() > 1;
	}
	return report;
}

bool StorageClient::SendLog(const Peer &peer, Store &store, PushReport &report, int maxFrames, int maxBatches)
{
	auto fail = [&](const Result &r) {
		report.error = r.error.isEmpty() ? QStringLiteral("no answer") : r.error;
		return false;
	};

	/* sessions */
	const std::vector<SyncSession> sessions = store.UnsyncedSessions(200, SyncTarget::Lan);
	if (!sessions.empty()) {
		QJsonArray list;
		for (const SyncSession &x : sessions) {
			QJsonObject o{{"id", (qint64)x.id},
				      {"started", x.started},
				      {"target", x.target},
				      {"width", x.width},
				      {"height", x.height}};
			if (x.ended) {
				o.insert("ended", *x.ended);
			}
			list.append(o);
		}
		const Result r = service.Request(
			peer, QJsonObject{{"op", "store_log"}, {"name", service.Name()}, {"sessions", list}});
		if (!r.ok) {
			return fail(r);
		}
		const double at = Now();
		for (const SyncSession &x : sessions) {
			store.MarkSessionSynced(x, at, SyncTarget::Lan);
		}
		report.sessions += (int)sessions.size();
	}

	/* screenshots, before the lines that point at them */
	for (const SyncFrame &f : store.UnsyncedFrames(maxFrames, SyncTarget::Lan)) {
		QFile file(f.frame.path);
		if (!file.open(QIODevice::ReadOnly)) {
			/* gone (pruned): nothing to send, ever */
			store.MarkFrameSynced(f.frame.id, -1, SyncTarget::Lan);
			continue;
		}
		QJsonArray lines;
		int seq = 0;
		for (const LogLine &l : store.FrameLines(f.frame.id)) {
			QJsonObject o{{"line", (qint64)l.id}, {"seq", seq++}};
			if (l.rect) {
				o.insert("rect", QJsonArray{l.rect->x0, l.rect->y0, l.rect->x1, l.rect->y1});
			}
			lines.append(o);
		}
		QJsonObject frame{{"id", (qint64)f.frame.id},
				  {"sort_ts", (qint64)f.frame.sortTs},
				  {"width", f.frame.width},
				  {"height", f.frame.height},
				  {"created", f.created},
				  {"file", BaseName(f.frame.path)},
				  {"session_id", OrNull(f.sessionId)}};
		const Result r = service.Upload(peer,
						QJsonObject{{"op", "store_frame"},
							    {"name", service.Name()},
							    {"frame", frame},
							    {"lines", lines},
							    {"size", file.size()}},
						file);
		if (!r.ok) {
			if (r.error == QLatin1String("it did not understand what was sent")) {
				/* e.g. a file type it does not keep: never retried */
				store.MarkFrameSynced(f.frame.id, -1, SyncTarget::Lan);
				continue;
			}
			return fail(r);
		}
		store.MarkFrameSynced(f.frame.id, Now(), SyncTarget::Lan);
		report.frames++;
	}

	/* lines, in batches that fit one message */
	for (int batch = 0; batch < maxBatches; batch++) {
		std::vector<SyncLine> lines = store.UnsyncedLines(kBatchLines, SyncTarget::Lan);
		if (lines.empty()) {
			break;
		}
		QJsonObject request;
		while (true) {
			QJsonArray list;
			for (const SyncLine &l : lines) {
				list.append(StoredLineJson(l));
			}
			request = QJsonObject{{"op", "store_log"}, {"name", service.Name()}, {"lines", list}};
			if (lines.size() <= 1 || QJsonDocument(request).toJson(QJsonDocument::Compact).size() <
							 Channel::kMaxMessage - 1024) {
				break;
			}
			lines.resize(lines.size() / 2);
		}
		const Result r = service.Request(peer, request);
		if (!r.ok) {
			return fail(r);
		}
		store.MarkLinesSynced(lines, Now(), SyncTarget::Lan);
		report.lines += (int)lines.size();
	}
	const SyncBacklog left = store.Unsynced(SyncTarget::Lan);
	report.more = left.lines + left.frames + left.sessions > 0;
	return true;
}

bool StorageClient::SendSegment(const Peer &peer, const QString &path, PushReport &report,
				const std::function<bool(const QString &, qint64, qint64)> &progress)
{
	const QString name = QFileInfo(path).fileName();
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		/* the loop deleted it meanwhile */
		report.more = true;
		return true;
	}
	const qint64 size = file.size();
	const Result r = service.Upload(peer,
					QJsonObject{{"op", "store_segment"},
						    {"name", service.Name()},
						    {"segment", name},
						    {"size", size},
						    {"modified", QFileInfo(path).lastModified().toSecsSinceEpoch()}},
					file, [&](qint64 done) { return !progress || progress(name, done, size); });
	if (!r.ok) {
		report.error = r.error.isEmpty() ? QStringLiteral("no answer") : r.error;
		return false;
	}
	if (r.body.contains("skip")) {
		report.skipped++;
	} else {
		report.segments++;
	}
	sent.insert(name);
	Save();
	return true;
}

} // namespace lucida::lan
