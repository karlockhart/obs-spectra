#pragma once

#include "lan.hpp"
#include "store.hpp"

#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace lucida::lan {

/* A storage PC: a paired PC that keeps other PCs' loop recordings and logs.
 *
 * A PC that stores on one sends it each loop segment once the segment is
 * finished (newest first, resuming a send that was cut off), and its log's
 * lines, sessions and screenshots as the Prisma backup does. It keeps its
 * own loop as it is: its quota makes it a buffer. The storage PC puts each
 * PC's recordings in a folder of their own and deletes the oldest segments,
 * whoever sent them, to stay under its quota, as the loop recording does
 * with its own; the log becomes a copy of that PC's chatlog.db, which the
 * log browser opens like any other. Only paired PCs are answered, and only
 * while "Store recordings for paired PCs" is on. */

/* A loop segment's file name as Spectra writes them ("2026-09-23
 * 14-05-00.mkv"), and nothing that could be a path */
bool IsSegmentName(const QString &name);

/* A log line as it is sent to the storage PC: every column, so the copy is
 * the original. A loop segment is sent as its file name. */
QJsonObject StoredLineJson(const SyncLine &line);
SyncLine StoredLineFromJson(const QJsonObject &o);

/* A PC whose recordings are stored here */
struct StoredSource {
	QString id; /* its key */
	QString name;
	QString folder; /* its folder in the storage */
	int segments = 0;
	qint64 bytes = 0;
	qint64 newest = 0; /* when its newest segment arrived, s since the epoch */

	QString LoopFolder() const;
	QString LogPath() const;
};

class StorageNode {
public:
	StorageNode(const QString &folder, quint64 quotaBytes);
	StorageNode(const StorageNode &) = delete;
	StorageNode &operator=(const StorageNode &) = delete;

	QString Folder() const { return root; }
	quint64 Quota() const { return quota; }
	/* How long the stored logs keep lines and screenshots (0: forever) */
	std::atomic<int> retentionDays{0};
	std::atomic<int> frameRetentionDays{0};

	/* Answers a paired PC's store_* request (Provider::store) */
	void Answer(Channel &channel, const QString &peerId, const QString &peerName, const QJsonObject &request);

	std::vector<StoredSource> Sources();
	/* Every stored segment and unfinished send, in bytes */
	quint64 UsedBytes();
	/* Deletes the oldest segments until the storage fits its quota; keep
	 * is never deleted */
	void EnforceQuota(const QString &keep = QString());

	/* Something arrived; called on the thread that received it */
	std::function<void()> changed;

private:
	QString root;
	quint64 quota;

	std::mutex mutex;                                       /* folders, receiving, the quota */
	std::map<QString, std::pair<QString, QString>> folders; /* id -> name, folder */
	std::set<QString> receiving;                            /* paths being written */

	std::mutex storesMutex;
	std::map<QString, std::unique_ptr<Store>> stores; /* by log path */
	std::map<QString, qint64> pruned;                 /* log path -> when, ms */

	void LoadSources();
	void SaveSources();
	/* The folder for a PC's recordings and log, made on first use */
	QString SourceFolder(const QString &id, const QString &name);
	Store *OpenStore(const QString &path);

	void AnswerSegment(Channel &channel, const QString &folder, const QJsonObject &request);
	void AnswerLog(Channel &channel, const QString &folder, const QJsonObject &request);
	void AnswerFrame(Channel &channel, const QString &folder, const QJsonObject &request);
	/* Takes the data after an {"offset"} answer into file, up to size bytes */
	bool Receive(Channel &channel, QFile &file, qint64 size, QString *error);
	void Changed();
};

/* What one pass of sending to the storage PC did */
struct PushReport {
	int sessions = 0;
	int frames = 0;
	int lines = 0;
	int segments = 0;
	int skipped = 0;   /* segments it had no room for */
	bool more = false; /* still waiting after this pass */
	QString error;     /* the pass stopped here; try again later */
};

/* Sending this PC's loop recordings and log to its storage PC */
class StorageClient {
public:
	/* stateFile remembers which segments were sent (and to which PC) */
	StorageClient(Service &service, const QString &stateFile);

	/* One pass: the log first (sessions, up to maxFrames screenshots, up to
	 * maxBatches batches of lines), then the newest finished segment not
	 * sent yet. store may be null (no log). progress gets the segment being
	 * sent and how far along it is, and returns false to stop. */
	PushReport Step(const Peer &node, Store *store, const QString &loopDir, bool recording,
			const std::function<bool(const QString &segment, qint64 sent, qint64 size)> &progress = {},
			int maxFrames = 5, int maxBatches = 10);

	/* Finished segments not sent yet, newest first (the one being written
	 * is not finished) */
	QStringList Waiting(const QString &loopDir, bool recording);

private:
	Service &service;
	QString stateFile;
	bool loaded = false;
	QString node;
	std::set<QString> sent;

	void Load();
	void Save();
	/* A new storage PC starts from nothing */
	void UseNode(const QString &id, Store *store);
	bool SendLog(const Peer &node, Store &store, PushReport &report, int maxFrames, int maxBatches);
	bool SendSegment(const Peer &node, const QString &path, PushReport &report,
			 const std::function<bool(const QString &, qint64, qint64)> &progress);
};

} // namespace lucida::lan
