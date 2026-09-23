#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <string>

class QWidget;

/*
 * Checks GitHub for a newer OBS-Spectra release and for a newer upstream
 * OBS Studio release than the one Spectra is currently based on.
 *
 * Spectra versions are "<OBS base>-spectra.<n>", e.g. 32.2.1-spectra.3.
 */
class SpectraUpdateCheck : public QObject {
	Q_OBJECT

public:
	struct Version {
		int major = 0;
		int minor = 0;
		int patch = 0;
		int spectra = 0;
		int rc = 0; /* 32.2.1-spectra.1-rc.2: a release candidate, older than the final */

		bool valid() const { return major || minor || patch; }
		QString Base() const;
		bool operator<(const Version &other) const;
	};

	static Version Parse(const QString &version);

	SpectraUpdateCheck(QWidget *parent, bool manual);

	void Start();

private:
	QPointer<QWidget> parentWidget;
	bool manual;
	bool failed = false;

	QString releaseTag;
	QString releaseUrl;
	QString releaseNotes;
	QString upstreamTag;

	void Apply(bool ok, long status, const std::string &body, const std::string &error, bool spectraRelease);
	void Finished();
};
