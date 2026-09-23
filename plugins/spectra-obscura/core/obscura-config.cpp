#include "obscura-config.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace obscura {

namespace {

QString EnvDir(const char *name)
{
	QString base = qEnvironmentVariable(name);
	return base.isEmpty() ? QDir::homePath() : base;
}

QJsonArray RegionJson(const spectra::Region &r)
{
	return QJsonArray{r.left, r.top, r.right, r.bottom};
}

spectra::Region RegionFrom(const QJsonValue &v, const spectra::Region &fallback)
{
	/* Obscura writes regions as [left, top, right, bottom] */
	QJsonArray a = v.toArray();
	if (a.size() != 4) {
		QJsonObject o = v.toObject();
		if (o.contains("left")) {
			return {o["left"].toDouble(), o["top"].toDouble(), o["right"].toDouble(),
				o["bottom"].toDouble()};
		}
		return fallback;
	}
	return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()};
}

QStringList Strings(const QJsonValue &v)
{
	QStringList out;
	for (const QJsonValue &s : v.toArray()) {
		out << s.toString();
	}
	return out;
}

} // namespace

QString AppDataDir()
{
	QString dir = QDir(EnvDir("APPDATA")).filePath(QStringLiteral("Obscura"));
	QDir().mkpath(dir);
	return dir;
}

QString Config::Path()
{
	return QDir(AppDataDir()).filePath(QStringLiteral("config.json"));
}

QString Config::DefinitionsPath()
{
	return QDir(AppDataDir()).filePath(QStringLiteral("definitions.obx"));
}

Config Config::Load()
{
	QFile file(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return FromJson({});
	}
	return FromJson(QJsonDocument::fromJson(file.readAll()).object());
}

Config Config::FromJson(const QJsonObject &o)
{
	Config c;
	c.outputDir = QDir(QDir::homePath()).filePath(QStringLiteral("Pictures/Obscura"));
	c.originalsDir = QDir(EnvDir("LOCALAPPDATA")).filePath(QStringLiteral("Obscura/originals"));

	auto str = [&](const char *k, QString &v) {
		if (o[k].isString()) {
			v = o[k].toString();
		}
	};
	auto boolean = [&](const char *k, bool &v) {
		if (o[k].isBool()) {
			v = o[k].toBool();
		}
	};
	str("hotkey", c.hotkey);
	str("crop_hotkey", c.cropHotkey);
	str("target_process", c.targetProcess);
	str("target_title", c.targetTitle);
	boolean("watch_enabled", c.watchEnabled);
	if (o["watch_folders"].isArray()) {
		c.watchFolders = Strings(o["watch_folders"]);
	}
	boolean("move_watched_originals", c.moveWatchedOriginals);
	str("output_dir", c.outputDir);
	boolean("opened_to_source_folder", c.openedToSourceFolder);
	boolean("keep_originals", c.keepOriginals);
	str("originals_dir", c.originalsDir);
	str("filename_prefix", c.filenamePrefix);
	str("timestamp_tz", c.timestampTz);
	c.chatRegion = RegionFrom(o["chat_region"], c.chatRegion);
	c.hudRegion = RegionFrom(o["hud_region"], c.hudRegion);
	str("fill", c.fill);
	if (o["seed_sensitive"].isArray()) {
		c.seedSensitive = Strings(o["seed_sensitive"]);
	}
	boolean("auto_apply", c.autoApply);
	c.autoMinScreens = o["auto_min_screens"].toInt(c.autoMinScreens);
	c.autoConfidence = o["auto_confidence"].toDouble(c.autoConfidence);
	boolean("use_installed_definitions", c.useInstalledDefinitions);
	str("dist_repo", c.distRepo);
	boolean("auto_update_app", c.autoUpdateApp);
	boolean("auto_update_defs", c.autoUpdateDefs);
	boolean("include_prerelease_defs", c.includePrereleaseDefs);
	c.imgbbExpiration = o["imgbb_expiration"].toInt(c.imgbbExpiration);
	boolean("imgbb_copy_link", c.imgbbCopyLink);
	boolean("imgbb_open_link", c.imgbbOpenLink);
	str("imgbb_album", c.imgbbAlbum);
	boolean("spectra_imgbb_auto_upload", c.imgbbAutoUpload);
	boolean("spectra_imgbb_upload_obs_screenshots", c.imgbbUploadObsShots);
	str("crop_review", c.cropReview);

	c.unknown = o;
	return c;
}

QJsonObject Config::ToJson() const
{
	QJsonObject o = unknown;
	o["hotkey"] = hotkey;
	o["crop_hotkey"] = cropHotkey;
	o["target_process"] = targetProcess;
	o["target_title"] = targetTitle;
	o["watch_enabled"] = watchEnabled;
	o["watch_folders"] = QJsonArray::fromStringList(watchFolders);
	o["move_watched_originals"] = moveWatchedOriginals;
	o["output_dir"] = outputDir;
	o["opened_to_source_folder"] = openedToSourceFolder;
	o["keep_originals"] = keepOriginals;
	o["originals_dir"] = originalsDir;
	o["filename_prefix"] = filenamePrefix;
	o["timestamp_tz"] = timestampTz;
	o["chat_region"] = RegionJson(chatRegion);
	o["hud_region"] = RegionJson(hudRegion);
	o["fill"] = fill;
	o["seed_sensitive"] = QJsonArray::fromStringList(seedSensitive);
	o["auto_apply"] = autoApply;
	o["auto_min_screens"] = autoMinScreens;
	o["auto_confidence"] = autoConfidence;
	o["use_installed_definitions"] = useInstalledDefinitions;
	o["dist_repo"] = distRepo;
	o["auto_update_app"] = autoUpdateApp;
	o["auto_update_defs"] = autoUpdateDefs;
	o["include_prerelease_defs"] = includePrereleaseDefs;
	o["imgbb_expiration"] = imgbbExpiration;
	o["imgbb_copy_link"] = imgbbCopyLink;
	o["imgbb_open_link"] = imgbbOpenLink;
	o["imgbb_album"] = imgbbAlbum;
	o["spectra_imgbb_auto_upload"] = imgbbAutoUpload;
	o["spectra_imgbb_upload_obs_screenshots"] = imgbbUploadObsShots;
	o["crop_review"] = cropReview;
	return o;
}

bool Config::Save() const
{
	QSaveFile file(Path());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(ToJson()).toJson(QJsonDocument::Indented));
	return file.commit();
}

} // namespace obscura
