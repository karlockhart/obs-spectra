#pragma once

/*
 * Events marked during the loop recording with the event hotkeys: "Event"
 * (no name) and up to NamedSlots named ones whose names are set in the Loop
 * Recording Settings (profile config SpectraLoop EventName1..). They are kept
 * by wall-clock time in spectra-events.json in the loop folder, so they still
 * line up after segments are archived or the quota deletes some, and the
 * Clip Maker shows them as pins on its timeline; double-clicking a pin names
 * (or renames) its event.
 *
 * Backups and archives take their events along (SpectraLoopArchive): a folder
 * of segments gets its own spectra-events.json, a block saved as one file a
 * "<file>.events.json" next to it that also has each event's offset into the
 * file, which the Clip Maker reads when it opens the file.
 */

#include <QColor>
#include <QDateTime>
#include <QString>

#include <vector>

namespace SpectraEvents {

constexpr int NamedSlots = 5;
/* Events older than this are dropped from the loop folder's file when a new
 * one is added (backups and archives keep theirs) */
constexpr int KeepDays = 60;

struct Event {
	QDateTime time;
	QString name;         /* empty: a plain "Event" */
	double offset = -1.0; /* seconds into the file, in a file's sidecar */
	/* who marked it: empty for the user (the event hotkeys), "lucida" or
	 * "lucida-speech" for Lucida's event triggers */
	QString source;
	QString detail; /* the line a trigger matched */

	bool FromLucida() const { return source.startsWith(QStringLiteral("lucida")); }
};

/* Pin colours: the user's are reserved (no trigger ever gets them) */
QColor PinColor(const Event &event);
/* A pin's hover text: the name, and the matched line for Lucida's */
QString PinText(const Event &event);

/* The loop folder's (or a folder of archived segments') events */
QString FilePath(const QString &folder);
/* The events saved next to one video file: "<file without extension>.events.json" */
QString SidecarPath(const QString &videoFile);

std::vector<Event> Load(const QString &folder);
std::vector<Event> LoadFile(const QString &eventsFile);
bool SaveFile(const QString &eventsFile, const std::vector<Event> &events);

bool Add(const QString &folder, const Event &event);
/* Names the event at that time (to the millisecond); an empty name makes it
 * a plain "Event" again */
bool Rename(const QString &eventsFile, const QDateTime &time, const QString &name);

/* What a pin shows: the name, or "Event" */
QString DisplayName(const Event &event);

} // namespace SpectraEvents
