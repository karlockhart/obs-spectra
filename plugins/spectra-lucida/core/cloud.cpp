#include "cloud.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <set>

namespace lucida {

namespace {

const QString kBase = QStringLiteral("/v1/lucida/sources/me");
constexpr int kBatch = 100;
constexpr int kSessionsPerPass = 50;
/* Prisma's limits (plugins/lucida/api.py) */
constexpr int kMaxBody = 4000;
constexpr int kMaxKey = 4096;
constexpr int kMaxTags = 32;
constexpr int kMaxTag = 64;
constexpr int kMaxChannel = 64;
constexpr int kMaxClock = 16;
constexpr long long kMaxTs = 100000000000LL;
constexpr int kMaxSeq = 100000;

double Now()
{
	return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

bool ValidKey(const QString &key)
{
	static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
	return re.match(key).hasMatch();
}

QJsonArray Strings(const QStringList &list)
{
	QJsonArray out;
	for (const QString &s : list) {
		if (out.size() >= kMaxTags) {
			break;
		}
		if (!s.isEmpty()) {
			out.append(s.left(kMaxTag));
		}
	}
	return out;
}

/* Failures worth retrying later (no answer, the server, auth, throttling);
 * any other refusal is about the row itself and would fail again */
bool Transient(const PrismaResult &r)
{
	return r.status == 0 || r.status >= 500 || r.status == 401 || r.status == 403 || r.status == 408 ||
	       r.status == 429;
}

QString ContentType(const QString &path)
{
	const QString ext = QFileInfo(path).suffix().toLower();
	if (ext == QLatin1String("png")) {
		return QStringLiteral("image/png");
	}
	if (ext == QLatin1String("webp")) {
		return QStringLiteral("image/webp");
	}
	return QStringLiteral("image/jpeg");
}

} // namespace

QString CloudCredentialsPath(const QString &configured, const QStringList &folders)
{
	if (!configured.isEmpty()) {
		return configured;
	}
	QStringList all = folders;
	const QString appData = qEnvironmentVariable("APPDATA");
	if (!appData.isEmpty()) {
		all << QDir(appData).filePath(QStringLiteral("Lucida"));
	}
	return FindCredentials(all);
}

QString FrameKey(const QString &path)
{
	const QString stem = QFileInfo(path).completeBaseName();
	return ValidKey(stem) ? stem : QString();
}

QString SessionKey(double started)
{
	return QString::number((long long)started);
}

QJsonObject LineJson(const SyncLine &s)
{
	const LogLine &l = s.line;
	QJsonObject o{
		{"sort_ts", std::clamp(l.sortTs, 0LL, kMaxTs - 1)},
		{"seq", std::clamp(l.seq, 0, kMaxSeq - 1)},
		{"ts_source", l.tsSource.isEmpty() ? QStringLiteral("wall") : l.tsSource.left(16)},
		{"body", l.body.left(kMaxBody)},
		{"dedup_key", (s.dedupKey.isEmpty() ? l.body : s.dedupKey).left(kMaxKey)},
		{"score", std::clamp(l.score, 0.0, 1.0)},
		{"frames", std::max(0, l.frames)},
		{"first_seen", l.firstSeen},
		{"last_seen", std::max(l.lastSeen, l.firstSeen)},
		{"tags", Strings(l.tags)},
		{"local_id", l.id},
		/* Spectra's additions: the tag rules' labels and carnivore mode's region */
		{"labels", Strings(l.labels)},
	};
	if (l.clock && !l.clock->isEmpty()) {
		o.insert("clock", l.clock->left(kMaxClock));
	}
	if (!l.channel.isEmpty()) {
		o.insert("channel", l.channel.left(kMaxChannel));
	}
	if (!l.region.isEmpty()) {
		o.insert("region", l.region.left(kMaxChannel));
	}
	if (l.rect) {
		o.insert("rect", QJsonArray{l.rect->x0, l.rect->y0, l.rect->x1, l.rect->y1});
	}
	if (s.framePath) {
		const QString key = FrameKey(*s.framePath);
		if (!key.isEmpty()) {
			o.insert("frame_key", key);
		}
	}
	if (s.sessionStarted) {
		o.insert("session_key", SessionKey(*s.sessionStarted));
	}
	return o;
}

CloudSync::CloudSync(Store &store_, PrismaClient &client_) : store(store_), client(client_) {}

bool CloudSync::Introduce(SyncReport &report)
{
	/* a log backed up to another gateway or as another client starts over */
	const PrismaCredentials &c = client.Credentials();
	const QString target = c.url + '|' + c.clientId;
	if (store.Meta("prisma_target") != target) {
		store.ResetSync();
		store.SetMeta("prisma_target", target);
	}
	QJsonObject info;
	if (!machine.isEmpty()) {
		info.insert("machine", machine.left(128));
	}
	if (!version.isEmpty()) {
		info.insert("version", version.left(32));
	}
	const PrismaResult r = client.Put(kBase, info);
	if (!r.Ok()) {
		report.error = r.Describe();
		return false;
	}
	introduced = true;
	return true;
}

bool CloudSync::SendSessions(SyncReport &report)
{
	for (const SyncSession &s : store.UnsyncedSessions(kSessionsPerPass)) {
		QJsonObject body{{"started", s.started}};
		if (s.ended) {
			body.insert("ended", *s.ended);
		}
		if (!s.target.isEmpty()) {
			body.insert("target", s.target.left(256));
		}
		if (s.width > 0 && s.height > 0) {
			body.insert("width", s.width);
			body.insert("height", s.height);
		}
		const PrismaResult r = client.Put(kBase + "/sessions/" + SessionKey(s.started), body);
		if (r.Ok()) {
			store.MarkSessionSynced(s, Now());
			report.sessions++;
		} else if (Transient(r)) {
			report.error = r.Describe();
			return false;
		} else {
			store.MarkSessionSynced(s, -1);
			report.refused++;
		}
	}
	return true;
}

bool CloudSync::SendFrames(SyncReport &report, int maxFrames)
{
	for (const SyncFrame &f : store.UnsyncedFrames(maxFrames)) {
		const QString key = FrameKey(f.frame.path);
		QFile file(f.frame.path);
		if (key.isEmpty() || !file.open(QIODevice::ReadOnly)) {
			/* pruned from disk, or a name Prisma cannot take */
			store.MarkFrameSynced(f.frame.id, -1);
			report.refused++;
			continue;
		}
		QJsonObject info{{"sort_ts", f.frame.sortTs}, {"content_type", ContentType(f.frame.path)}};
		if (f.frame.width > 0 && f.frame.height > 0) {
			info.insert("width", f.frame.width);
			info.insert("height", f.frame.height);
		}
		if (f.sessionStarted) {
			info.insert("session_key", SessionKey(*f.sessionStarted));
		}
		const PrismaResult r = client.UploadImage(kBase + "/frames/" + key, file.readAll(), info);
		if (r.Ok()) {
			store.MarkFrameSynced(f.frame.id, Now());
			report.frames++;
		} else if (Transient(r) || r.status == 409 || r.status == 404) {
			/* 409: storage has not got the image yet; 404 (from /uploaded): the
			 * record went away, so the next pass PUTs it and uploads again */
			report.error = r.Describe();
			return false;
		} else {
			store.MarkFrameSynced(f.frame.id, -1);
			report.refused++;
		}
	}
	return true;
}

bool CloudSync::SendLines(SyncReport &report, int maxBatches)
{
	for (int batch = 0; batch < maxBatches; batch++) {
		const std::vector<SyncLine> lines = store.UnsyncedLines(kBatch);
		if (lines.empty()) {
			return true;
		}
		QJsonArray list;
		for (const SyncLine &l : lines) {
			list.append(LineJson(l));
		}
		const PrismaResult r = client.Post(kBase + "/lines", QJsonObject{{"lines", list}});
		if (r.Ok()) {
			store.MarkLinesSynced(lines, Now());
			report.lines += (int)lines.size();
			continue;
		}
		if (Transient(r)) {
			report.error = r.Describe();
			return false;
		}
		/* One bad line fails the whole batch. A 422 names the bad ones
		 * (detail[].loc = ["body", "lines", <index>, ...]): refuse those and
		 * send the rest again; otherwise try the lines one at a time. */
		std::set<int> bad;
		for (const QJsonValue &d : r.body.toObject().value("detail").toArray()) {
			const QJsonArray loc = d.toObject().value("loc").toArray();
			if (loc.size() >= 3 && loc[1] == QLatin1String("lines") && loc[2].isDouble()) {
				bad.insert(loc[2].toInt());
			}
		}
		if (r.status == 422 && !bad.empty() && *bad.rbegin() < (int)lines.size()) {
			std::vector<SyncLine> refused, rest;
			QJsonArray again;
			for (int i = 0; i < (int)lines.size(); i++) {
				if (bad.count(i)) {
					refused.push_back(lines[i]);
				} else {
					rest.push_back(lines[i]);
					again.append(LineJson(lines[i]));
				}
			}
			store.MarkLinesSynced(refused, -1);
			report.refused += (int)refused.size();
			if (rest.empty()) {
				continue;
			}
			const PrismaResult retry = client.Post(kBase + "/lines", QJsonObject{{"lines", again}});
			if (retry.Ok()) {
				store.MarkLinesSynced(rest, Now());
				report.lines += (int)rest.size();
				continue;
			}
			if (Transient(retry)) {
				report.error = retry.Describe();
				return false;
			}
		}
		for (const SyncLine &l : lines) {
			const PrismaResult one =
				client.Post(kBase + "/lines", QJsonObject{{"lines", QJsonArray{LineJson(l)}}});
			if (one.Ok()) {
				store.MarkLinesSynced({l}, Now());
				report.lines++;
			} else if (Transient(one)) {
				report.error = one.Describe();
				return false;
			} else {
				store.MarkLinesSynced({l}, -1);
				report.refused++;
			}
		}
	}
	return true;
}

SyncReport CloudSync::Step(int maxFrames, int maxBatches)
{
	SyncReport report;
	/* screenshots before the lines that point at them */
	if ((introduced || Introduce(report)) && SendSessions(report) && (!frames || SendFrames(report, maxFrames))) {
		SendLines(report, maxBatches);
	}
	const SyncBacklog left = store.Unsynced();
	report.more = left.lines > 0 || left.sessions > 0 || (frames && left.frames > 0);
	return report;
}

} // namespace lucida
