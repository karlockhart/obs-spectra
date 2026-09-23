#pragma once

#include <QDateTime>
#include <QRegularExpression>
#include <QString>

#include <optional>

namespace obscura {

/* Our own output names end in _YYYY-MM-DD_HH-MM-SS[_unix]; the watcher skips them */
const QRegularExpression &OurSuffixRe();
bool IsImageFile(const QString &path);

/* "2026-09-12_11-41-45_1789231305" from the HUD clock, else the capture time */
QString TimestampSuffix(std::optional<long long> unixTs, const QDateTime &fallback, const QString &tz = "local");

/* folder/<stem without our suffix>_<suffix>.png, made unique */
QString OutputPath(const QString &folder, const QString &stem, const QString &suffix);

/* path, or "stem (2).ext", "stem (3).ext", ... if taken */
QString UniquePath(const QString &path);

} // namespace obscura
