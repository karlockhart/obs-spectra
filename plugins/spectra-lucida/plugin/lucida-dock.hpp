#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace lucida {

class Controller;
class Viewer;

/* "Chat Log" dock: live Lucida log with search */
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(Controller *controller, QWidget *parent = nullptr);

	void Reload();
	/* Opens the viewer, optionally at a line */
	void OpenViewer(long long lineId = 0);
	void OpenSettings();

private:
	QPointer<Controller> controller;
	QLabel *status;
	QLabel *speechStatus;
	QLineEdit *search;
	QCheckBox *pause;
	QListWidget *list;
	QPointer<Viewer> viewer;
};

} // namespace lucida
