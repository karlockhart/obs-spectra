#pragma once

/*
 * OBS-Spectra's GitHub releases: version ordering, picking the release an
 * update channel offers and the zip an install should download. Only Qt Core,
 * so spectra-update-tests can test it without the rest of the frontend.
 *
 * Tags look like 32.2.2-spectra.2 (a release) and 32.2.2-spectra.3-rc.6 (a
 * release candidate, published as a GitHub pre-release).
 */

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace SpectraReleases {

struct Version {
	int major = 0;
	int minor = 0;
	int patch = 0;
	int spectra = 0;
	int beta = 0; /* 32.2.1-beta2 */
	int rc = 0;   /* 32.2.1-spectra.1-rc.2 */

	bool valid() const { return major || minor || patch; }
	bool prerelease() const { return beta || rc; }
	QString Base() const;
	/* base version, then spectra.N, then beta < rc < final: a final release
	 * sorts after its own release candidates */
	bool operator<(const Version &other) const;
	bool operator==(const Version &other) const;
};

Version ParseVersion(const QString &version);

/* True when the version is exactly a release tag. Builds between tags
 * ("32.2.2-spectra.2-5-gabc1234") are developer builds, which must not
 * replace themselves with a download. */
bool IsReleaseTag(const QString &version);

enum class Channel { Stable, ReleaseCandidates };

/* The "Spectra" "UpdateChannel" setting: "stable" or "rc". Unset, a release
 * candidate stays on release candidates and anything else on stable. */
Channel ChannelFromSetting(const QString &setting, const QString &currentVersion);
QString ChannelSetting(Channel channel);

struct Asset {
	QString name;
	QString url;
	qint64 size = 0;
};

struct Release {
	QString tag;
	QString htmlUrl;
	QString notes;
	bool prerelease = false;
	bool draft = false;
	Version version;
	QList<Asset> assets;
};

/* The /repos/{repo}/releases list. Empty with *error set when it isn't one. */
QList<Release> ParseReleases(const QByteArray &json, QString *error = nullptr);

/* OBS-Spectra or OBS-Spectra Lite (no Lucida, Obscura or speech). Each updates
 * only from its own zips: OBS-Spectra-<tag>-Windows-x64[-Portable].zip and
 * OBS-Spectra-<tag>-Windows-x64-Lite[-Portable].zip. */
enum class Edition { Full, Lite };

/* The newest release the channel offers that has a Windows zip of the
 * edition: Stable skips pre-releases, ReleaseCandidates takes the newest of
 * all. */
std::optional<Release> SelectRelease(const QList<Release> &releases, Channel channel, Edition edition = Edition::Full);

struct Download {
	Asset zip;
	Asset checksum; /* <zip>.sha256; empty name when the release has none */
	bool portable = false;
};

/* A portable install gets the -Portable.zip, any other the regular zip; each
 * falls back to the other kind (the updater keeps the install's own portable
 * mode either way). */
std::optional<Download> SelectDownload(const Release &release, bool portable, Edition edition = Edition::Full);

/* The hash in a sha256sum line ("<hex>  <name>"), lower case; empty when the
 * text doesn't start with 64 hex digits. */
QString ParseSha256(const QByteArray &text);

} // namespace SpectraReleases
