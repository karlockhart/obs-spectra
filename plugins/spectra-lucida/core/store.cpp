#include "store.hpp"

#include <spectra-vision/text.hpp>

#include <sqlite3.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace lucida {

namespace {

const char *kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS sessions (
    id          INTEGER PRIMARY KEY,
    started     REAL NOT NULL,
    ended       REAL,
    target      TEXT,
    width       INTEGER,
    height      INTEGER
);
CREATE TABLE IF NOT EXISTS lines (
    id          INTEGER PRIMARY KEY,
    session_id  INTEGER,
    sort_ts     INTEGER NOT NULL,
    seq         INTEGER NOT NULL DEFAULT 0,
    ts_source   TEXT NOT NULL DEFAULT 'wall',
    clock       TEXT,
    channel     TEXT,
    tags        TEXT,
    body        TEXT NOT NULL,
    dedup_key   TEXT NOT NULL,
    score       REAL NOT NULL DEFAULT 0,
    frames      INTEGER NOT NULL DEFAULT 1,
    first_seen  REAL NOT NULL,
    last_seen   REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS lines_order ON lines(sort_ts, seq, id);
CREATE INDEX IF NOT EXISTS lines_key ON lines(dedup_key, sort_ts);
CREATE INDEX IF NOT EXISTS lines_channel ON lines(channel);
CREATE TABLE IF NOT EXISTS frames (
    id          INTEGER PRIMARY KEY,
    session_id  INTEGER,
    sort_ts     INTEGER NOT NULL,
    path        TEXT NOT NULL,
    width       INTEGER,
    height      INTEGER,
    created     REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS frames_order ON frames(sort_ts);
)SQL";

/* Columns added after Lucida 0.1.0; older databases are migrated on open */
const std::pair<const char *, const char *> kLaterColumns[] = {
	{"frame_id", "INTEGER"},
	{"rect", "TEXT"},
	/* Spectra: chat colour for Obscura's learner, tag-rule labels, and the
	 * loop recording segment the line was first seen in */
	{"colour", "TEXT"},
	{"labels", "TEXT"},
	{"video", "TEXT"},
	{"video_offset", "REAL"}};

constexpr double kFuzzyRatio = 0.9; /* below this, two readings are different lines */
constexpr int kPrefixMin = 12;      /* shorter than this, a shared opening proves nothing */

double Now()
{
	return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

std::string Utf8(const QString &s)
{
	return s.toStdString();
}

/* RAII prepared statement with simple binding */
class Stmt {
public:
	Stmt(sqlite3 *db, const char *sql)
	{
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
			stmt = nullptr;
		}
	}
	~Stmt() { sqlite3_finalize(stmt); }
	explicit operator bool() const { return stmt != nullptr; }

	Stmt &Bind(int i, long long v)
	{
		sqlite3_bind_int64(stmt, i, v);
		return *this;
	}
	Stmt &Bind(int i, int v)
	{
		sqlite3_bind_int(stmt, i, v);
		return *this;
	}
	Stmt &Bind(int i, double v)
	{
		sqlite3_bind_double(stmt, i, v);
		return *this;
	}
	Stmt &Bind(int i, const QString &v)
	{
		std::string s = Utf8(v);
		sqlite3_bind_text(stmt, i, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
		return *this;
	}
	Stmt &BindNull(int i)
	{
		sqlite3_bind_null(stmt, i);
		return *this;
	}
	template<typename T> Stmt &Bind(int i, const std::optional<T> &v)
	{
		if (v) {
			Bind(i, *v);
		} else {
			BindNull(i);
		}
		return *this;
	}

	/* SQLITE_ROW, SQLITE_DONE or an error code */
	int Step() { return stmt ? sqlite3_step(stmt) : SQLITE_ERROR; }
	bool Run() { return Step() == SQLITE_DONE; }
	void Reset()
	{
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}

	int ColumnIndex(const char *name) const
	{
		int n = sqlite3_column_count(stmt);
		for (int i = 0; i < n; i++) {
			if (strcmp(sqlite3_column_name(stmt, i), name) == 0) {
				return i;
			}
		}
		return -1;
	}
	bool IsNull(int c) const { return c < 0 || sqlite3_column_type(stmt, c) == SQLITE_NULL; }
	long long Int(int c) const { return c < 0 ? 0 : sqlite3_column_int64(stmt, c); }
	double Real(int c) const { return c < 0 ? 0.0 : sqlite3_column_double(stmt, c); }
	QString Text(int c) const
	{
		if (c < 0) {
			return {};
		}
		const unsigned char *t = sqlite3_column_text(stmt, c);
		return t ? QString::fromUtf8(reinterpret_cast<const char *>(t), sqlite3_column_bytes(stmt, c))
			 : QString();
	}
	std::optional<QString> OptText(int c) const
	{
		return IsNull(c) ? std::nullopt : std::optional<QString>(Text(c));
	}

	sqlite3_stmt *stmt = nullptr;
};

std::optional<spectra::Rect> ParseRect(const QString &text)
{
	if (text.isEmpty()) {
		return std::nullopt;
	}
	QStringList parts = text.split(',');
	if (parts.size() != 4) {
		return std::nullopt;
	}
	int v[4];
	for (int i = 0; i < 4; i++) {
		bool ok = false;
		v[i] = parts[i].trimmed().toInt(&ok);
		if (!ok) {
			return std::nullopt;
		}
	}
	return spectra::Rect{v[0], v[1], v[2], v[3]};
}

LogLine RowToLine(const Stmt &s)
{
	LogLine l;
	l.id = s.Int(s.ColumnIndex("id"));
	l.sortTs = s.Int(s.ColumnIndex("sort_ts"));
	l.seq = (int)s.Int(s.ColumnIndex("seq"));
	l.tsSource = s.Text(s.ColumnIndex("ts_source"));
	l.clock = s.OptText(s.ColumnIndex("clock"));
	QString channel = s.Text(s.ColumnIndex("channel"));
	l.channel = channel.isEmpty() ? QStringLiteral("other") : channel;
	QJsonDocument tags = QJsonDocument::fromJson(s.Text(s.ColumnIndex("tags")).toUtf8());
	for (const QJsonValue &v : tags.array()) {
		l.tags << v.toString();
	}
	l.body = s.Text(s.ColumnIndex("body"));
	l.score = s.Real(s.ColumnIndex("score"));
	l.frames = (int)s.Int(s.ColumnIndex("frames"));
	int fid = s.ColumnIndex("frame_id");
	if (!s.IsNull(fid)) {
		l.frameId = s.Int(fid);
	}
	l.rect = ParseRect(s.Text(s.ColumnIndex("rect")));
	for (const QJsonValue &v : QJsonDocument::fromJson(s.Text(s.ColumnIndex("labels")).toUtf8()).array()) {
		l.labels << v.toString();
	}
	QJsonArray colour = QJsonDocument::fromJson(s.Text(s.ColumnIndex("colour")).toUtf8()).array();
	if (colour.size() == 13) {
		std::array<double, 13> c{};
		for (int i = 0; i < 13; i++) {
			c[i] = colour[i].toDouble();
		}
		l.colour = c;
	}
	l.firstSeen = s.Real(s.ColumnIndex("first_seen"));
	const QString video = s.Text(s.ColumnIndex("video"));
	if (!video.isEmpty()) {
		l.video = VideoSpot{video, s.Real(s.ColumnIndex("video_offset"))};
	}
	return l;
}

std::vector<LogLine> Collect(Stmt &s, bool reverse = false)
{
	std::vector<LogLine> out;
	while (s.Step() == SQLITE_ROW) {
		out.push_back(RowToLine(s));
	}
	if (reverse) {
		std::reverse(out.begin(), out.end());
	}
	return out;
}

/* _same_line: do two normalised readings describe the same chat line? */
bool SameLine(const QString &a, const QString &b, bool timed)
{
	if (spectra::SequenceRatio(a, b) >= kFuzzyRatio) {
		return true;
	}
	if (!timed) {
		return false;
	}
	/* Row grouping sometimes glues the start of the next line onto this one */
	const QString &shortS = a.size() <= b.size() ? a : b;
	const QString &longS = a.size() <= b.size() ? b : a;
	if (shortS.toUcs4().size() < kPrefixMin || shortS.size() == longS.size()) {
		return false;
	}
	QList<uint> l = longS.toUcs4();
	QString prefix = QString::fromUcs4(reinterpret_cast<const char32_t *>(l.constData()),
					   std::min<qsizetype>(shortS.toUcs4().size(), l.size()));
	return spectra::SequenceRatio(shortS, prefix) >= kFuzzyRatio;
}

std::optional<QString> RectText(const spectra::ChatEntry &e)
{
	if (e.rects.empty()) {
		return std::nullopt;
	}
	int x0 = e.rects[0].x0, y0 = e.rects[0].y0, x1 = e.rects[0].x1, y1 = e.rects[0].y1;
	for (const spectra::Rect &r : e.rects) {
		x0 = std::min(x0, r.x0);
		y0 = std::min(y0, r.y0);
		x1 = std::max(x1, r.x1);
		y1 = std::max(y1, r.y1);
	}
	return QStringLiteral("%1,%2,%3,%4").arg(x0).arg(y0).arg(x1).arg(y1);
}

double EntryScore(const spectra::ChatEntry &e)
{
	double sum = 0.0;
	int n = 0;
	for (const spectra::Row &r : e.rows) {
		for (const spectra::OcrBox &b : r.boxes) {
			sum += b.score;
			n++;
		}
	}
	return n ? sum / n : 0.0;
}

QString ColourJson(const std::array<float, 13> &colour)
{
	QJsonArray arr;
	for (float v : colour) {
		arr.append(std::round(v * 10000.0) / 10000.0);
	}
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString TagsJson(const QStringList &tags)
{
	QJsonArray arr;
	for (const QString &t : tags) {
		arr.append(t);
	}
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

} // namespace

/* ------------------------------------------------------------------------- */

QString Normalise(const QString &text)
{
	static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
	static const QRegularExpression spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
	QString s = text.toLower();
	s.replace(nonAlnum, QStringLiteral(" "));
	s.replace(spaces, QStringLiteral(" "));
	return s.trimmed();
}

QString DedupKey(const std::optional<QString> &clock, const QString &body)
{
	return (clock && !clock->isEmpty() ? *clock : QStringLiteral("-")) + '|' + Normalise(body);
}

QString LogLine::When() const
{
	return QDateTime::fromSecsSinceEpoch(sortTs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

Store::Store(const QString &path_, double dedupWindow_) : dedupWindow(dedupWindow_), path(path_) {}

Store::~Store()
{
	Close();
}

bool Store::Exec(const char *sql)
{
	char *err = nullptr;
	int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
	sqlite3_free(err);
	return rc == SQLITE_OK;
}

QString Store::Pragma(const char *name)
{
	Stmt s(db, (std::string("PRAGMA ") + name).c_str());
	return s.Step() == SQLITE_ROW ? s.Text(0) : QString();
}

bool Store::Open(QString *error)
{
	Close();
	QDir().mkpath(QFileInfo(path).absolutePath());
	std::string p = Utf8(path);
	if (sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
			    nullptr) != SQLITE_OK) {
		if (error) {
			*error = db ? QString::fromUtf8(sqlite3_errmsg(db)) : QStringLiteral("out of memory");
		}
		sqlite3_close(db);
		db = nullptr;
		return false;
	}
	sqlite3_busy_timeout(db, 5000);

	/* Journal mode first, before any DDL: SQLite refuses to change it inside
	 * a transaction, and a rollback-journal connection writing a file that
	 * another holds in WAL mode corrupts it. */
	QString mode = Pragma("journal_mode=WAL");
	if (mode.compare(QLatin1String("wal"), Qt::CaseInsensitive) != 0 && error) {
		*error = QStringLiteral("Could not put the log into WAL mode (got %1)").arg(mode);
	}
	Exec("PRAGMA synchronous=NORMAL");
	if (!Exec(kSchema)) {
		if (error) {
			*error = QString::fromUtf8(sqlite3_errmsg(db));
		}
		Close();
		return false;
	}
	Migrate();
	fts = TryFts();
	LoadWindow();
	return true;
}

void Store::Close()
{
	if (db) {
		sqlite3_close_v2(db);
		db = nullptr;
	}
	recent.clear();
	byClock.clear();
}

void Store::Migrate()
{
	std::set<std::string> have;
	{
		Stmt s(db, "PRAGMA table_info(lines)");
		while (s.Step() == SQLITE_ROW) {
			have.insert(Utf8(s.Text(1)));
		}
	}
	for (const auto &[name, kind] : kLaterColumns) {
		if (!have.count(name)) {
			std::string sql = std::string("ALTER TABLE lines ADD COLUMN ") + name + " " + kind;
			Exec(sql.c_str());
		}
	}
}

bool Store::TryFts()
{
	/* Creating the table succeeds even when an existing index is damaged;
	 * that only shows on the first query, so probe it here. */
	if (!Exec("CREATE VIRTUAL TABLE IF NOT EXISTS lines_fts USING fts5(body, content='')")) {
		return false;
	}
	Stmt s(db, "SELECT rowid FROM lines_fts LIMIT 1");
	if (!s) {
		return false;
	}
	int rc = s.Step();
	return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

void Store::LoadWindow()
{
	/* Re-arm the dedup window from disk so a restart does not duplicate what
	 * is still on screen */
	const double now = Now();
	Stmt s(db, "SELECT id, dedup_key, clock, body, score, sort_ts, last_seen FROM lines"
		   " WHERE sort_ts >= ? OR last_seen >= ?");
	s.Bind(1, (long long)(now - dedupWindow)).Bind(2, now - dedupWindow);
	while (s.Step() == SQLITE_ROW) {
		long long sortTs = s.Int(5);
		Remember({s.Int(0), s.Text(1), s.OptText(2), s.Text(3), s.Real(4), sortTs,
			  std::max(sortTs, (long long)s.Real(6))});
	}
}

long long Store::StartSession(const QString &target, int width, int height)
{
	Stmt s(db, "INSERT INTO sessions(started, target, width, height) VALUES (?,?,?,?)");
	s.Bind(1, Now()).Bind(2, target).Bind(3, width).Bind(4, height).Run();
	return sqlite3_last_insert_rowid(db);
}

void Store::EndSession(long long sessionId)
{
	Stmt s(db, "UPDATE sessions SET ended=? WHERE id=?");
	s.Bind(1, Now()).Bind(2, sessionId).Run();
}

std::vector<long long> Store::AddFrame(const std::vector<spectra::ChatEntry> &entries, long long frameTs,
				       const QString &tsSource, std::optional<long long> sessionId)
{
	std::vector<long long> added;
	if (!db) {
		return added;
	}
	const double now = Now();
	PruneWindow(frameTs);
	Exec("BEGIN");
	for (size_t seq = 0; seq < entries.size(); seq++) {
		const spectra::ChatEntry &entry = entries[seq];
		QString body = entry.body.trimmed();
		if (body.isEmpty()) {
			continue;
		}
		double score = EntryScore(entry);
		QString key = DedupKey(entry.time, body);
		if (WindowLine *hit = Match(key, entry.time, body)) {
			Touch(*hit, body, score, frameTs, now);
			continue;
		}
		long long rowId = Insert(entry, body, key, score, frameTs, (int)seq, tsSource, sessionId, now);
		Remember({rowId, key, entry.time, body, score, frameTs, frameTs});
		added.push_back(rowId);
	}
	Exec("COMMIT");
	return added;
}

long long Store::Insert(const spectra::ChatEntry &entry, const QString &body, const QString &key, double score,
			long long frameTs, int seq, const QString &tsSource, std::optional<long long> sessionId,
			double now)
{
	Stmt s(db, "INSERT INTO lines(session_id, sort_ts, seq, ts_source, clock, channel, tags, body,"
		   " dedup_key, score, frames, first_seen, last_seen, rect, colour, labels)"
		   " VALUES (?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?)");
	s.Bind(1, sessionId)
		.Bind(2, frameTs)
		.Bind(3, seq)
		.Bind(4, tsSource)
		.Bind(5, entry.time)
		.Bind(6, entry.channel)
		.Bind(7, TagsJson(entry.tags))
		.Bind(8, body)
		.Bind(9, key)
		.Bind(10, score)
		.Bind(11, now)
		.Bind(12, now)
		.Bind(13, RectText(entry))
		.Bind(14, ColourJson(entry.colour))
		.Bind(15, TagsJson(labeler ? labeler(body) : QStringList()))
		.Run();
	long long rowId = sqlite3_last_insert_rowid(db);
	if (fts) {
		Stmt f(db, "INSERT INTO lines_fts(rowid, body) VALUES (?,?)");
		f.Bind(1, rowId).Bind(2, body).Run();
	}
	return rowId;
}

void Store::Touch(WindowLine &hit, const QString &body, double score, long long frameTs, double now)
{
	/* Another sighting of a line we already have: keep the best reading */
	if (score > hit.score + 0.01 && body != hit.body) {
		Stmt s(db, "UPDATE lines SET body=?, score=?, frames=frames+1, last_seen=? WHERE id=?");
		s.Bind(1, body).Bind(2, score).Bind(3, now).Bind(4, hit.rowId).Run();
		if (labeler) {
			Stmt l(db, "UPDATE lines SET labels=? WHERE id=?");
			l.Bind(1, TagsJson(labeler(body))).Bind(2, hit.rowId).Run();
		}
		if (fts) {
			Stmt del(db, "INSERT INTO lines_fts(lines_fts, rowid, body) VALUES ('delete', ?, ?)");
			del.Bind(1, hit.rowId).Bind(2, hit.body).Run();
			Stmt ins(db, "INSERT INTO lines_fts(rowid, body) VALUES (?,?)");
			ins.Bind(1, hit.rowId).Bind(2, body).Run();
		}
		hit.body = body;
		hit.score = score;
	} else {
		Stmt s(db, "UPDATE lines SET frames=frames+1, last_seen=? WHERE id=?");
		s.Bind(1, now).Bind(2, hit.rowId).Run();
	}
	/* A line that stays on screen stays inside the window */
	hit.seenTs = std::max(hit.seenTs, frameTs);
}

Store::WindowLine *Store::Match(const QString &key, const std::optional<QString> &clock, const QString &body)
{
	auto exact = recent.find(Utf8(key));
	if (exact != recent.end()) {
		return &exact->second;
	}
	/* OCR noise: compare with the lines sharing the in-game clock */
	const QString target = Normalise(body);
	auto bucket = byClock.find(Utf8(clock && !clock->isEmpty() ? *clock : QStringLiteral("-")));
	if (bucket == byClock.end()) {
		return nullptr;
	}
	for (const std::string &candidateKey : bucket->second) {
		auto it = recent.find(candidateKey);
		if (it == recent.end()) {
			continue;
		}
		if (SameLine(target, Normalise(it->second.body), clock.has_value())) {
			return &it->second;
		}
	}
	return nullptr;
}

void Store::Remember(const WindowLine &item)
{
	std::string key = Utf8(item.key);
	recent[key] = item;
	byClock[Utf8(item.clock && !item.clock->isEmpty() ? *item.clock : QStringLiteral("-"))].push_back(key);
}

void Store::PruneWindow(long long nowTs)
{
	const double cutoff = nowTs - dedupWindow;
	std::vector<std::string> stale;
	for (const auto &[key, item] : recent) {
		if (item.seenTs < cutoff) {
			stale.push_back(key);
		}
	}
	for (const std::string &key : stale) {
		auto it = recent.find(key);
		std::string clockKey = Utf8(it->second.clock && !it->second.clock->isEmpty() ? *it->second.clock
											     : QStringLiteral("-"));
		recent.erase(it);
		auto bucket = byClock.find(clockKey);
		if (bucket != byClock.end()) {
			auto &v = bucket->second;
			v.erase(std::remove(v.begin(), v.end(), key), v.end());
			if (v.empty()) {
				byClock.erase(bucket);
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Screenshots */

long long Store::AttachFrame(const std::vector<long long> &lineIds, const QString &framePath, int width, int height,
			     long long sortTs, std::optional<long long> sessionId)
{
	Exec("BEGIN");
	Stmt s(db, "INSERT INTO frames(session_id, sort_ts, path, width, height, created) VALUES (?,?,?,?,?,?)");
	s.Bind(1, sessionId).Bind(2, sortTs).Bind(3, framePath).Bind(4, width).Bind(5, height).Bind(6, Now()).Run();
	long long frameId = sqlite3_last_insert_rowid(db);
	Stmt u(db, "UPDATE lines SET frame_id=? WHERE id=?");
	for (long long id : lineIds) {
		u.Reset();
		u.Bind(1, frameId).Bind(2, id).Run();
	}
	Exec("COMMIT");
	return frameId;
}

std::optional<Frame> Store::GetFrame(long long frameId)
{
	Stmt s(db, "SELECT id, sort_ts, path, width, height FROM frames WHERE id=?");
	s.Bind(1, frameId);
	if (s.Step() != SQLITE_ROW) {
		return std::nullopt;
	}
	return Frame{s.Int(0), s.Int(1), s.Text(2), (int)s.Int(3), (int)s.Int(4)};
}

std::vector<LogLine> Store::FrameLines(long long frameId)
{
	Stmt s(db, "SELECT * FROM lines WHERE frame_id=? ORDER BY seq, id");
	s.Bind(1, frameId);
	return Collect(s);
}

QStringList Store::PruneFrames(int days)
{
	QStringList paths;
	if (days <= 0 || !db) {
		return paths;
	}
	long long cutoff = (long long)(Now() - days * 86400.0);
	std::vector<long long> ids;
	{
		Stmt s(db, "SELECT id, path FROM frames WHERE sort_ts < ?");
		s.Bind(1, cutoff);
		while (s.Step() == SQLITE_ROW) {
			ids.push_back(s.Int(0));
			paths << s.Text(1);
		}
	}
	if (ids.empty()) {
		return paths;
	}
	Exec("BEGIN");
	Stmt u(db, "UPDATE lines SET frame_id=NULL WHERE frame_id=?");
	Stmt d(db, "DELETE FROM frames WHERE id=?");
	for (long long id : ids) {
		u.Reset();
		u.Bind(1, id).Run();
		d.Reset();
		d.Bind(1, id).Run();
	}
	Exec("COMMIT");
	return paths;
}

/* ------------------------------------------------------------------------- */
/* Reading */

std::vector<LogLine> Store::Recent(int limit, long long afterId)
{
	Stmt s(db, "SELECT * FROM lines WHERE id > ? ORDER BY sort_ts DESC, seq DESC, id DESC LIMIT ?");
	s.Bind(1, afterId).Bind(2, limit);
	return Collect(s, true);
}

std::optional<LogLine> Store::Line(long long id)
{
	Stmt s(db, "SELECT * FROM lines WHERE id=?");
	s.Bind(1, id);
	if (s.Step() != SQLITE_ROW) {
		return std::nullopt;
	}
	return RowToLine(s);
}

std::vector<LogLine> Store::Since(long long startTs, std::optional<long long> endTs)
{
	Stmt s(db, "SELECT * FROM lines WHERE sort_ts BETWEEN ? AND ? ORDER BY sort_ts, seq, id");
	s.Bind(1, startTs).Bind(2, endTs.value_or(2147483647LL));
	return Collect(s);
}

std::vector<LogLine> Store::Search(const QString &query, int limit, const QString &channel)
{
	if (!db) {
		return {};
	}
	auto likeSearch = [&]() {
		std::string sql = "SELECT * FROM lines WHERE body LIKE ?";
		if (!channel.isEmpty()) {
			sql += " AND channel LIKE ?";
		}
		sql += " ORDER BY sort_ts DESC, seq DESC LIMIT ?";
		Stmt s(db, sql.c_str());
		int i = 1;
		s.Bind(i++, QStringLiteral("%%1%").arg(query));
		if (!channel.isEmpty()) {
			s.Bind(i++, QStringLiteral("%%1%").arg(channel));
		}
		s.Bind(i, limit);
		return Collect(s, true);
	};

	if (!fts) {
		return likeSearch();
	}

	std::string sql = "SELECT lines.* FROM lines_fts JOIN lines ON lines.id = lines_fts.rowid"
			  " WHERE lines_fts MATCH ?";
	if (!channel.isEmpty()) {
		sql += " AND lines.channel LIKE ?";
	}
	sql += " ORDER BY sort_ts DESC, seq DESC LIMIT ?";
	Stmt s(db, sql.c_str());
	if (!s) {
		return likeSearch();
	}
	int i = 1;
	s.Bind(i++, query);
	if (!channel.isEmpty()) {
		s.Bind(i++, QStringLiteral("%%1%").arg(channel));
	}
	s.Bind(i, limit);

	std::vector<LogLine> out;
	int rc;
	while ((rc = s.Step()) == SQLITE_ROW) {
		out.push_back(RowToLine(s));
	}
	if (rc != SQLITE_DONE) {
		/* A malformed FTS query (e.g. a bare quote): fall back for this
		 * query only, so the index keeps being maintained */
		return likeSearch();
	}
	std::reverse(out.begin(), out.end());
	return out;
}

std::vector<LogLine> Store::Find(const Query &q)
{
	if (!db) {
		return {};
	}
	/* Filters other than the text, as SQL on "lines" */
	std::string filters;
	auto add = [&](const char *clause) {
		filters += " AND ";
		filters += clause;
	};
	if (!q.channel.isEmpty()) {
		add("lines.channel = ?");
	}
	if (!q.label.isEmpty()) {
		add("lines.labels LIKE ?");
	}
	if (q.from) {
		add("lines.sort_ts >= ?");
	}
	if (q.to) {
		add("lines.sort_ts <= ?");
	}
	if (q.frameId) {
		add("lines.frame_id = ?");
	}
	if (q.withShot) {
		add("lines.frame_id IS NOT NULL");
	}
	const std::string order = q.frameId ? " ORDER BY lines.seq, lines.id LIMIT ?"
					    : " ORDER BY lines.sort_ts DESC, lines.seq DESC, lines.id DESC LIMIT ?";
	const QString text = q.text.trimmed();

	auto run = [&](const std::string &sql, const std::optional<QString> &textArg, int *rc) {
		std::vector<LogLine> out;
		Stmt s(db, sql.c_str());
		if (!s) {
			*rc = SQLITE_ERROR;
			return out;
		}
		int i = 1;
		if (textArg) {
			s.Bind(i++, *textArg);
		}
		if (!q.channel.isEmpty()) {
			s.Bind(i++, q.channel);
		}
		if (!q.label.isEmpty()) {
			/* labels is a JSON array of strings: match the quoted label */
			const QString quoted =
				QString::fromUtf8(QJsonDocument(QJsonArray{q.label}).toJson(QJsonDocument::Compact));
			s.Bind(i++, QStringLiteral("%") + quoted.mid(1, quoted.size() - 2) + QStringLiteral("%"));
		}
		if (q.from) {
			s.Bind(i++, *q.from);
		}
		if (q.to) {
			s.Bind(i++, *q.to);
		}
		if (q.frameId) {
			s.Bind(i++, *q.frameId);
		}
		s.Bind(i, q.limit);
		while ((*rc = s.Step()) == SQLITE_ROW) {
			out.push_back(RowToLine(s));
		}
		if (!q.frameId) {
			std::reverse(out.begin(), out.end());
		}
		return out;
	};

	int rc = SQLITE_DONE;
	if (text.isEmpty()) {
		return run("SELECT * FROM lines WHERE 1" + filters + order, std::nullopt, &rc);
	}
	if (fts) {
		std::vector<LogLine> out = run("SELECT lines.* FROM lines_fts JOIN lines ON lines.id = lines_fts.rowid"
					       " WHERE lines_fts MATCH ?" +
						       filters + order,
					       text, &rc);
		if (rc == SQLITE_DONE) {
			return out;
		}
		/* a malformed FTS query (e.g. a bare quote): plain matching instead */
	}
	return run("SELECT * FROM lines WHERE body LIKE ?" + filters + order, QStringLiteral("%%1%").arg(text), &rc);
}

QStringList Store::Channels()
{
	QStringList out;
	Stmt s(db, "SELECT channel, COUNT(*) n FROM lines WHERE channel IS NOT NULL AND channel != ''"
		   " GROUP BY channel ORDER BY n DESC");
	while (s.Step() == SQLITE_ROW) {
		out << s.Text(0);
	}
	return out;
}

QStringList Store::Labels()
{
	QStringList out;
	Stmt s(db, "SELECT DISTINCT labels FROM lines WHERE labels IS NOT NULL AND labels != '[]'");
	while (s.Step() == SQLITE_ROW) {
		for (const QJsonValue &v : QJsonDocument::fromJson(s.Text(0).toUtf8()).array()) {
			if (!out.contains(v.toString())) {
				out << v.toString();
			}
		}
	}
	out.sort(Qt::CaseInsensitive);
	return out;
}

std::vector<FrameInfo> Store::Frames(int limit, std::optional<long long> from, std::optional<long long> to)
{
	std::vector<FrameInfo> out;
	Stmt s(db, "SELECT frames.id, frames.sort_ts, frames.path, frames.width, frames.height,"
		   " COUNT(lines.id), group_concat(NULLIF(lines.labels, '[]'), ',') FROM frames"
		   " LEFT JOIN lines ON lines.frame_id = frames.id"
		   " WHERE frames.sort_ts BETWEEN ? AND ?"
		   " GROUP BY frames.id ORDER BY frames.sort_ts DESC, frames.id DESC LIMIT ?");
	s.Bind(1, from.value_or(0)).Bind(2, to.value_or(std::numeric_limits<long long>::max())).Bind(3, limit);
	while (s.Step() == SQLITE_ROW) {
		FrameInfo f{{s.Int(0), s.Int(1), s.Text(2), (int)s.Int(3), (int)s.Int(4)}, (int)s.Int(5), {}};
		/* the JSON arrays of the frame's lines, joined by commas */
		const QByteArray all = "[" + s.Text(6).toUtf8() + "]";
		for (const QJsonValue &line : QJsonDocument::fromJson(all).array()) {
			for (const QJsonValue &v : line.toArray()) {
				if (!f.labels.contains(v.toString())) {
					f.labels << v.toString();
				}
			}
		}
		out.push_back(std::move(f));
	}
	return out;
}

int Store::Relabel()
{
	if (!db || !labeler) {
		return 0;
	}
	std::vector<std::pair<long long, QString>> changes;
	{
		Stmt s(db, "SELECT id, body, labels FROM lines");
		while (s.Step() == SQLITE_ROW) {
			const QString labels = TagsJson(labeler(s.Text(1)));
			const QString old = s.Text(2);
			if (labels != (old.isEmpty() ? QStringLiteral("[]") : old)) {
				changes.emplace_back(s.Int(0), labels);
			}
		}
	}
	Exec("BEGIN");
	Stmt u(db, "UPDATE lines SET labels=? WHERE id=?");
	for (const auto &[id, labels] : changes) {
		u.Reset();
		u.Bind(1, labels).Bind(2, id).Run();
	}
	Exec("COMMIT");
	return (int)changes.size();
}

void Store::SetVideo(const std::vector<long long> &lineIds, const VideoSpot &spot)
{
	if (!db || lineIds.empty()) {
		return;
	}
	Exec("BEGIN");
	Stmt u(db, "UPDATE lines SET video=?, video_offset=? WHERE id=?");
	for (long long id : lineIds) {
		u.Reset();
		u.Bind(1, spot.path).Bind(2, spot.offset).Bind(3, id).Run();
	}
	Exec("COMMIT");
}

Stats Store::GetStats()
{
	Stats st;
	{
		Stmt s(db, "SELECT COUNT(*), MIN(sort_ts), MAX(sort_ts), SUM(frames) FROM lines");
		if (s.Step() == SQLITE_ROW) {
			st.lines = s.Int(0);
			if (!s.IsNull(1)) {
				st.first = s.Int(1);
				st.last = s.Int(2);
			}
			st.sightings = s.Int(3);
		}
	}
	{
		Stmt s(db, "SELECT channel, COUNT(*) n FROM lines GROUP BY channel ORDER BY n DESC LIMIT 12");
		while (s.Step() == SQLITE_ROW) {
			st.channels.emplace_back(s.Text(0), s.Int(1));
		}
	}
	{
		Stmt s(db, "SELECT COUNT(*) FROM sessions");
		if (s.Step() == SQLITE_ROW) {
			st.sessions = s.Int(0);
		}
	}
	st.bytes = QFileInfo(path).size();
	return st;
}

int Store::Prune(int days, QStringList *orphanedFiles)
{
	if (days <= 0 || !db) {
		return 0;
	}
	long long cutoff = (long long)(Now() - days * 86400.0);
	std::vector<std::pair<long long, QString>> rows;
	{
		Stmt s(db, "SELECT id, body FROM lines WHERE sort_ts < ?");
		s.Bind(1, cutoff);
		while (s.Step() == SQLITE_ROW) {
			rows.emplace_back(s.Int(0), s.Text(1));
		}
	}
	if (rows.empty()) {
		return 0;
	}
	Exec("BEGIN");
	Stmt d(db, "DELETE FROM lines WHERE id=?");
	for (const auto &r : rows) {
		d.Reset();
		d.Bind(1, r.first).Run();
	}
	/* Screenshots nothing points at any more: delete the rows and hand the
	 * files to the caller (Lucida left the files behind) */
	{
		Stmt s(db, "SELECT path FROM frames WHERE id NOT IN (SELECT DISTINCT frame_id FROM lines"
			   " WHERE frame_id IS NOT NULL)");
		while (s.Step() == SQLITE_ROW) {
			if (orphanedFiles) {
				*orphanedFiles << s.Text(0);
			}
		}
	}
	Exec("DELETE FROM frames WHERE id NOT IN (SELECT DISTINCT frame_id FROM lines WHERE frame_id IS NOT NULL)");
	if (fts) {
		/* A contentless FTS5 table is told what to forget */
		Stmt f(db, "INSERT INTO lines_fts(lines_fts, rowid, body) VALUES ('delete', ?, ?)");
		for (const auto &r : rows) {
			f.Reset();
			f.Bind(1, r.first).Bind(2, r.second).Run();
		}
	}
	Exec("COMMIT");
	return (int)rows.size();
}

/* ------------------------------------------------------------------------- */
/* Repair */

std::optional<RepairResult> Store::Repair(const QString &filePath, QString *error)
{
	auto fail = [&](const QString &msg) -> std::optional<RepairResult> {
		if (error) {
			*error = msg;
		}
		return std::nullopt;
	};
	if (!QFileInfo::exists(filePath)) {
		return fail(QStringLiteral("%1 does not exist").arg(filePath));
	}

	/* Refuse to rebuild a database something else has open */
	{
		sqlite3 *probe = nullptr;
		std::string p = Utf8(filePath);
		sqlite3_open_v2(p.c_str(), &probe, SQLITE_OPEN_READWRITE, nullptr);
		sqlite3_busy_timeout(probe, 1000);
		int rc = sqlite3_exec(probe, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr);
		if (rc == SQLITE_OK) {
			sqlite3_exec(probe, "ROLLBACK", nullptr, nullptr, nullptr);
		}
		QString msg = QString::fromUtf8(sqlite3_errmsg(probe));
		sqlite3_close(probe);
		if (rc != SQLITE_OK) {
			return fail(QStringLiteral("%1 is open in another program - close it and try again. "
						   "SQLite said: %2")
					    .arg(QFileInfo(filePath).fileName(), msg));
		}
	}

	QFileInfo info(filePath);
	QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
	QString backup = info.dir().filePath(
		QStringLiteral("%1.damaged-%2.%3").arg(info.completeBaseName(), stamp, info.suffix()));
	QString repaired =
		info.dir().filePath(QStringLiteral("%1.repaired.%2").arg(info.completeBaseName(), info.suffix()));
	QFile::remove(repaired);

	RepairResult result;
	result.backup = backup;
	{
		sqlite3 *source = nullptr;
		std::string p = Utf8(filePath);
		sqlite3_open_v2(p.c_str(), &source, SQLITE_OPEN_READONLY, nullptr);

		Store target(repaired);
		if (!target.Open(error)) {
			sqlite3_close(source);
			return std::nullopt;
		}
		for (const char *table : {"sessions", "frames", "lines"}) {
			std::set<std::string> targetColumns;
			{
				Stmt ti(target.db, (std::string("PRAGMA table_info(") + table + ")").c_str());
				while (ti.Step() == SQLITE_ROW) {
					targetColumns.insert(Utf8(ti.Text(1)));
				}
			}
			int kept = 0, dropped = 0;
			Stmt rows(source, (std::string("SELECT * FROM ") + table).c_str());
			if (rows) {
				target.Exec("BEGIN");
				while (true) {
					int rc = rows.Step();
					if (rc == SQLITE_DONE) {
						break;
					}
					if (rc != SQLITE_ROW) {
						dropped++;
						if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
							break;
						}
						continue;
					}
					std::vector<int> cols;
					std::string names, marks;
					int n = sqlite3_column_count(rows.stmt);
					for (int c = 0; c < n; c++) {
						std::string name = sqlite3_column_name(rows.stmt, c);
						if (!targetColumns.count(name)) {
							continue;
						}
						cols.push_back(c);
						names += (names.empty() ? "" : ",") + name;
						marks += marks.empty() ? "?" : ",?";
					}
					Stmt ins(target.db, (std::string("INSERT INTO ") + table + "(" + names +
							     ") VALUES (" + marks + ")")
								    .c_str());
					for (size_t i = 0; i < cols.size(); i++) {
						sqlite3_bind_value(ins.stmt, (int)i + 1,
								   sqlite3_column_value(rows.stmt, cols[i]));
					}
					if (ins.Run()) {
						kept++;
					} else {
						dropped++;
					}
				}
				target.Exec("COMMIT");
			}
			result.copied[table] = kept;
			result.lost[table] = dropped;
		}
		if (target.fts) {
			/* The index is derived: rebuild rather than copy it */
			target.Exec("BEGIN");
			target.Exec("DELETE FROM lines_fts");
			target.Exec("INSERT INTO lines_fts(rowid, body) SELECT id, body FROM lines");
			target.Exec("COMMIT");
		}
		result.integrity = target.Pragma("integrity_check");
		sqlite3_close(source);
	}

	QFile::remove(filePath + "-wal");
	QFile::remove(filePath + "-shm");
	if (!QFile::rename(filePath, backup) || !QFile::rename(repaired, filePath)) {
		return fail(QStringLiteral("Could not swap the repaired log into place"));
	}
	return result;
}

} // namespace lucida
