#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QTimer>
#include <QWidget>

/* Spectra's in-game notifications: small toasts drawn in a frameless,
 * click-through, always-on-top window over the game's monitor.
 *
 * Anti-cheat safety: this is an ordinary window in Spectra's own process.
 * Nothing is injected into or hooked in the game, no game process handle is
 * opened (only GetForegroundWindow/GetWindowRect/MonitorFromWindow to find
 * where to draw), there are no input hooks, and the window is never hidden
 * from screen capture (SetWindowDisplayAffinity is a cheat-overlay tell).
 * It only covers a small corner, never the whole game, and its native window
 * is destroyed as soon as the last toast is gone.
 *
 * Plugins reach it through the global proc handler:
 *   spectra_notify(in string kind, in string title, in string text, in string key)
 * kind is one of the Kind names below ("loop", "clip", "screenshot", ...);
 * a toast with the same non-empty key replaces the previous one. */
class SpectraOverlay : public QWidget {
	Q_OBJECT

public:
	enum class Kind { Info, Loop, LoopStop, Clip, Screenshot, Obscura, Upload, Warning, Error };
	enum class Corner { TopRight, TopLeft, BottomRight, BottomLeft, TopCenter };

	explicit SpectraOverlay(QWidget *parent = nullptr);
	~SpectraOverlay() override;

	static Kind KindFromName(const QString &name);

	/* Events that can be turned off one by one: SpectraOverlay/Show<Name> */
	struct Category {
		const char *configKey;
		const char *labelKey;
	};
	static const QList<Category> &Categories();
	static const char *CategoryKey(Kind kind);

	/* Thread safe */
	void Notify(Kind kind, const QString &title, const QString &text = QString(), const QString &key = QString());

	/* Reads SpectraOverlay/{Enabled,Corner,DurationSec,Show*} from the profile */
	void LoadSettings();
	void SetStyle(bool enabled, Corner corner, int durationSec);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	struct Toast {
		Kind kind;
		QString title;
		QString text;
		QString key;
		qint64 born = 0;     /* ms on clock */
		qint64 deadline = 0; /* ms on clock when it starts leaving */
		qint64 leaving = -1; /* ms on clock it started leaving, -1 while shown */
		float y = -1.0f;     /* animated slot position */
	};

	QList<Toast> toasts;
	QElapsedTimer clock;
	QTimer frame;
	QRect anchor; /* area the toasts are laid out in, global logical coords */

	bool enabled = true;
	QList<QString> hiddenCategories;
	Corner corner = Corner::TopRight;
	int durationMs = 4500;

	void Show(Kind kind, const QString &title, const QString &text, const QString &key);
	void Tick();
	void Relayout();
	void ApplyNativeStyle();
	QRect GameArea() const;
};
