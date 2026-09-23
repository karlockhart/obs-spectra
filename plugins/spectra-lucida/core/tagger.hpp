#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QRegularExpression;

namespace lucida {

/* A tag added to every chat line one of its triggers matches, e.g.
 * "bodycam-rp" for "bodycam" or "admin duty" for "/aduty*" */
struct TagRule {
	QString tag;
	QStringList triggers;

	bool operator==(const TagRule &) const = default;
};

QList<TagRule> DefaultTagRules();
QString TagRulesToJson(const QList<TagRule> &rules);
/* Rules from JSON; nullopt-like empty list for invalid input */
QList<TagRule> TagRulesFromJson(const QString &json, bool *ok = nullptr);

/* Matches triggers against a line's words, ignoring case and word
 * separators: "bodycam" matches "Bodycam", "body cam" and "body-cam", but not
 * "nobody came". A trigger may span up to four words. "*" matches any run of
 * characters and "?" one character ("/aduty*" matches "/aduty" and
 * "/aduty2"). With typo tolerance, a trigger of six or more characters and
 * no wildcards also matches a word one letter off (OCR slips, plurals). */
class Tagger {
public:
	explicit Tagger(const QList<TagRule> &rules = {}, bool tolerateTypos = true);
	~Tagger();
	Tagger(const Tagger &) = delete;
	Tagger &operator=(const Tagger &) = delete;

	bool Empty() const { return compiled.empty(); }
	/* The tags of `text`, in rule order, without duplicates */
	QStringList Tags(const QString &text) const;

	/* Lower case without separators: "Body-Cam" -> "bodycam" */
	static QString Fold(const QString &text);

private:
	struct Trigger {
		QString folded;
		std::unique_ptr<QRegularExpression> wildcard;
	};
	struct Compiled {
		QString tag;
		std::vector<Trigger> triggers;
	};
	std::vector<Compiled> compiled;
	bool tolerateTypos;

	bool Matches(const Trigger &t, const QString &candidate) const;
};

} // namespace lucida
