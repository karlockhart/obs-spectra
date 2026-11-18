#pragma once

/*
 * Exporting clips and loop recordings to Prisma, through Lucida (which holds
 * the Prisma connection: Lucida Settings > Cloud > Connect to Prisma). Lucida
 * queues each export and uploads it in the background, resuming after a
 * restart; see plugins/spectra-lucida/plugin/lucida-prisma.hpp. Without
 * Lucida (Lite, Ultralight, or switched off) or a connection, Ready() is
 * false and nothing offers an export.
 */

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace SpectraPrisma {

bool Ready();

/* kind: "clips" or "loops". Queued in Lucida; false when it isn't there */
bool Export(const QString &kind, const QString &path, const QString &title, const QString &game,
	    const QDateTime &started, double duration, const QStringList &tags);

} // namespace SpectraPrisma
