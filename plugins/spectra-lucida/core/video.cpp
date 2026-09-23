#include "video.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <cmath>
#include <limits>

namespace lucida {

namespace {
constexpr double kSlack = 3.0; /* s: file times and keyframe splits are not exact */
}

double SegmentStart(const QString &fileName)
{
	QDateTime t = QDateTime::fromString(QFileInfo(fileName).completeBaseName().left(19),
					    QStringLiteral("yyyy-MM-dd HH-mm-ss"));
	return t.isValid() ? t.toMSecsSinceEpoch() / 1000.0 : std::numeric_limits<double>::quiet_NaN();
}

std::optional<VideoSpot> LocateVideo(const QString &loopDir, double wallTs, bool recording)
{
	if (loopDir.isEmpty()) {
		return std::nullopt;
	}
	/* names are timestamps, so name order is time order */
	const QFileInfoList files = QDir(loopDir).entryInfoList({QStringLiteral("*.mkv")}, QDir::Files, QDir::Name);
	for (int i = (int)files.size() - 1; i >= 0; i--) {
		const double start = SegmentStart(files[i].fileName());
		if (std::isnan(start) || start > wallTs) {
			continue;
		}
		double end;
		if (i + 1 < files.size() && !std::isnan(SegmentStart(files[i + 1].fileName()))) {
			end = SegmentStart(files[i + 1].fileName());
		} else if (recording) {
			end = std::numeric_limits<double>::infinity();
		} else {
			end = files[i].lastModified().toMSecsSinceEpoch() / 1000.0;
		}
		if (wallTs > end + kSlack) {
			return std::nullopt;
		}
		return VideoSpot{QDir::cleanPath(files[i].absoluteFilePath()), std::max(0.0, wallTs - start)};
	}
	return std::nullopt;
}

QString FormatOffset(double seconds)
{
	const long long s = (long long)std::floor(std::max(0.0, seconds));
	if (s >= 3600) {
		return QStringLiteral("%1:%2:%3")
			.arg(s / 3600)
			.arg((s / 60) % 60, 2, 10, QChar('0'))
			.arg(s % 60, 2, 10, QChar('0'));
	}
	return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

} // namespace lucida
