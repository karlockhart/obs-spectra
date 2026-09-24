#pragma once

#include <spectra-vision/chat.hpp>

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace obscura {

/* Obscura's %APPDATA%\Obscura folder: config.json, training.jsonl,
 * definitions.obx and signing_key.pem, shared with the standalone app */
QString AppDataDir();

/* Obscura's settings (port of obscura/config.py). Stored in
 * %APPDATA%\Obscura\config.json so the standalone app and Spectra agree;
 * keys this version does not know are preserved on save.
 *
 * Hotkeys are Spectra's (OBS Settings > Hotkeys); the hotkey strings are only
 * kept for the standalone app. */
struct Config {
	QString hotkey = QStringLiteral("ctrl+f12");
	QString cropHotkey = QStringLiteral("ctrl+shift+f12");
	QString targetProcess = QStringLiteral("FiveM.*GTAProcess\\.exe");
	QString targetTitle = QStringLiteral("FiveM");

	bool watchEnabled = true;
	QStringList watchFolders;
	bool moveWatchedOriginals = true;

	QString outputDir;
	bool openedToSourceFolder = true;
	bool keepOriginals = true;
	QString originalsDir;
	QString filenamePrefix = QStringLiteral("fivem");
	QString timestampTz = QStringLiteral("local");

	spectra::Region chatRegion = spectra::kDefaultChatRegion;
	spectra::Region hudRegion = spectra::kDefaultHudRegion;
	QString fill = QStringLiteral("auto");

	QStringList seedSensitive = {QStringLiteral("admin chat"), QStringLiteral("admin system"),
				     QStringLiteral("support chat"), QStringLiteral("report")};
	bool autoApply = true;
	int autoMinScreens = 10;
	double autoConfidence = 0.9;
	bool useInstalledDefinitions = true;

	QString distRepo = QStringLiteral("karlockhart/obscura-dist");
	bool autoUpdateApp = true;
	bool autoUpdateDefs = true;
	bool includePrereleaseDefs = true;

	int imgbbExpiration = 0;
	bool imgbbCopyLink = true;
	bool imgbbOpenLink = false;
	QString imgbbAlbum;                          /* album ID or ibb.co album link */
	bool imgbbAutoUpload = false;                /* upload every Obscura screenshot once saved */
	bool imgbbUploadObsShots = false;            /* also OBS's own screenshots, uncensored */
	QString cropReview = QStringLiteral("auto"); /* auto | always | never */

	static QString Path();
	static QString DefinitionsPath();
	static Config Load();
	static Config FromJson(const QJsonObject &o);
	QJsonObject ToJson() const;
	bool Save() const;

private:
	QJsonObject unknown;
};

} // namespace obscura
