#include "SpectraEvents.hpp"

#include <OBSApp.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cstdlib>

namespace SpectraEvents {

QString FilePath(const QString &folder)
{
	return QDir(folder).filePath(QStringLiteral("spectra-events.json"));
}

QString SidecarPath(const QString &videoFile)
{
	const QFileInfo info(videoFile);
	return info.dir().filePath(info.completeBaseName() + QStringLiteral(".events.json"));
}

std::vector<Event> LoadFile(const QString &eventsFile)
{
	std::vector<Event> events;
	QFile file(eventsFile);
	if (!file.open(QIODevice::ReadOnly)) {
		return events;
	}
	const QJsonArray array = QJsonDocument::fromJson(file.readAll()).object().value("events").toArray();
	for (const QJsonValue &value : array) {
		const QJsonObject o = value.toObject();
		Event e;
		e.time = QDateTime::fromString(o.value("time").toString(), Qt::ISODateWithMs);
		e.name = o.value("name").toString();
		e.offset = o.value("offset").toDouble(-1.0);
		e.source = o.value("source").toString();
		e.detail = o.value("detail").toString();
		if (e.time.isValid() || e.offset >= 0.0) {
			events.push_back(std::move(e));
		}
	}
	std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
		return a.offset >= 0.0 && b.offset >= 0.0 ? a.offset < b.offset : a.time < b.time;
	});
	return events;
}

std::vector<Event> Load(const QString &folder)
{
	return LoadFile(FilePath(folder));
}

bool SaveFile(const QString &eventsFile, const std::vector<Event> &events)
{
	QJsonArray array;
	for (const Event &e : events) {
		QJsonObject o;
		if (e.time.isValid()) {
			o["time"] = e.time.toString(Qt::ISODateWithMs);
		}
		if (e.offset >= 0.0) {
			o["offset"] = e.offset;
		}
		if (!e.name.isEmpty()) {
			o["name"] = e.name;
		}
		if (!e.source.isEmpty()) {
			o["source"] = e.source;
		}
		if (!e.detail.isEmpty()) {
			o["detail"] = e.detail;
		}
		array.append(o);
	}
	QJsonObject root;
	root["events"] = array;

	QDir().mkpath(QFileInfo(eventsFile).absolutePath());
	QSaveFile file(eventsFile);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return file.commit();
}

bool Add(const QString &folder, const Event &event)
{
	std::vector<Event> events = Load(folder);
	const QDateTime cutoff = QDateTime::currentDateTime().addDays(-KeepDays);
	events.erase(std::remove_if(events.begin(), events.end(),
				    [&](const Event &e) { return e.time.isValid() && e.time < cutoff; }),
		     events.end());
	events.push_back(event);
	return SaveFile(FilePath(folder), events);
}

bool Rename(const QString &eventsFile, const QDateTime &time, const QString &name)
{
	std::vector<Event> events = LoadFile(eventsFile);
	bool found = false;
	for (Event &e : events) {
		if (e.time.isValid() && std::abs(e.time.msecsTo(time)) <= 1) {
			e.name = name.trimmed();
			found = true;
		}
	}
	return found && SaveFile(eventsFile, events);
}

QColor PinColor(const Event &event)
{
	/* amber is the user's alone; Lucida's are violet */
	return event.FromLucida() ? QColor(0xb0, 0x7c, 0xff) : QColor(0xff, 0xb0, 0x20);
}

QString PinText(const Event &event)
{
	QString text = DisplayName(event);
	if (event.FromLucida()) {
		QString line = event.detail.simplified();
		if (line.size() > 90) {
			line = line.left(90) + QStringLiteral("...");
		}
		text += QStringLiteral("\n") + QTStr(event.source == "lucida-speech" ? "Spectra.Event.FromLucidaSpeech"
										     : "Spectra.Event.FromLucida")
						       .arg(line);
	}
	return text;
}

QString DisplayName(const Event &event)
{
	return event.name.isEmpty() ? QTStr("Spectra.Event.Default") : event.name;
}

} // namespace SpectraEvents
