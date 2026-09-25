#pragma once

#include <spectra-vision/chat.hpp>

#include <QJsonObject>
#include <QString>

#include <optional>
#include <vector>

namespace lucida {

/* What carnivore mode does with text in a drawn region */
enum class RegionMode {
	Read,   /* log it under the region's name */
	Other,  /* log it under "other" */
	Ignore, /* do not log it */
};

/* A region drawn in the region editor */
struct RegionRule {
	QString name;
	spectra::Region area; /* fractions of the frame */
	RegionMode mode = RegionMode::Read;

	bool operator==(const RegionRule &o) const
	{
		return name == o.name && mode == o.mode && area.left == o.area.left && area.top == o.area.top &&
		       area.right == o.area.right && area.bottom == o.area.bottom;
	}
};

QString ModeKey(RegionMode mode);
RegionMode ModeFromKey(const QString &key);

/* Lucida's part of a game profile: how to read that game's screen */
struct LucidaProfile {
	bool carnivore = true;
	spectra::Region chatRegion = spectra::kDefaultChatRegion;
	spectra::Region hudRegion = spectra::kDefaultHudRegion;
	bool readHud = true;
	/* Carnivore mode: text outside the drawn regions is "other" instead of
	 * becoming new regions */
	bool onlyDrawn = false;
	std::vector<RegionRule> regions;

	QJsonObject ToJson() const;
	static LucidaProfile FromJson(const QJsonObject &json);
};

/* Settings for one game, chosen by the executable its capture is hooked
 * onto. Each Spectra feature keeps its own section ("lucida" for Lucida);
 * sections a feature does not know are kept as they are, so the same file
 * can grow into per-game settings for the rest of Spectra. */
struct GameProfile {
	QString name;
	QString executable; /* regular expression, case-insensitive, matched anywhere in the file name */
	QJsonObject sections;

	bool Matches(const QString &exe) const;
	std::optional<LucidaProfile> Lucida() const;
	void SetLucida(const LucidaProfile &lucida);
};

/* The per-game profiles file (JSON):
 *   {"version": 1, "profiles": [{"name": "FiveM", "executable": "FiveM.*GTAProcess\\.exe",
 *                                "lucida": {...}}]} */
class GameProfiles {
public:
	std::vector<GameProfile> profiles;

	/* A missing file is an empty list */
	bool Load(const QString &path, QString *error = nullptr);
	bool Save(const QString &path, QString *error = nullptr) const;

	/* The first profile for exe, in list order */
	const GameProfile *Match(const QString &exe) const;
	/* A pattern matching every profile's executable (empty if none) */
	QString AnyExecutable() const;
};

} // namespace lucida
