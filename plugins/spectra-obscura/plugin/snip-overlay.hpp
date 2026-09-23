#pragma once

#include <spectra-vision/chat.hpp>

#include <QDialog>
#include <QPixmap>

#include <optional>

namespace obscura {

/* Region selector (port of obscura/gui/snip.py): shows the frozen desktop
 * over every monitor and lets the user drag a rectangle. The image is
 * captured before the overlay appears, so what is dragged over is exactly
 * what gets cropped. */
class SnipOverlay : public QDialog {
	Q_OBJECT

public:
	explicit SnipOverlay(const QImage &desktop);

	/* Rectangle in desktop image pixels, or nullopt when cancelled */
	static std::optional<spectra::Rect> SelectRegion(const QImage &desktop);

protected:
	void paintEvent(QPaintEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	QPixmap pixmap;
	std::optional<QPointF> origin;
	QPointF cursor;
	std::optional<spectra::Rect> result;

	QPointF Scale() const;
	std::optional<QRectF> Selection() const;
	spectra::Rect ToImage(const QRectF &sel) const;
};

} // namespace obscura
