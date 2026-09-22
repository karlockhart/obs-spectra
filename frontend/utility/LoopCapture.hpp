#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

class OBSBasic;

/*
 * Makes sure the program output actually shows the game while loop
 * recording. If the program scene has no capture hooked onto a window of
 * a matching game process, a "Spectra Game Capture" source is added and
 * pointed at the game's main window. If that cannot hook (e.g. blocked by
 * anti-cheat), a "Spectra Window Capture" (Windows Graphics Capture) of the
 * same window is shown instead.
 */
class LoopCapture : public QObject {
	Q_OBJECT

public:
	enum class State {
		Idle,
		WaitingForWindow,
		Hooking,
		GameCapture,
		WindowCapture,
		ExistingCapture,
		NotCapturing,
	};

	explicit LoopCapture(OBSBasic *main);

	/* Checks and repairs the capture; call periodically while recording. */
	void Update(const QStringList &processPatterns);
	void Reset();

	/* Deletes the capture sources Spectra created, so they are recreated
	 * with default settings on the next update. */
	void RemoveManagedSources();

	State GetState() const { return state; }
	QString StatusText() const;

signals:
	void stateChanged(LoopCapture::State state);

private:
	OBSBasic *main;
	State state = State::Idle;
	QString windowString;
	QString windowExe;
	QDateTime hookingSince;
	QDateTime fallbackSince;

	void SetState(State newState);
};
