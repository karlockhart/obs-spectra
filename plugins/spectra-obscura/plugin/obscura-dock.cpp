#include "obscura-dock.hpp"
#include "obscura-controller.hpp"

#include <obs-module.h>

#include <spectra-censor/censor.hpp>

#include <QCheckBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace obscura {

namespace {
QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}
} // namespace

Dock::Dock(Controller *controller_, QWidget *parent) : QWidget(parent), controller(controller_)
{
	status = new QLabel(controller->Status());
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);

	auto button = [this](const char *key, const char *tip, auto slot) {
		QPushButton *b = new QPushButton(T(key));
		if (tip) {
			b->setToolTip(T(tip));
		}
		connect(b, &QPushButton::clicked, this, slot);
		return b;
	};
	QPushButton *capture =
		button("Obscura.Dock.Capture", "Obscura.Dock.Capture.Tip", [this] { controller->Capture(); });
	QPushButton *region =
		button("Obscura.Dock.Region", "Obscura.Dock.Region.Tip", [this] { controller->CaptureRegion(); });
	QPushButton *open = button("Obscura.Dock.Open", nullptr, [this] { controller->OpenScreenshots(this); });
	review = button("Obscura.Dock.Review", nullptr, [this] { controller->ShowReview(); });
	uploadLast = button("Obscura.Dock.UploadLast", nullptr, [this] { controller->UploadLast(); });
	QPushButton *folder = button("Obscura.Dock.OutputFolder", nullptr,
				     [this] { controller->OpenFolder(controller->Cfg().outputDir); });
	QPushButton *settings = button("Obscura.Dock.Settings", nullptr, [this] { controller->OpenSettings(this); });
	pause = new QCheckBox(T("Obscura.Dock.Pause"));
	connect(pause, &QCheckBox::toggled, controller, &Controller::SetPaused);

	QGridLayout *grid = new QGridLayout();
	grid->addWidget(capture, 0, 0);
	grid->addWidget(region, 0, 1);
	grid->addWidget(open, 1, 0);
	grid->addWidget(review, 1, 1);
	grid->addWidget(uploadLast, 2, 0);
	grid->addWidget(folder, 2, 1);
	grid->addWidget(settings, 3, 0);
	grid->addWidget(pause, 3, 1);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->addWidget(status);
	layout->addLayout(grid);
	layout->addStretch(1);

	connect(controller, &Controller::statusChanged, status, &QLabel::setText);
	connect(controller, &Controller::queueChanged, this, &Dock::Refresh);
	connect(controller, &Controller::lastSavedChanged, this, &Dock::Refresh);
	Refresh();
}

void Dock::Refresh()
{
	if (!controller) {
		return;
	}
	const int queued = controller->Queued();
	review->setText(queued ? T("Obscura.Dock.ReviewCount").arg(queued) : T("Obscura.Dock.Review"));
	const QString last = controller->LastSaved();
	uploadLast->setEnabled(!last.isEmpty());
	uploadLast->setToolTip(last.isEmpty() ? QString()
					      : T("Obscura.Dock.UploadLast.Tip").arg(QFileInfo(last).fileName()));
}

} // namespace obscura
