#include "lucida-dock.hpp"
#include "lucida-controller.hpp"
#include "lucida-settings.hpp"
#include "lucida-viewer.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

namespace lucida {

namespace {

/* Spectra's Clip Maker (the frontend's spectra_edit_moment proc) */
bool EditClip(const VideoSpot &spot, double before, double after)
{
	const QByteArray path = spot.path.toUtf8();
	calldata_t cd = {0};
	calldata_set_string(&cd, "path", path.constData());
	calldata_set_float(&cd, "offset", spot.offset);
	calldata_set_float(&cd, "before", before);
	calldata_set_float(&cd, "after", after);
	const bool called = proc_handler_call(obs_get_proc_handler(), "spectra_edit_moment", &cd);
	calldata_free(&cd);
	return called;
}

} // namespace

namespace {
constexpr int kShownLines = 300;

QString LineLabel(const LogLine &l)
{
	QString time = QDateTime::fromSecsSinceEpoch(l.sortTs).toString(QStringLiteral("HH:mm:ss"));
	QString text = QStringLiteral("%1  [%2] %3  %4")
			       .arg(time, l.clock.value_or(QStringLiteral("--:--:--")),
				    l.channel.leftJustified(18, ' '), l.body);
	for (const QString &tag : l.labels) {
		text += QStringLiteral("  #") + tag;
	}
	return text;
}
} // namespace

Dock::Dock(Controller *controller_, QWidget *parent) : QWidget(parent), controller(controller_)
{
	status = new QLabel(controller->Status());
	status->setWordWrap(true);

	search = new QLineEdit();
	search->setPlaceholderText(obs_module_text("Lucida.Dock.Search"));
	search->setClearButtonEnabled(true);

	QPushButton *sample = new QPushButton(obs_module_text("Lucida.Dock.SampleNow"));
	QPushButton *open = new QPushButton(obs_module_text("Lucida.Dock.OpenViewer"));
	open->setToolTip(obs_module_text("Lucida.Dock.OpenViewer.Tip"));
	QPushButton *settings = new QPushButton(obs_module_text("Lucida.Dock.Settings"));
	pause = new QCheckBox(obs_module_text("Lucida.Dock.Pause"));
	pause->setChecked(controller->Paused());

	list = new QListWidget();
	list->setAlternatingRowColors(true);
	list->setUniformItemSizes(true);
	list->setWordWrap(false);
	QFont mono = list->font();
	mono.setStyleHint(QFont::Monospace);
	mono.setFamily(QStringLiteral("Consolas"));
	list->setFont(mono);

	QHBoxLayout *row = new QHBoxLayout();
	row->addWidget(search, 1);
	row->addWidget(sample);
	row->addWidget(open);
	row->addWidget(settings);
	row->addWidget(pause);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->addWidget(status);
	layout->addLayout(row);
	layout->addWidget(list, 1);

	connect(search, &QLineEdit::returnPressed, this, &Dock::Reload);
	connect(search, &QLineEdit::textChanged, this, [this](const QString &t) {
		if (t.isEmpty()) {
			Reload();
		}
	});
	connect(sample, &QPushButton::clicked, controller, &Controller::SampleNow);
	connect(open, &QPushButton::clicked, this, [this] { OpenViewer(); });
	connect(settings, &QPushButton::clicked, this, &Dock::OpenSettings);
	connect(controller, &Controller::relabelled, this, &Dock::Reload);
	connect(list, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { OpenViewer(item->data(Qt::UserRole).toLongLong()); });
	connect(pause, &QCheckBox::toggled, controller, &Controller::SetPaused);
	connect(controller, &Controller::statusChanged, status, &QLabel::setText);
	connect(controller, &Controller::ticked, this, [this](int added, double, bool) {
		if (added > 0 && search->text().isEmpty()) {
			Reload();
		}
	});

	Reload();
}

void Dock::OpenViewer(long long lineId)
{
	if (!viewer) {
		QPointer<Controller> c = controller;
		ViewerSource source{[c]() { return c ? c->Reader() : nullptr; },
				    [c](const LogLine &line) {
					    return c ? c->VideoFor(line) : std::optional<VideoSpot>();
				    },
				    EditClip};
		viewer = new Viewer(std::move(source), window());
		viewer->setWindowFlag(Qt::Window);
	}
	viewer->show();
	viewer->raise();
	viewer->activateWindow();
	if (lineId) {
		viewer->ShowLine(lineId);
	}
}

void Dock::OpenSettings()
{
	if (!controller) {
		return;
	}
	SettingsDialog dialog(controller, window());
	if (dialog.exec() == QDialog::Accepted) {
		Reload();
	}
}

void Dock::Reload()
{
	if (!controller || !controller->Reader()) {
		list->clear();
		return;
	}
	QScrollBar *bar = list->verticalScrollBar();
	const bool atBottom = bar->value() >= bar->maximum() - 2;
	const int keep = bar->value();

	Store *store = controller->Reader();
	const QString query = search->text().trimmed();
	std::vector<LogLine> lines = query.isEmpty() ? store->Recent(kShownLines) : store->Search(query, kShownLines);

	list->setUpdatesEnabled(false);
	list->clear();
	for (const LogLine &l : lines) {
		QListWidgetItem *item = new QListWidgetItem(LineLabel(l));
		item->setData(Qt::UserRole, l.id);
		item->setToolTip(QStringLiteral("%1\n%2").arg(l.When(), l.body));
		list->addItem(item);
	}
	list->setUpdatesEnabled(true);

	if (atBottom || !query.isEmpty()) {
		list->scrollToBottom();
	} else {
		bar->setValue(keep);
	}
}

} // namespace lucida
