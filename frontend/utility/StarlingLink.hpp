#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class OBSBasic;

/*
 * Starling (local voice conversion) integration. While a Starling session
 * is converting, the microphone is muted and a "Starling Voice" audio
 * capture of the virtual cable Starling plays into is added to the mix, so
 * loop recordings carry the converted voice instead of the raw one. When
 * the session stops, the capture is removed and the microphone unmuted
 * again (only if Spectra muted it).
 *
 * Starling holds the named mutex Local\StarlingSessionActive for as long
 * as a session runs (Windows removes it if Starling crashes) and writes
 * the output device it opened to %LOCALAPPDATA%\Starling\session.json.
 */
class StarlingLink : public QObject {
	Q_OBJECT

public:
	explicit StarlingLink(OBSBasic *main);

	/* Loop setting "StarlingVoice"; disabling releases immediately. */
	void SetEnabled(bool enabled);
	bool Engaged() const { return engaged; }

signals:
	void engagedChanged(bool engaged);

private:
	QTimer timer;
	bool enabled = false;
	bool engaged = false;
	/* Microphone source already handled this session, so a mic the user
	 * unmutes by hand stays unmuted */
	QString handledMicUuid;
	/* Last reason not to engage, so it is logged once rather than every
	 * poll */
	QString lastSkipReason;

	void Poll();
	void Engage(const QString &deviceId, const QString &deviceName);
	void Release();
	void Skip(const QString &reason);
};
