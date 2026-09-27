#pragma once

#include "prisma.hpp"
#include "store.hpp"

#include <QJsonObject>
#include <QString>

namespace lucida {

/* Backing the log up to Prisma's Lucida plugin (/v1/lucida, see Prisma's
 * docs/lucida.md). This install is one source; lines, sessions and
 * screenshots are upserted, so anything can be sent again safely. The
 * local log stays the original: pruning it deletes nothing in the cloud. */

/* The credentials file to use: the configured one, else the first
 * prisma-*.json next to the log, in Spectra's Lucida folder or in
 * %APPDATA%\Lucida (where Lucida keeps it). Empty if there is none. */
QString CloudCredentialsPath(const QString &configured, const QStringList &folders);

/* A line as Prisma takes it, clipped to Prisma's limits */
QJsonObject LineJson(const SyncLine &line);
/* A screenshot's key: its file name without extension, if Prisma accepts it */
QString FrameKey(const QString &path);
/* A session's key: when it started, in whole seconds */
QString SessionKey(double started);

struct SyncReport {
	int sessions = 0;
	int frames = 0;
	int lines = 0;
	int refused = 0;   /* rows Prisma rejected; not retried */
	bool more = false; /* still waiting after this pass */
	QString error;     /* the pass stopped here; try again later */
};

class CloudSync {
public:
	CloudSync(Store &store, PrismaClient &client);

	/* Upload screenshots too (else only lines and sessions) */
	bool frames = true;
	/* Describes this install to Prisma */
	QString machine;
	QString version;

	/* One pass: sessions, then up to maxFrames screenshots, then up to
	 * maxBatches batches of 100 lines, oldest first */
	SyncReport Step(int maxFrames = 5, int maxBatches = 10);

private:
	Store &store;
	PrismaClient &client;
	bool introduced = false;

	bool Introduce(SyncReport &report);
	bool SendSessions(SyncReport &report);
	bool SendFrames(SyncReport &report, int maxFrames);
	bool SendLines(SyncReport &report, int maxBatches);
};

} // namespace lucida
