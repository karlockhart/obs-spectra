#include "profiles.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace lucida {

namespace {
constexpr int kVersion = 1;
const char *kLucida = "lucida";

QJsonArray RegionJson(const spectra::Region &r)
{
	return QJsonArray{r.left, r.top, r.right, r.bottom};
}

spectra::Region RegionFrom(const QJsonValue &value, const spectra::Region &fallback)
{
	const QJsonArray a = value.toArray();
	if (a.size() != 4) {
		return fallback;
	}
	return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()};
}
} // namespace

QString ModeKey(RegionMode mode)
{
	switch (mode) {
	case RegionMode::Other:
		return QStringLiteral("other");
	case RegionMode::Ignore:
		return QStringLiteral("ignore");
	default:
		return QStringLiteral("read");
	}
}

RegionMode ModeFromKey(const QString &key)
{
	if (key == QLatin1String("other")) {
		return RegionMode::Other;
	}
	if (key == QLatin1String("ignore")) {
		return RegionMode::Ignore;
	}
	return RegionMode::Read;
}

QJsonObject LucidaProfile::ToJson() const
{
	QJsonArray rules;
	for (const RegionRule &r : regions) {
		rules.append(QJsonObject{{"name", r.name}, {"area", RegionJson(r.area)}, {"mode", ModeKey(r.mode)}});
	}
	return QJsonObject{{"carnivore", carnivore}, {"chat", RegionJson(chatRegion)}, {"hud", RegionJson(hudRegion)},
			   {"readHud", readHud},     {"onlyDrawn", onlyDrawn},         {"regions", rules}};
}

LucidaProfile LucidaProfile::FromJson(const QJsonObject &json)
{
	LucidaProfile p;
	p.carnivore = json.value("carnivore").toBool(p.carnivore);
	p.chatRegion = RegionFrom(json.value("chat"), p.chatRegion);
	p.hudRegion = RegionFrom(json.value("hud"), p.hudRegion);
	p.readHud = json.value("readHud").toBool(p.readHud);
	p.onlyDrawn = json.value("onlyDrawn").toBool(p.onlyDrawn);
	for (const QJsonValue &v : json.value("regions").toArray()) {
		const QJsonObject o = v.toObject();
		const QString name = o.value("name").toString().trimmed();
		if (name.isEmpty()) {
			continue;
		}
		p.regions.push_back({name, RegionFrom(o.value("area"), {}), ModeFromKey(o.value("mode").toString())});
	}
	return p;
}

bool GameProfile::Matches(const QString &exe) const
{
	if (executable.isEmpty() || exe.isEmpty()) {
		return false;
	}
	/* as the frame grabber matches its target process */
	const QRegularExpression re(executable, QRegularExpression::CaseInsensitiveOption);
	return re.isValid() && re.match(exe).hasMatch();
}

std::optional<LucidaProfile> GameProfile::Lucida() const
{
	if (!sections.contains(kLucida)) {
		return std::nullopt;
	}
	return LucidaProfile::FromJson(sections.value(kLucida).toObject());
}

void GameProfile::SetLucida(const LucidaProfile &lucida)
{
	sections.insert(kLucida, lucida.ToJson());
}

bool GameProfiles::Load(const QString &path, QString *error)
{
	profiles.clear();
	QFile f(path);
	if (!f.exists()) {
		return true;
	}
	if (!f.open(QIODevice::ReadOnly)) {
		if (error) {
			*error = f.errorString();
		}
		return false;
	}
	QJsonParseError parse;
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
	if (!doc.isObject()) {
		if (error) {
			*error = parse.errorString();
		}
		return false;
	}
	for (const QJsonValue &v : doc.object().value("profiles").toArray()) {
		QJsonObject o = v.toObject();
		GameProfile p;
		p.name = o.take("name").toString();
		p.executable = o.take("executable").toString();
		p.sections = o;
		profiles.push_back(std::move(p));
	}
	return true;
}

bool GameProfiles::Save(const QString &path, QString *error) const
{
	QJsonArray list;
	for (const GameProfile &p : profiles) {
		QJsonObject o = p.sections;
		o.insert("name", p.name);
		o.insert("executable", p.executable);
		list.append(o);
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (error) {
			*error = f.errorString();
		}
		return false;
	}
	f.write(QJsonDocument(QJsonObject{{"version", kVersion}, {"profiles", list}}).toJson());
	if (!f.commit()) {
		if (error) {
			*error = f.errorString();
		}
		return false;
	}
	return true;
}

const GameProfile *GameProfiles::Match(const QString &exe) const
{
	for (const GameProfile &p : profiles) {
		if (p.Matches(exe)) {
			return &p;
		}
	}
	return nullptr;
}

QString GameProfiles::AnyExecutable() const
{
	QStringList parts;
	for (const GameProfile &p : profiles) {
		if (!p.executable.isEmpty() && QRegularExpression(p.executable).isValid()) {
			parts << QStringLiteral("(?:%1)").arg(p.executable);
		}
	}
	return parts.join('|');
}

} // namespace lucida
