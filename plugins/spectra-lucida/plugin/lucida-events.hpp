#pragma once

/*
 * Lucida's event triggers: when a logged chat or speech line matches one of
 * a rule's triggers, Spectra pins an event of the rule's name on the loop
 * recording (a pin of Lucida's own colour in the Clip Maker), and with "auto
 * clip" saves a clip around the moment too. Matching is the tag rules' own
 * (Tagger: case, spaces and hyphens ignored, * and ? wildcards, optional typo
 * tolerance).
 *
 * Spectra is reached through its spectra_add_event proc, so nothing happens
 * without OBS-Spectra's frontend (Lucida Viewer, tests).
 */

#include "tagger.hpp"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <mutex>

namespace lucida {

struct EventRule {
	QString name;
	QStringList triggers;
	bool clip = false; /* also save a clip around the moment */
	bool operator==(const EventRule &) const = default;
};

QString EventRulesToJson(const QList<EventRule> &rules);
QList<EventRule> EventRulesFromJson(const QString &json);

struct EventSettings {
	QList<EventRule> rules;
	/* an auto clip: this long before the line and after it */
	int clipBefore = 30;
	int clipAfter = 10;
	/* the same rule fires again only after this long */
	int cooldown = 30;
	bool operator==(const EventSettings &) const = default;
};

class EventTriggers {
public:
	static EventTriggers &Get();

	void Apply(const EventSettings &settings, bool tolerateTypos);
	/* A line just logged (any thread): wallTs in seconds since the epoch */
	void Check(const QString &body, double wallTs, bool speech);

private:
	std::mutex mutex;
	EventSettings settings;
	std::unique_ptr<Tagger> tagger;
	QHash<QString, bool> clips;
	QHash<QString, double> lastFired;
};

} // namespace lucida
