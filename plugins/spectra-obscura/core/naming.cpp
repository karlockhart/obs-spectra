#include "naming.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTimeZone>

namespace obscura {

const QRegularExpression &OurSuffixRe()
{
	static const QRegularExpression re(QStringLiteral("_\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}(_\\d{10})?$"));
	return re;
}

bool IsImageFile(const QString &path)
{
	static const QStringList exts = {"png", "jpg", "jpeg", "bmp", "webp"};
	return exts.contains(QFileInfo(path).suffix().toLower());
}

QString TimestampSuffix(std::optional<long long> unixTs, const QDateTime &fallback, const QString &tz)
{
	static const QString fmt = QStringLiteral("yyyy-MM-dd_HH-mm-ss");
	const bool local = tz == QStringLiteral("local");
	if (unixTs) {
		QDateTime dt = QDateTime::fromSecsSinceEpoch(*unixTs, QTimeZone::UTC);
		dt = local ? dt.toLocalTime() : dt;
		return QStringLiteral("%1_%2").arg(dt.toString(fmt)).arg(*unixTs);
	}
	return (local ? fallback.toLocalTime() : fallback.toUTC()).toString(fmt);
}

QString OutputPath(const QString &folder, const QString &stem, const QString &suffix)
{
	QString clean = stem;
	clean.remove(OurSuffixRe());
	return UniquePath(QDir(folder).filePath(QStringLiteral("%1_%2.png").arg(clean, suffix)));
}

QString UniquePath(const QString &path)
{
	QFileInfo info(path);
	QString candidate = path;
	for (int n = 2; QFileInfo::exists(candidate); n++) {
		const QString ext = info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
		candidate =
			info.dir().filePath(QStringLiteral("%1 (%2)%3").arg(info.completeBaseName()).arg(n).arg(ext));
	}
	return candidate;
}

} // namespace obscura
