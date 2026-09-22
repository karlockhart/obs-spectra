#pragma once

/* Shared censoring UI and upload for Spectra's chat tools (Lucida's viewer
 * and Obscura's review window). Ports obscura/gui/review.py's view items and
 * obscura/upload.py. */

#include <spectra-vision/chat.hpp>
#include <spectra-vision/image.hpp>

#include <QColor>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsView>
#include <QImage>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

#if defined(_WIN32) && defined(SPECTRA_CENSOR_BUILD)
#define SPECTRA_CENSOR_API __declspec(dllexport)
#elif defined(_WIN32)
#define SPECTRA_CENSOR_API __declspec(dllimport)
#else
#define SPECTRA_CENSOR_API
#endif

#ifdef _MSC_VER
/* members are only used inside the library */
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace spectra::censor {

SPECTRA_CENSOR_API QImage ToQImage(const Image &img);
SPECTRA_CENSOR_API Image FromQImage(const QImage &img);

/* Writes a PNG via <name>.part and a rename, like obscura.redact.write_png */
SPECTRA_CENSOR_API bool WritePng(const QString &path, const Image &img);

/* A chat entry's outline: dashed green when kept, red when censored.
 * Left click toggles (through the callback). */
class SPECTRA_CENSOR_API EntryItem : public QGraphicsPathItem {
public:
	EntryItem(int index, const std::vector<Rect> &rects, const QString &tooltip, std::function<void(int)> onClick);

	int Index() const { return index; }
	void SetCensor(bool censor);
	bool Censor() const { return censor; }
	void SetSelected(bool selected);

protected:
	void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
	void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;
	void mousePressEvent(QGraphicsSceneMouseEvent *event) override;

private:
	int index;
	std::function<void(int)> onClick;
	bool censor = false;
	bool hover = false;
	bool selected = false;
	void Restyle();
};

/* A hand-drawn redaction box (magenta); right click removes it */
class SPECTRA_CENSOR_API ManualItem : public QGraphicsRectItem {
public:
	ManualItem(const Rect &rect, std::function<void(ManualItem *)> onRemove);
	Rect Box() const { return box; }

protected:
	void mousePressEvent(QGraphicsSceneMouseEvent *event) override;

private:
	Rect box;
	std::function<void(ManualItem *)> onRemove;
};

/* Zoomable screenshot view: fit on load/resize, wheel to zoom under the
 * mouse, double click to fit again, rubber band while drawing */
class SPECTRA_CENSOR_API ImageView : public QGraphicsView {
	Q_OBJECT

public:
	explicit ImageView(QWidget *parent = nullptr);

	void SetImage(const QImage &image);
	void SwapPixmap(const QImage &image);
	void ClearImage(const QString &message = QString());
	QGraphicsPixmapItem *PixmapItem() const { return pixmapItem; }
	QSize ImageSize() const { return imageSize; }

	bool drawing = false;

signals:
	/* Drawn rectangle in image pixels (clamped; ignored if under 3 px) */
	void rectDrawn(const spectra::Rect &rect);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	QGraphicsPixmapItem *pixmapItem = nullptr;
	QGraphicsRectItem *band = nullptr;
	QPointF origin;
	QSize imageSize;
	bool fit = true;
	void Refit();
};

/* Obscura's user preferences that the censoring tools share
 * (%APPDATA%\Obscura\config.json; defaults when it is missing) */
struct ObscuraPrefs {
	QString fill = QStringLiteral("auto");
	QStringList seedSensitive = {QStringLiteral("admin chat"), QStringLiteral("admin system"),
				     QStringLiteral("support chat"), QStringLiteral("report")};
	int imgbbExpiration = 0;
	bool imgbbCopyLink = true;
	bool imgbbOpenLink = false;
};
SPECTRA_CENSOR_API QString ObscuraConfigPath();
SPECTRA_CENSOR_API ObscuraPrefs LoadObscuraPrefs();

/* --- imgbb (API key in Windows Credential Manager "Obscura:imgbb") ------- */

SPECTRA_CENSOR_API std::optional<QString> LoadImgbbKey();
SPECTRA_CENSOR_API bool SaveImgbbKey(const QString &key);
SPECTRA_CENSOR_API void ClearImgbbKey();

struct UploadResult {
	bool ok = false;
	QString url;
	QString displayUrl;
	QString deleteUrl;
	QString id;
	QString error;
};

/* POST https://api.imgbb.com/1/upload. Blocking: call off the UI thread.
 * expiration in seconds, 0 = never. */
SPECTRA_CENSOR_API UploadResult UploadToImgbb(const QString &path, int expiration = 0,
					      const QString &apiKey = QString());

} // namespace spectra::censor

#ifdef _MSC_VER
#pragma warning(pop)
#endif
