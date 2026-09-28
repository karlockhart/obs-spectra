#pragma once

#include "SpectraReleases.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <optional>
#include <string>

class QWidget;

/*
 * Checks GitHub for a newer OBS-Spectra release on the chosen update channel
 * (Settings > General > Updates) and for a newer upstream OBS Studio release
 * than the one Spectra is currently based on. A newer release can be
 * installed from the notification (SpectraUpdateDialog).
 *
 * Spectra versions are "<OBS base>-spectra.<n>", e.g. 32.2.1-spectra.3, and
 * release candidates "<OBS base>-spectra.<n>-rc.<m>".
 */
class SpectraUpdateCheck : public QObject {
	Q_OBJECT

public:
	SpectraUpdateCheck(QWidget *parent, bool manual);

	/* The update channel from the settings (see SpectraReleases::ChannelFromSetting) */
	static SpectraReleases::Channel CurrentChannel();

	void Start();

private:
	QPointer<QWidget> parentWidget;
	bool manual;
	bool failed = false;

	SpectraReleases::Channel channel;
	std::optional<SpectraReleases::Release> release;
	QString upstreamTag;

	void ApplySpectra(bool ok, long status, const std::string &body, const std::string &error);
	void ApplyUpstream(bool ok, long status, const std::string &body, const std::string &error);
	void Finished();
};
