/*
 * lucida-replay: feeds screenshots through Lucida's recorder (real OCR) into
 * a fresh log, then checks every kept screenshot: each line listed on it is
 * read again from its box on that image and compared with the line's text.
 *
 *   lucida-replay [--chat L,T,R,B] [--hud L,T,R,B] [--annotate DIR] <out dir> image...
 *
 * Images are replayed in file-name order; a name that is a unix time (as
 * Lucida names its screenshots) is used as the wall clock.
 */

#include "../core/recorder.hpp"
#include "../core/store.hpp"

#include <spectra-vision/image.hpp>
#include <spectra-vision/ocr.hpp>
#include <spectra-vision/text.hpp>

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cstdio>

using namespace lucida;

static spectra::Image Load(const QString &path)
{
	QImage q(path);
	if (q.isNull()) {
		return {};
	}
	q = q.convertToFormat(QImage::Format_RGB888);
	spectra::Image img(q.width(), q.height(), 3);
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

static bool ParseRegion(const QString &text, spectra::Region &r)
{
	const QStringList v = text.split(',');
	if (v.size() != 4) {
		return false;
	}
	r = {v[0].toDouble(), v[1].toDouble(), v[2].toDouble(), v[3].toDouble()};
	return true;
}

/* How many of the line's words the box's text has (0..1) */
static double WordsFound(const QString &line, const QString &boxText)
{
	const QStringList words = Normalise(line).split(' ', Qt::SkipEmptyParts);
	const QStringList found = Normalise(boxText).split(' ', Qt::SkipEmptyParts);
	if (words.isEmpty()) {
		return 1.0;
	}
	int hits = 0;
	for (const QString &w : words) {
		for (const QString &f : found) {
			if (w == f || (w.size() >= 4 && spectra::SequenceRatio(w, f) >= 0.8)) {
				hits++;
				break;
			}
		}
	}
	return (double)hits / words.size();
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	RecorderConfig cfg;
	QString annotate;
	QStringList positional;
	const QStringList args = app.arguments();
	for (int i = 1; i < args.size(); i++) {
		if (args[i] == "--chat" && i + 1 < args.size()) {
			ParseRegion(args[++i], cfg.chatRegion);
		} else if (args[i] == "--hud" && i + 1 < args.size()) {
			ParseRegion(args[++i], cfg.hudRegion);
		} else if (args[i] == "--annotate" && i + 1 < args.size()) {
			annotate = args[++i];
		} else {
			positional << args[i];
		}
	}
	if (positional.size() < 2) {
		fprintf(stderr,
			"usage: lucida-replay [--chat L,T,R,B] [--hud L,T,R,B] [--annotate DIR] <out dir> image...\n");
		return 2;
	}
	const QString out = positional.takeFirst();
	QStringList files = positional;
	std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
		return QFileInfo(a).completeBaseName() < QFileInfo(b).completeBaseName();
	});

	std::string error;
	auto ocr = spectra::OcrEngine::Create({}, error);
	if (!ocr) {
		fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}

	QDir(out).removeRecursively();
	QDir().mkpath(out);
	Store store(QDir(out).filePath("chatlog.db"));
	if (!store.Open(nullptr)) {
		fprintf(stderr, "error: cannot open the log in %s\n", qPrintable(out));
		return 1;
	}
	cfg.keepFrames = true;
	cfg.framesDir = QDir(out).filePath("frames");

	int next = 0;
	long long wall = 0;
	Recorder recorder(cfg, store, *ocr, [&]() -> std::optional<GrabbedFrame> {
		if (next >= files.size()) {
			return std::nullopt;
		}
		const QString file = files[next++];
		bool ok = false;
		const long long ts = QFileInfo(file).completeBaseName().left(10).toLongLong(&ok);
		wall = ok ? ts : wall + 5;
		return GrabbedFrame{Load(file), QFileInfo(file).fileName()};
	});
	recorder.wallClock = [&]() {
		return wall;
	};
	while (next < files.size()) {
		Tick t = recorder.Step();
		printf("%s: %d entries, %d new\n", qPrintable(QFileInfo(files[next - 1]).fileName()), t.entries,
		       t.added);
	}
	recorder.Close();

	if (!annotate.isEmpty()) {
		QDir().mkpath(annotate);
	}
	int frames = 0, checked = 0, bad = 0, missingRect = 0;
	auto kept = store.Frames(100000);
	std::reverse(kept.begin(), kept.end());
	for (const FrameInfo &info : kept) {
		frames++;
		QImage shot(info.frame.path);
		spectra::Image img = Load(info.frame.path);
		QPainter p;
		if (!annotate.isEmpty()) {
			shot = shot.convertToFormat(QImage::Format_RGB32);
			p.begin(&shot);
			QFont f = p.font();
			f.setPixelSize(14);
			p.setFont(f);
		}
		printf("\n%s: %zu lines\n", qPrintable(QFileInfo(info.frame.path).fileName()),
		       store.FrameLines(info.frame.id).size());
		for (const LogLine &l : store.FrameLines(info.frame.id)) {
			if (!l.rect) {
				missingRect++;
				printf("  NO BOX  %s\n", qPrintable(l.body));
				continue;
			}
			const spectra::Rect &r = *l.rect;
			spectra::Image crop = spectra::Crop(img, std::max(0, r.x0 - 4), std::max(0, r.y0 - 2),
							    std::min(img.width, r.x1 + 4),
							    std::min(img.height, r.y1 + 2));
			QString text;
			for (const spectra::OcrBox &b : ocr->Read(crop, 0.3f)) {
				text += QString::fromStdString(b.text) + ' ';
			}
			const double found = WordsFound(l.body, text);
			const bool ok = found >= 0.6;
			checked++;
			bad += ok ? 0 : 1;
			printf("  %s %3.0f%%  [%d,%d-%d,%d]  %s\n", ok ? "ok " : "BAD", found * 100, r.x0, r.y0, r.x1,
			       r.y1, qPrintable(l.body.left(70)));
			if (!ok) {
				printf("           box reads: %s\n", qPrintable(text.trimmed().left(90)));
			}
			if (p.isActive()) {
				p.setPen(QPen(ok ? QColor(40, 220, 90) : QColor(240, 40, 40), 2));
				p.drawRect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
				p.drawText(r.x1 + 6, r.y1 - 4, QStringLiteral("#%1").arg(l.id));
			}
		}
		if (p.isActive()) {
			p.end();
			shot.save(QDir(annotate).filePath(QFileInfo(info.frame.path).completeBaseName() + ".png"));
		}
	}
	printf("\n%d screenshots, %d boxes checked, %d wrong, %d lines without a box\n", frames, checked, bad,
	       missingRect);
	return bad || missingRect ? 1 : 0;
}
