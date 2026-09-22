#pragma once

#include <QSplashScreen>

/* Startup splash showing the Spectra, Obscura and Lucida icons. */
class SpectraSplash : public QSplashScreen {
	Q_OBJECT

public:
	explicit SpectraSplash(const QString &version);
	~SpectraSplash();

	/* Updates the status line of the splash, if one is showing. */
	static void Message(const QString &text);

private:
	static SpectraSplash *current;
};
