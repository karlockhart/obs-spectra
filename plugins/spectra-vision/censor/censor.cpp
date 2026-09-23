#include <spectra-censor/censor.hpp>

#include <QBrush>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPen>
#include <QUrl>
#include <QUrlQuery>
#include <QWheelEvent>

#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <winhttp.h>
#endif

namespace spectra::censor {

namespace {
const QColor kCensorPen(255, 70, 70), kCensorFill(230, 40, 40, 110);
const QColor kKeepPen(80, 220, 120), kKeepHover(80, 220, 120, 45);
const QColor kManualPen(255, 60, 220), kManualFill(230, 40, 200, 120);

QPen CosmeticPen(const QColor &colour, qreal width, Qt::PenStyle style = Qt::SolidLine)
{
	QPen pen(colour, width, style);
	pen.setCosmetic(true);
	return pen;
}

QRectF ToRectF(const Rect &r)
{
	return QRectF(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}
} // namespace

QImage ToQImage(const Image &img)
{
	QImage q(img.width, img.height, QImage::Format_RGB888);
	for (int y = 0; y < img.height; y++) {
		const uint8_t *src = img.row(y);
		uchar *dst = q.scanLine(y);
		for (int x = 0; x < img.width; x++) {
			const uint8_t *px = src + x * img.channels;
			const bool grey = img.channels < 3;
			dst[x * 3 + 0] = px[grey ? 0 : 2];
			dst[x * 3 + 1] = px[grey ? 0 : 1];
			dst[x * 3 + 2] = px[0];
		}
	}
	return q;
}

Image FromQImage(const QImage &input)
{
	QImage q = input.convertToFormat(QImage::Format_RGB888);
	Image img(q.width(), q.height(), 3);
	for (int y = 0; y < q.height(); y++) {
		const uchar *src = q.constScanLine(y);
		uint8_t *dst = img.row(y);
		for (int x = 0; x < q.width(); x++) {
			dst[x * 3 + 0] = src[x * 3 + 2];
			dst[x * 3 + 1] = src[x * 3 + 1];
			dst[x * 3 + 2] = src[x * 3 + 0];
		}
	}
	return img;
}

bool WritePng(const QString &path, const Image &img)
{
	const QString part = path + ".part";
	if (!ToQImage(img).save(part, "PNG")) {
		return false;
	}
	QFile::remove(path);
	return QFile::rename(part, path);
}

/* ------------------------------------------------------------------------- */

EntryItem::EntryItem(int index_, const std::vector<Rect> &rects, const QString &tooltip,
		     std::function<void(int)> onClick_)
	: index(index_),
	  onClick(std::move(onClick_))
{
	QPainterPath path;
	for (const Rect &r : rects) {
		path.addRect(ToRectF(r));
	}
	setPath(path.simplified());
	setAcceptHoverEvents(true);
	setToolTip(tooltip);
	setCursor(Qt::PointingHandCursor);
	setZValue(1);
	Restyle();
}

void EntryItem::SetCensor(bool value)
{
	censor = value;
	Restyle();
}

void EntryItem::SetSelected(bool value)
{
	selected = value;
	Restyle();
}

void EntryItem::Restyle()
{
	const qreal width = hover || selected ? 2.5 : 1.2;
	if (censor) {
		setPen(CosmeticPen(kCensorPen, width));
		setBrush(QBrush(kCensorFill));
	} else {
		setPen(CosmeticPen(kKeepPen, width, Qt::DashLine));
		setBrush(hover ? QBrush(kKeepHover) : QBrush(Qt::NoBrush));
	}
}

void EntryItem::hoverEnterEvent(QGraphicsSceneHoverEvent *)
{
	hover = true;
	Restyle();
}

void EntryItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *)
{
	hover = false;
	Restyle();
}

void EntryItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		if (onClick) {
			onClick(index);
		}
		event->accept();
	} else {
		event->ignore();
	}
}

ManualItem::ManualItem(const Rect &rect, std::function<void(ManualItem *)> onRemove_)
	: QGraphicsRectItem(ToRectF(rect)),
	  box(rect),
	  onRemove(std::move(onRemove_))
{
	setPen(CosmeticPen(kManualPen, 1.5));
	setBrush(QBrush(kManualFill));
	setToolTip(QStringLiteral("Manual redaction - right-click to remove"));
	setZValue(2);
}

void ManualItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
	if (event->button() == Qt::RightButton) {
		if (onRemove) {
			onRemove(this);
		}
		event->accept();
	} else {
		event->ignore();
	}
}

/* ------------------------------------------------------------------------- */

ImageView::ImageView(QWidget *parent) : QGraphicsView(parent)
{
	setScene(new QGraphicsScene(this));
	setBackgroundBrush(QColor(24, 24, 28));
	setRenderHints(renderHints() | QPainter::SmoothPixmapTransform);
	setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
}

void ImageView::SetImage(const QImage &image)
{
	scene()->clear();
	band = nullptr;
	pixmapItem = scene()->addPixmap(QPixmap::fromImage(image));
	imageSize = image.size();
	setSceneRect(pixmapItem->boundingRect());
	fit = true;
	Refit();
}

void ImageView::SwapPixmap(const QImage &image)
{
	if (pixmapItem) {
		pixmapItem->setPixmap(QPixmap::fromImage(image));
	}
}

void ImageView::ClearImage(const QString &message)
{
	scene()->clear();
	band = nullptr;
	pixmapItem = nullptr;
	imageSize = QSize();
	if (!message.isEmpty()) {
		QGraphicsTextItem *t = scene()->addText(message);
		t->setDefaultTextColor(QColor(200, 200, 200));
		setSceneRect(t->boundingRect());
	}
}

void ImageView::Refit()
{
	if (fit && pixmapItem) {
		fitInView(sceneRect(), Qt::KeepAspectRatio);
	}
}

void ImageView::resizeEvent(QResizeEvent *event)
{
	QGraphicsView::resizeEvent(event);
	Refit();
}

void ImageView::wheelEvent(QWheelEvent *event)
{
	const double factor = std::pow(1.2, event->angleDelta().y() / 120.0);
	fit = false;
	scale(factor, factor);
}

void ImageView::mouseDoubleClickEvent(QMouseEvent *)
{
	fit = true;
	Refit();
}

void ImageView::mousePressEvent(QMouseEvent *event)
{
	if (drawing && event->button() == Qt::LeftButton && pixmapItem) {
		origin = mapToScene(event->position().toPoint());
		band = scene()->addRect(QRectF(origin, origin), CosmeticPen(QColor(255, 220, 0), 1.5, Qt::DashLine));
		band->setZValue(10);
		return;
	}
	QGraphicsView::mousePressEvent(event);
}

void ImageView::mouseMoveEvent(QMouseEvent *event)
{
	if (band) {
		band->setRect(QRectF(origin, mapToScene(event->position().toPoint())).normalized());
		return;
	}
	QGraphicsView::mouseMoveEvent(event);
}

void ImageView::mouseReleaseEvent(QMouseEvent *event)
{
	if (band) {
		QRectF r = band->rect();
		scene()->removeItem(band);
		delete band;
		band = nullptr;
		/* x0 = max(0, int(left)), x1 = min(w, int(right + 1)) as in Obscura */
		const int w = imageSize.width(), h = imageSize.height();
		Rect rect{std::max(0, (int)r.left()), std::max(0, (int)r.top()), std::min(w, (int)(r.right() + 1)),
			  std::min(h, (int)(r.bottom() + 1))};
		if (rect.x1 - rect.x0 >= 3 && rect.y1 - rect.y0 >= 3) {
			emit rectDrawn(rect);
		}
		return;
	}
	QGraphicsView::mouseReleaseEvent(event);
}

/* ------------------------------------------------------------------------- */

QString ObscuraConfigPath()
{
	QString base = qEnvironmentVariable("APPDATA");
	if (base.isEmpty()) {
		base = QDir::homePath();
	}
	return QDir(base).filePath(QStringLiteral("Obscura/config.json"));
}

ObscuraPrefs LoadObscuraPrefs()
{
	ObscuraPrefs prefs;
	QFile file(ObscuraConfigPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return prefs;
	}
	QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
	if (o.value("fill").isString()) {
		prefs.fill = o.value("fill").toString();
	}
	if (o.value("seed_sensitive").isArray()) {
		prefs.seedSensitive.clear();
		for (const QJsonValue &v : o.value("seed_sensitive").toArray()) {
			prefs.seedSensitive << v.toString();
		}
	}
	prefs.imgbbExpiration = o.value("imgbb_expiration").toInt(prefs.imgbbExpiration);
	prefs.imgbbCopyLink = o.value("imgbb_copy_link").toBool(prefs.imgbbCopyLink);
	prefs.imgbbOpenLink = o.value("imgbb_open_link").toBool(prefs.imgbbOpenLink);
	return prefs;
}

/* ------------------------------------------------------------------------- */
/* imgbb */

#ifdef _WIN32
static const wchar_t *kCredentialTarget = L"Obscura:imgbb";

std::optional<QByteArray> HttpGet(const QString &url, QString *error, int timeoutMs)
{
	auto fail = [&](const QString &msg) -> std::optional<QByteArray> {
		if (error) {
			*error = msg;
		}
		return std::nullopt;
	};
	std::wstring wurl = url.toStdWString();
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256] = {}, path[2048] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
		return fail(QStringLiteral("Not an https URL: %1").arg(url));
	}
	std::wstring object = path;
	if (parts.dwExtraInfoLength) {
		object += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	}

	/* follows redirects (GitHub release assets redirect to a CDN) */
	HINTERNET session = WinHttpOpen(L"OBS-Spectra", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
				    : nullptr;
	QByteArray body;
	DWORD status = 0;
	bool ok = false;
	if (request) {
		WinHttpSetTimeouts(request, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
		const wchar_t *headers = L"Accept: application/vnd.github+json, */*\r\n";
		ok = WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		     WinHttpReceiveResponse(request, nullptr);
		if (ok) {
			DWORD size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			DWORD available = 0;
			while (WinHttpQueryDataAvailable(request, &available) && available) {
				QByteArray chunk(available, Qt::Uninitialized);
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) {
					break;
				}
				body.append(chunk.constData(), read);
				if (body.size() > 64 * 1024 * 1024) {
					break;
				}
			}
		}
	}
	const DWORD lastError = GetLastError();
	if (request) {
		WinHttpCloseHandle(request);
	}
	if (connect) {
		WinHttpCloseHandle(connect);
	}
	if (session) {
		WinHttpCloseHandle(session);
	}
	if (!ok) {
		return fail(QStringLiteral("Could not reach %1 (error %2)")
				    .arg(QString::fromWCharArray(host))
				    .arg(lastError));
	}
	if (status != 200) {
		return fail(QStringLiteral("HTTP %1 from %2").arg(status).arg(QString::fromWCharArray(host)));
	}
	return body;
}

std::optional<QString> LoadImgbbKey()
{
	PCREDENTIALW cred = nullptr;
	if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &cred)) {
		return std::nullopt;
	}
	QString key;
	if (cred->CredentialBlobSize > 0) {
		/* Obscura (pywin32) stores the key as UTF-16LE */
		key = QString::fromUtf16(reinterpret_cast<const char16_t *>(cred->CredentialBlob),
					 cred->CredentialBlobSize / 2);
		while (key.endsWith(QChar('\0'))) {
			key.chop(1);
		}
	}
	CredFree(cred);
	key = key.trimmed();
	return key.isEmpty() ? std::nullopt : std::optional<QString>(key);
}

bool SaveImgbbKey(const QString &input)
{
	QString key = input.trimmed();
	if (key.isEmpty()) {
		ClearImgbbKey();
		return true;
	}
	std::u16string blob = key.toStdU16String();
	std::wstring user = L"imgbb";
	CREDENTIALW cred = {};
	cred.Type = CRED_TYPE_GENERIC;
	cred.TargetName = const_cast<LPWSTR>(kCredentialTarget);
	cred.CredentialBlobSize = (DWORD)(blob.size() * sizeof(char16_t));
	cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
	cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
	cred.UserName = user.data();
	return CredWriteW(&cred, 0) != FALSE;
}

void ClearImgbbKey()
{
	CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0);
}

UploadResult UploadToImgbb(const QString &path, int expiration, const QString &apiKey)
{
	UploadResult result;
	QString key = apiKey.trimmed();
	if (key.isEmpty()) {
		key = LoadImgbbKey().value_or(QString());
	}
	if (key.isEmpty()) {
		result.error = QStringLiteral("No imgbb API key set");
		return result;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		result.error = QStringLiteral("Could not read %1").arg(path);
		return result;
	}

	QUrlQuery form;
	form.addQueryItem("key", key);
	form.addQueryItem("image", QString::fromLatin1(file.readAll().toBase64()));
	form.addQueryItem("name", QFileInfo(path).completeBaseName());
	if (expiration > 0) {
		form.addQueryItem("expiration", QString::number(expiration));
	}
	QByteArray body = form.query(QUrl::FullyEncoded).replace('+', "%2B").toUtf8();

	HINTERNET session = WinHttpOpen(L"OBS-Spectra", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, L"api.imgbb.com", INTERNET_DEFAULT_HTTPS_PORT, 0)
				    : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", L"/1/upload", nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
				    : nullptr;
	QByteArray response;
	DWORD status = 0;
	bool sent = false;
	if (request) {
		WinHttpSetTimeouts(request, 15000, 15000, 60000, 60000);
		const wchar_t *headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
		sent = WinHttpSendRequest(request, headers, (DWORD)-1L, body.data(), (DWORD)body.size(),
					  (DWORD)body.size(), 0) &&
		       WinHttpReceiveResponse(request, nullptr);
		if (sent) {
			DWORD size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			DWORD available = 0;
			while (WinHttpQueryDataAvailable(request, &available) && available) {
				QByteArray chunk(available, Qt::Uninitialized);
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) {
					break;
				}
				response.append(chunk.constData(), read);
			}
		}
	}
	const DWORD lastError = GetLastError();
	if (request) {
		WinHttpCloseHandle(request);
	}
	if (connect) {
		WinHttpCloseHandle(connect);
	}
	if (session) {
		WinHttpCloseHandle(session);
	}

	if (!sent) {
		result.error = QStringLiteral("Could not reach imgbb (error %1)").arg(lastError);
		return result;
	}
	QJsonObject json = QJsonDocument::fromJson(response).object();
	if (status != 200 || !json.value("success").toBool()) {
		QString detail = json.value("error").toObject().value("message").toString();
		result.error = QStringLiteral("imgbb rejected the upload: %1")
				       .arg(detail.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : detail);
		return result;
	}
	QJsonObject d = json.value("data").toObject();
	result.ok = true;
	result.url = d.value("url").toString();
	result.displayUrl = d.value("display_url").toString(result.url);
	result.deleteUrl = d.value("delete_url").toString();
	result.id = d.value("id").toString();
	return result;
}
#else
std::optional<QString> LoadImgbbKey()
{
	return std::nullopt;
}
bool SaveImgbbKey(const QString &)
{
	return false;
}
void ClearImgbbKey() {}
UploadResult UploadToImgbb(const QString &, int, const QString &)
{
	UploadResult r;
	r.error = QStringLiteral("Upload is only available on Windows");
	return r;
}
#endif

} // namespace spectra::censor
