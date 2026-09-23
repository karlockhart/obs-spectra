#include "tagger.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace lucida {

namespace {

constexpr int kMaxWords = 4;      /* a trigger may span this many words */
constexpr int kTypoMinLength = 6; /* shorter triggers must match exactly */

/* Characters that separate the parts of one word ("body-cam", "body_cam") */
bool IsJoiner(QChar c)
{
	return c.isSpace() || c == '-' || c == '_' || c == '.' || c == '\'' || c == QChar(0x2019);
}

/* Punctuation around a word that is not part of it ("bodycam," "(bodycam)");
 * a leading "/", "!", "#" or "@" starts a command or mention and is kept */
bool IsLeadingPunctuation(QChar c)
{
	return (c.isPunct() || c.isSymbol()) && c != '/' && c != '#' && c != '@' && c != '!';
}

bool IsTrailingPunctuation(QChar c)
{
	return c.isPunct() || c.isSymbol();
}

QString FoldWord(const QString &word)
{
	int begin = 0, end = (int)word.size();
	while (begin < end && IsLeadingPunctuation(word[begin])) {
		begin++;
	}
	while (end > begin && IsTrailingPunctuation(word[end - 1])) {
		end--;
	}
	return Tagger::Fold(word.mid(begin, end - begin));
}

/* Levenshtein distance, stopping early once it exceeds limit */
int EditDistance(const QString &a, const QString &b, int limit)
{
	const int n = (int)a.size(), m = (int)b.size();
	if (std::abs(n - m) > limit) {
		return limit + 1;
	}
	std::vector<int> prev(m + 1), cur(m + 1);
	for (int j = 0; j <= m; j++) {
		prev[j] = j;
	}
	for (int i = 1; i <= n; i++) {
		cur[0] = i;
		int best = cur[0];
		for (int j = 1; j <= m; j++) {
			int cost = a[i - 1] == b[j - 1] ? 0 : 1;
			cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
			best = std::min(best, cur[j]);
		}
		if (best > limit) {
			return limit + 1;
		}
		std::swap(prev, cur);
	}
	return prev[m];
}

} // namespace

QList<TagRule> DefaultTagRules()
{
	return {
		{QStringLiteral("bodycam-rp"), {QStringLiteral("bodycam*"), QStringLiteral("body camera")}},
		{QStringLiteral("admin duty"), {QStringLiteral("/aduty*")}},
	};
}

QString TagRulesToJson(const QList<TagRule> &rules)
{
	QJsonArray array;
	for (const TagRule &r : rules) {
		QJsonObject o;
		o["tag"] = r.tag;
		o["triggers"] = QJsonArray::fromStringList(r.triggers);
		array.append(o);
	}
	return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<TagRule> TagRulesFromJson(const QString &json, bool *ok)
{
	QList<TagRule> rules;
	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
	if (ok) {
		*ok = error.error == QJsonParseError::NoError && doc.isArray();
	}
	for (const QJsonValue &v : doc.array()) {
		QJsonObject o = v.toObject();
		TagRule r;
		r.tag = o["tag"].toString().trimmed();
		for (const QJsonValue &t : o["triggers"].toArray()) {
			QString trigger = t.toString().trimmed();
			if (!trigger.isEmpty()) {
				r.triggers << trigger;
			}
		}
		if (!r.tag.isEmpty() && !r.triggers.isEmpty()) {
			rules << r;
		}
	}
	return rules;
}

QString Tagger::Fold(const QString &text)
{
	QString out;
	out.reserve(text.size());
	for (QChar c : text) {
		if (!IsJoiner(c)) {
			out += c.toLower();
		}
	}
	return out;
}

Tagger::Tagger(const QList<TagRule> &rules, bool tolerateTypos_) : tolerateTypos(tolerateTypos_)
{
	for (const TagRule &rule : rules) {
		Compiled c;
		c.tag = rule.tag.trimmed();
		for (const QString &raw : rule.triggers) {
			Trigger t;
			t.folded = Fold(raw);
			if (t.folded.isEmpty() || t.folded == "*") {
				continue;
			}
			if (t.folded.contains('*') || t.folded.contains('?')) {
				QString pattern;
				for (QChar ch : t.folded) {
					if (ch == '*') {
						pattern += QStringLiteral(".*");
					} else if (ch == '?') {
						pattern += '.';
					} else {
						pattern += QRegularExpression::escape(QString(ch));
					}
				}
				t.wildcard = std::make_unique<QRegularExpression>(
					QRegularExpression::anchoredPattern(pattern),
					QRegularExpression::UseUnicodePropertiesOption);
			}
			c.triggers.push_back(std::move(t));
		}
		if (!c.tag.isEmpty() && !c.triggers.empty()) {
			compiled.push_back(std::move(c));
		}
	}
}

Tagger::~Tagger() = default;

bool Tagger::Matches(const Trigger &t, const QString &candidate) const
{
	if (t.wildcard) {
		return t.wildcard->match(candidate).hasMatch();
	}
	if (candidate == t.folded) {
		return true;
	}
	return tolerateTypos && t.folded.size() >= kTypoMinLength && EditDistance(candidate, t.folded, 1) <= 1;
}

QStringList Tagger::Tags(const QString &text) const
{
	QStringList tags;
	if (compiled.empty()) {
		return tags;
	}
	QStringList words;
	for (const QString &w : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
		QString folded = FoldWord(w);
		if (!folded.isEmpty()) {
			words << folded;
		}
	}
	/* every run of up to kMaxWords consecutive words, joined */
	QStringList candidates;
	for (int i = 0; i < words.size(); i++) {
		QString joined;
		for (int n = 0; n < kMaxWords && i + n < words.size(); n++) {
			joined += words[i + n];
			candidates << joined;
		}
	}
	for (const Compiled &c : compiled) {
		if (tags.contains(c.tag)) {
			continue;
		}
		bool hit = false;
		for (const Trigger &t : c.triggers) {
			for (const QString &candidate : candidates) {
				if (Matches(t, candidate)) {
					hit = true;
					break;
				}
			}
			if (hit) {
				break;
			}
		}
		if (hit) {
			tags << c.tag;
		}
	}
	return tags;
}

} // namespace lucida
