/*
 * spectra-ocr: runs spectra-vision OCR on image files and prints JSON lines,
 * one per image: {"file", "ms", "boxes": [{"text", "score", "box": [x0, y0,
 * x1, y1]}]}. Used to compare against RapidOCR.
 *
 *   spectra-ocr [--models DIR] [--threads N] [--score S] [--line] image...
 */

#include <spectra-vision/ocr.hpp>

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <cstdio>

using namespace spectra;

static Image Load(const QString &path)
{
	QImage q(path);
	if (q.isNull()) {
		return {};
	}
	q = q.convertToFormat(QImage::Format_RGB888);
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

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	OcrEngine::Options options;
	float score = 0.0f;
	bool lineOnly = false;
	QStringList files;

	QStringList args = app.arguments();
	for (int i = 1; i < args.size(); i++) {
		if (args[i] == "--models" && i + 1 < args.size()) {
			options.modelDir = args[++i].toStdString();
		} else if (args[i] == "--threads" && i + 1 < args.size()) {
			options.threads = args[++i].toInt();
		} else if (args[i] == "--score" && i + 1 < args.size()) {
			score = args[++i].toFloat();
		} else if (args[i] == "--line") {
			lineOnly = true;
		} else {
			files << args[i];
		}
	}

	std::string error;
	auto engine = OcrEngine::Create(options, error);
	if (!engine) {
		fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}

	for (const QString &file : files) {
		Image img = Load(file);
		QJsonObject result;
		result["file"] = file;
		if (img.empty()) {
			result["error"] = "could not load image";
		} else if (lineOnly) {
			auto t0 = std::chrono::steady_clock::now();
			std::string text = engine->RecognizeLine(img);
			auto t1 = std::chrono::steady_clock::now();
			result["ms"] = std::chrono::duration<double, std::milli>(t1 - t0).count();
			result["text"] = QString::fromStdString(text);
		} else {
			auto t0 = std::chrono::steady_clock::now();
			auto boxes = engine->Read(img, score);
			auto t1 = std::chrono::steady_clock::now();
			result["ms"] = std::chrono::duration<double, std::milli>(t1 - t0).count();
			QJsonArray list;
			for (const OcrBox &b : boxes) {
				QJsonObject o;
				o["text"] = QString::fromStdString(b.text);
				o["score"] = b.score;
				o["box"] = QJsonArray{b.x0, b.y0, b.x1, b.y1};
				list.append(o);
			}
			result["boxes"] = list;
		}
		printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
		fflush(stdout);
	}
	return 0;
}
