#include "SpectraReleases.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <climits>
#include <tuple>

namespace SpectraReleases {

static const QString PORTABLE_SUFFIX = QStringLiteral("-Windows-x64-Portable.zip");
static const QString REGULAR_SUFFIX = QStringLiteral("-Windows-x64.zip");

QString Version::Base() const
{
	return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

static std::tuple<int, int, int, int, int> Key(const Version &v)
{
	/* betas, then release candidates, then the final release */
	int stage = INT_MAX;
	if (v.beta) {
		stage = v.beta;
	} else if (v.rc) {
		stage = 1000000 + v.rc;
	}
	return {v.major, v.minor, v.patch, v.spectra, stage};
}

bool Version::operator<(const Version &other) const
{
	return Key(*this) < Key(other);
}

bool Version::operator==(const Version &other) const
{
	return Key(*this) == Key(other);
}

Version ParseVersion(const QString &version)
{
	static const QRegularExpression baseRe(QStringLiteral("^\\s*v?(\\d+)\\.(\\d+)\\.(\\d+)"));
	static const QRegularExpression spectraRe(QStringLiteral("-spectra\\.(\\d+)"));
	static const QRegularExpression betaRe(QStringLiteral("-beta\\.?(\\d+)"));
	static const QRegularExpression rcRe(QStringLiteral("-rc\\.?(\\d+)"));

	Version v;
	QRegularExpressionMatch base = baseRe.match(version);
	if (!base.hasMatch()) {
		return v;
	}
	v.major = base.captured(1).toInt();
	v.minor = base.captured(2).toInt();
	v.patch = base.captured(3).toInt();
	QRegularExpressionMatch m = spectraRe.match(version);
	if (m.hasMatch()) {
		v.spectra = m.captured(1).toInt();
	}
	m = betaRe.match(version);
	if (m.hasMatch()) {
		v.beta = m.captured(1).toInt();
	}
	m = rcRe.match(version);
	if (m.hasMatch()) {
		v.rc = m.captured(1).toInt();
	}
	return v;
}

bool IsReleaseTag(const QString &version)
{
	/* the tags create-release in .github/workflows/push.yaml publishes */
	static const QRegularExpression re(
		QStringLiteral("^\\d+\\.\\d+\\.\\d+(-spectra\\.\\d+)?(-(beta|rc)\\.?\\d+)?$"));
	return re.match(version).hasMatch();
}

Channel ChannelFromSetting(const QString &setting, const QString &currentVersion)
{
	if (setting == QLatin1String("rc")) {
		return Channel::ReleaseCandidates;
	}
	if (setting == QLatin1String("stable")) {
		return Channel::Stable;
	}
	return ParseVersion(currentVersion).prerelease() ? Channel::ReleaseCandidates : Channel::Stable;
}

QString ChannelSetting(Channel channel)
{
	return channel == Channel::ReleaseCandidates ? QStringLiteral("rc") : QStringLiteral("stable");
}

QList<Release> ParseReleases(const QByteArray &json, QString *error)
{
	QList<Release> releases;
	QJsonParseError parseError;
	QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
	if (!doc.isArray()) {
		if (error) {
			*error = parseError.error != QJsonParseError::NoError
					 ? parseError.errorString()
					 : QStringLiteral("not a list of releases");
		}
		return releases;
	}

	for (const QJsonValue &value : doc.array()) {
		QJsonObject obj = value.toObject();
		Release r;
		r.tag = obj["tag_name"].toString();
		r.htmlUrl = obj["html_url"].toString();
		r.notes = obj["body"].toString();
		r.prerelease = obj["prerelease"].toBool();
		r.draft = obj["draft"].toBool();
		r.version = ParseVersion(r.tag);
		for (const QJsonValue &a : obj["assets"].toArray()) {
			QJsonObject asset = a.toObject();
			r.assets.append({asset["name"].toString(), asset["browser_download_url"].toString(),
					 (qint64)asset["size"].toDouble()});
		}
		if (!r.tag.isEmpty()) {
			releases.append(r);
		}
	}
	return releases;
}

static const Asset *FindZip(const Release &release, bool portable)
{
	const QString &suffix = portable ? PORTABLE_SUFFIX : REGULAR_SUFFIX;
	for (const Asset &asset : release.assets) {
		if (asset.name.endsWith(suffix, Qt::CaseInsensitive) && !asset.url.isEmpty()) {
			return &asset;
		}
	}
	return nullptr;
}

std::optional<Release> SelectRelease(const QList<Release> &releases, Channel channel)
{
	std::optional<Release> best;
	for (const Release &r : releases) {
		if (r.draft || !r.version.valid()) {
			continue;
		}
		if (channel == Channel::Stable && (r.prerelease || r.version.prerelease())) {
			continue;
		}
		if (!FindZip(r, true) && !FindZip(r, false)) {
			continue;
		}
		if (!best || best->version < r.version) {
			best = r;
		}
	}
	return best;
}

std::optional<Download> SelectDownload(const Release &release, bool portable)
{
	const Asset *zip = FindZip(release, portable);
	bool gotPortable = portable;
	if (!zip) {
		zip = FindZip(release, !portable);
		gotPortable = !portable;
	}
	if (!zip) {
		return std::nullopt;
	}

	Download d;
	d.zip = *zip;
	d.portable = gotPortable;
	const QString checksumName = zip->name + QStringLiteral(".sha256");
	for (const Asset &asset : release.assets) {
		if (asset.name.compare(checksumName, Qt::CaseInsensitive) == 0) {
			d.checksum = asset;
			break;
		}
	}
	return d;
}

QString ParseSha256(const QByteArray &text)
{
	QByteArray hash = text.trimmed();
	/* sha256sum writes "<hash>  <name>"; certutil and others write only the hash */
	qsizetype end = 0;
	while (end < hash.size() && !QChar::isSpace((uchar)hash[end]) && hash[end] != '*') {
		end++;
	}
	hash = hash.left(end).toLower();
	static const QRegularExpression hexRe(QStringLiteral("^[0-9a-f]{64}$"));
	QString result = QString::fromLatin1(hash);
	return hexRe.match(result).hasMatch() ? result : QString();
}

} // namespace SpectraReleases
