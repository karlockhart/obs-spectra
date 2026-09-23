#pragma once

#include "video.hpp"

#include <spectra-vision/chat.hpp>

#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace lucida {

/* The chat log: one SQLite file holding every chat line once, in time order
 * (port of lucida/store.py, schema-compatible with Lucida's chatlog.db).
 *
 * Lines are keyed on in-game clock + normalised body; a near-match against
 * the lines still inside the dedup window is the same line. Ordering uses the
 * HUD unix clock of the frame that first showed a line, then its on-screen
 * position. */

QString Normalise(const QString &text);
QString DedupKey(const std::optional<QString> &clock, const QString &body);

struct LogLine {
	long long id = 0;
	long long sortTs = 0;
	int seq = 0;
	QString tsSource;
	std::optional<QString> clock;
	QString channel;
	QStringList tags;
	QString body;
	double score = 0.0;
	int frames = 0;
	std::optional<long long> frameId;
	std::optional<spectra::Rect> rect;
	/* Spectra additions */
	QStringList labels;                           /* from the tag rules */
	std::optional<std::array<double, 13>> colour; /* for Obscura's learner */
	double firstSeen = 0.0;                       /* wall clock, s */
	std::optional<VideoSpot> video;               /* loop segment when first seen */

	QString When() const;
};

/* What the log browser asks for; empty fields match everything */
struct Query {
	QString text;
	QString channel;
	QString label;
	std::optional<long long> from; /* sort_ts range */
	std::optional<long long> to;
	std::optional<long long> frameId;
	bool withShot = false;
	int limit = 500;
};

struct Frame {
	long long id = 0;
	long long sortTs = 0;
	QString path;
	int width = 0;
	int height = 0;
};

struct FrameInfo {
	Frame frame;
	int lines = 0;
	QStringList labels;
};

struct Stats {
	long long lines = 0;
	long long sightings = 0;
	std::optional<long long> first;
	std::optional<long long> last;
	long long sessions = 0;
	long long bytes = 0;
	std::vector<std::pair<QString, long long>> channels;
};

struct RepairResult {
	QString backup;
	std::unordered_map<std::string, int> copied;
	std::unordered_map<std::string, int> lost;
	QString integrity;
};

class Store {
public:
	explicit Store(const QString &path, double dedupWindow = 900.0);
	~Store();
	Store(const Store &) = delete;
	Store &operator=(const Store &) = delete;

	/* Opens (creating/migrating) the database; false with error on failure */
	bool Open(QString *error = nullptr);
	void Close();
	bool IsOpen() const { return db != nullptr; }
	bool HasFts() const { return fts; }
	const QString &Path() const { return path; }

	double dedupWindow;

	long long StartSession(const QString &target, int width, int height);
	void EndSession(long long sessionId);

	/* Records a frame's entries; returns the ids of the new lines */
	std::vector<long long> AddFrame(const std::vector<spectra::ChatEntry> &entries, long long frameTs,
					const QString &tsSource = "wall", std::optional<long long> sessionId = {});

	long long AttachFrame(const std::vector<long long> &lineIds, const QString &path, int width, int height,
			      long long sortTs, std::optional<long long> sessionId = {});
	std::optional<Frame> GetFrame(long long frameId);
	std::vector<LogLine> FrameLines(long long frameId);
	/* Forgets screenshots older than days; returns the files to delete */
	QStringList PruneFrames(int days);

	std::vector<LogLine> Recent(int limit = 50, long long afterId = 0);
	std::optional<LogLine> Line(long long id);
	std::vector<LogLine> Since(long long startTs, std::optional<long long> endTs = {});
	std::vector<LogLine> Search(const QString &query, int limit = 50, const QString &channel = QString());
	/* The newest q.limit lines matching q, oldest first */
	std::vector<LogLine> Find(const Query &q);
	QStringList Channels();
	QStringList Labels();
	/* Kept screenshots, newest first */
	std::vector<FrameInfo> Frames(int limit = 200, std::optional<long long> from = {},
				      std::optional<long long> to = {});

	/* Tags lines as they are added (and when a better reading replaces one) */
	std::function<QStringList(const QString &body)> labeler;
	/* Re-tags every line with labeler; returns how many changed */
	int Relabel();
	void SetVideo(const std::vector<long long> &lineIds, const VideoSpot &spot);
	Stats GetStats();
	/* Drops lines older than days. Returns how many; orphaned screenshot
	 * files are added to orphanedFiles for the caller to delete. */
	int Prune(int days, QStringList *orphanedFiles = nullptr);

	/* Salvages a damaged log into a fresh file, keeping the original as
	 * <stem>.damaged-<date>.db. Fails if the file is open elsewhere. */
	static std::optional<RepairResult> Repair(const QString &path, QString *error = nullptr);

	/* Direct access for tests and maintenance */
	sqlite3 *Db() const { return db; }
	QString Pragma(const char *name);

private:
	struct WindowLine {
		long long rowId;
		QString key;
		std::optional<QString> clock;
		QString body;
		double score;
		long long sortTs; /* first seen */
		long long seenTs; /* last seen; the dedup window counts from here */
	};

	QString path;
	sqlite3 *db = nullptr;
	bool fts = false;
	std::unordered_map<std::string, WindowLine> recent;
	std::unordered_map<std::string, std::vector<std::string>> byClock;

	bool Exec(const char *sql);
	void Migrate();
	bool TryFts();
	void LoadWindow();
	void Remember(const WindowLine &item);
	void PruneWindow(long long nowTs);
	WindowLine *Match(const QString &key, const std::optional<QString> &clock, const QString &body);
	void Touch(WindowLine &hit, const QString &body, double score, long long frameTs, double now);
	long long Insert(const spectra::ChatEntry &entry, const QString &body, const QString &key, double score,
			 long long frameTs, int seq, const QString &tsSource, std::optional<long long> sessionId,
			 double now);
};

} // namespace lucida
