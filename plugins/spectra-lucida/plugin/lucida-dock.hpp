#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace lucida {

class Controller;

/* "Chat Log" dock: live Lucida log with search */
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(Controller *controller, QWidget *parent = nullptr);

	void Reload();

private:
	QPointer<Controller> controller;
	QLabel *status;
	QLineEdit *search;
	QCheckBox *pause;
	QListWidget *list;
};

} // namespace lucida
