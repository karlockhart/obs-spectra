#include "lucida-events.hpp"

#include <obs.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace lucida {

QString EventRulesToJson(const QList<EventRule> &rules)
{
	QJsonArray array;
	for (const EventRule &rule : rules) {
		QJsonObject o;
		o["event"] = rule.name;
		o["triggers"] = QJsonArray::fromStringList(rule.triggers);
		o["clip"] = rule.clip;
		array.append(o);
	}
	return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<EventRule> EventRulesFromJson(const QString &json)
{
	QList<EventRule> rules;
	for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
		const QJsonObject o = value.toObject();
		EventRule rule;
		rule.name = o.value("event").toString().trimmed();
		for (const QJsonValue &t : o.value("triggers").toArray()) {
			if (!t.toString().trimmed().isEmpty()) {
				rule.triggers << t.toString().trimmed();
			}
		}
		rule.clip = o.value("clip").toBool();
		if (!rule.name.isEmpty() && !rule.triggers.isEmpty()) {
			rules << rule;
		}
	}
	return rules;
}

EventTriggers &EventTriggers::Get()
{
	static EventTriggers triggers;
	return triggers;
}

void EventTriggers::Apply(const EventSettings &s, bool tolerateTypos)
{
	/* the rules as tag rules: the "tag" is the event's name */
	QList<TagRule> asTags;
	QHash<QString, bool> clipNames;
	for (const EventRule &rule : s.rules) {
		asTags << TagRule{rule.name, rule.triggers};
		clipNames[rule.name] = clipNames.value(rule.name) || rule.clip;
	}
	std::lock_guard<std::mutex> lock(mutex);
	settings = s;
	tagger = asTags.isEmpty() ? nullptr : std::make_unique<Tagger>(asTags, tolerateTypos);
	clips = clipNames;
}

void EventTriggers::Check(const QString &body, double wallTs, bool speech)
{
	struct Fire {
		QString name;
		bool clip;
	};
	std::vector<Fire> fire;
	int before, after;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!tagger) {
			return;
		}
		for (const QString &name : tagger->Tags(body)) {
			auto last = lastFired.find(name);
			if (last != lastFired.end() && wallTs - last.value() < settings.cooldown) {
				continue;
			}
			lastFired[name] = wallTs;
			fire.push_back({name, clips.value(name)});
		}
		before = settings.clipBefore;
		after = settings.clipAfter;
	}

	for (const Fire &f : fire) {
		const QByteArray name = f.name.toUtf8(), detail = body.toUtf8();
		calldata_t cd = {0};
		calldata_set_string(&cd, "name", name.constData());
		calldata_set_string(&cd, "detail", detail.constData());
		calldata_set_string(&cd, "source", speech ? "lucida-speech" : "lucida");
		calldata_set_float(&cd, "time", wallTs);
		calldata_set_bool(&cd, "clip", f.clip);
		calldata_set_int(&cd, "before", before);
		calldata_set_int(&cd, "after", after);
		const bool handled = proc_handler_call(obs_get_proc_handler(), "spectra_add_event", &cd);
		calldata_free(&cd);
		blog(LOG_INFO, "[Lucida] Event trigger \"%s\" matched%s%s", name.constData(),
		     f.clip ? " (auto clip)" : "", handled ? "" : ", but Spectra isn't there to pin it");
	}
}

} // namespace lucida
