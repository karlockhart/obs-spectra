#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

namespace obscura {

class Controller;

/* "Obscura" dock: capture buttons, the review queue and the last result */
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(Controller *controller, QWidget *parent = nullptr);

private:
	QPointer<Controller> controller;
	QLabel *status;
	QPushButton *review;
	QPushButton *uploadLast;
	QCheckBox *pause;

	void Refresh();
};

} // namespace obscura
