#include "SpectraSplash.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QScreen>

#include "moc_SpectraSplash.cpp"

SpectraSplash *SpectraSplash::current = nullptr;

static constexpr int SPLASH_WIDTH = 640;
static constexpr int SPLASH_HEIGHT = 360;
static constexpr int ICON_SIZE = 96;

static QPixmap RenderSplash(const QString &version)
{
	QScreen *screen = QGuiApplication::primaryScreen();
	qreal dpr = screen ? screen->devicePixelRatio() : 1.0;

	QPixmap pixmap(QSize(SPLASH_WIDTH, SPLASH_HEIGHT) * dpr);
	pixmap.setDevicePixelRatio(dpr);
	pixmap.fill(Qt::transparent);

	QPainter p(&pixmap);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	QRectF bounds(0, 0, SPLASH_WIDTH, SPLASH_HEIGHT);
	QLinearGradient bg(bounds.topLeft(), bounds.bottomRight());
	bg.setColorAt(0.0, QColor(0x1a, 0x1c, 0x22));
	bg.setColorAt(1.0, QColor(0x10, 0x11, 0x15));
	p.setPen(Qt::NoPen);
	p.setBrush(bg);
	p.drawRoundedRect(bounds, 14, 14);

	/* Thin spectrum stripe along the top */
	QLinearGradient stripe(0, 0, SPLASH_WIDTH, 0);
	const QColor spectrum[] = {QColor(0xe6, 0x32, 0x32), QColor(0xff, 0x9f, 0x1a), QColor(0xff, 0xe2, 0x3a),
				   QColor(0x3e, 0xd2, 0x7a), QColor(0x5a, 0xc8, 0xfa), QColor(0x9b, 0x5c, 0xf6)};
	for (int i = 0; i < 6; i++) {
		stripe.setColorAt(i / 5.0, spectrum[i]);
	}
	p.setBrush(stripe);
	p.drawRect(QRectF(24, 0, SPLASH_WIDTH - 48, 4));

	QFont title = p.font();
	title.setPixelSize(40);
	title.setBold(true);
	p.setFont(title);
	p.setPen(QColor(0xf2, 0xf2, 0xf2));
	p.drawText(QRectF(0, 36, SPLASH_WIDTH, 52), Qt::AlignHCenter | Qt::AlignVCenter, "OBS-Spectra");

	struct Tool {
		const char *icon;
		const char *name;
	};
	const Tool tools[] = {{":/res/images/spectra/spectra.svg", "Spectra"},
			      {":/res/images/spectra/obscura.svg", "Obscura"},
			      {":/res/images/spectra/lucida.svg", "Lucida"}};

	QFont label = p.font();
	label.setPixelSize(16);
	label.setBold(true);
	p.setFont(label);

	const int gap = 72;
	const int rowWidth = 3 * ICON_SIZE + 2 * gap;
	int x = (SPLASH_WIDTH - rowWidth) / 2;
	const int iconTop = 116;
	for (const Tool &tool : tools) {
		QIcon icon(tool.icon);
		p.drawPixmap(x, iconTop, icon.pixmap(QSize(ICON_SIZE, ICON_SIZE), dpr));
		p.setPen(QColor(0xd8, 0xd8, 0xd8));
		p.drawText(QRectF(x - 20, iconTop + ICON_SIZE + 10, ICON_SIZE + 40, 24), Qt::AlignHCenter, tool.name);
		x += ICON_SIZE + gap;
	}

	QFont small = p.font();
	small.setPixelSize(12);
	small.setBold(false);
	p.setFont(small);
	p.setPen(QColor(0x8a, 0x8f, 0x99));
	p.drawText(QRectF(0, SPLASH_HEIGHT - 36, SPLASH_WIDTH - 24, 20), Qt::AlignRight | Qt::AlignVCenter, version);

	p.end();
	return pixmap;
}

SpectraSplash::SpectraSplash(const QString &version) : QSplashScreen(RenderSplash(version))
{
	setAttribute(Qt::WA_TranslucentBackground);
	current = this;
}

SpectraSplash::~SpectraSplash()
{
	if (current == this) {
		current = nullptr;
	}
}

void SpectraSplash::Message(const QString &text)
{
	if (current && current->isVisible()) {
		/* showMessage() repaints synchronously */
		current->showMessage("  " + text, Qt::AlignBottom | Qt::AlignLeft, QColor(0xb0, 0xb4, 0xbc));
	}
}
