#pragma once

#include <QComboBox>
#include <QWidget>

class QCheckBox;
class QListWidget;
class QPushButton;

/* Editable combo box that lists the applications with an open window each
 * time its dropdown is opened. */
class SpectraRunningAppsCombo : public QComboBox {
	Q_OBJECT

public:
	explicit SpectraRunningAppsCombo(QWidget *parent = nullptr);

	void showPopup() override;

private:
	void Refresh();
};

/*
 * Picks the applications loop recording watches for: a list of process
 * name patterns (* and ? are wildcards) that can be typed in or chosen from
 * the running applications, plus "any fullscreen application".
 */
class SpectraAppPicker : public QWidget {
	Q_OBJECT

public:
	/* Without `fullscreenOption` the "any fullscreen application" checkbox
	 * is hidden */
	explicit SpectraAppPicker(bool fullscreenOption = true, QWidget *parent = nullptr);

	void SetEntryHint(const QString &placeholder, const QString &tip);

	/* Comma-separated patterns as stored in the "Processes" setting */
	void SetPatterns(const QString &patterns);
	QString Patterns() const;

	void SetAnyFullscreen(bool enabled);
	bool AnyFullscreen() const;

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	QListWidget *list;
	SpectraRunningAppsCombo *entry;
	QPushButton *add;
	QPushButton *remove;
	QCheckBox *anyFullscreen;

	void AddEntry();
	void RemoveSelected();
	void AddPattern(const QString &pattern);
};
