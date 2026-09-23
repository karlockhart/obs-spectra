/* Parity tests for Obscura's C++ port against fixtures generated from the
 * Python code (make_fixtures.py): obscura-tests <tests/data> <scratch dir> */

#include "definitions.hpp"
#include "learner.hpp"
#include "naming.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace obscura;

static int checks = 0, failures = 0;

#define CHECK(cond, ...)                                              \
	do {                                                          \
		checks++;                                             \
		if (!(cond)) {                                        \
			failures++;                                   \
			printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
		}                                                     \
	} while (0)

static Features ToFeatures(const QJsonObject &o)
{
	Features f;
	f.channel = o["channel"].toString();
	for (const QJsonValue &t : o["tags"].toArray()) {
		f.tags << t.toString();
	}
	f.body = o["body"].toString();
	if (o["colour"].isArray()) {
		std::array<double, 13> c{};
		QJsonArray a = o["colour"].toArray();
		for (int i = 0; i < 13; i++) {
			c[i] = a[i].toDouble();
		}
		f.colour = c;
	}
	return f;
}

static QString U32(const std::u32string &s)
{
	return QString::fromUcs4(s.data(), (qsizetype)s.size());
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (argc < 3) {
		printf("usage: obscura-tests <tests/data> <scratch dir>\n");
		return 2;
	}
	const QDir data(QString::fromLocal8Bit(argv[1]));
	const QDir scratch(QString::fromLocal8Bit(argv[2]));
	QDir().mkpath(scratch.path());

	QFile fx(data.filePath("fixtures.json"));
	(void)fx.open(QIODevice::ReadOnly);
	const QJsonObject F = QJsonDocument::fromJson(fx.readAll()).object();
	CHECK(!F.isEmpty(), "fixtures.json missing");

	/* --- TF-IDF char_wb --------------------------------------------------- */
	std::vector<QString> docs;
	for (const QJsonValue &d : F["docs"].toArray()) {
		docs.push_back(d.toString());
	}
	for (size_t i = 0; i < docs.size(); i++) {
		std::vector<std::u32string> got = TfidfCharWb::Ngrams(docs[i]);
		QJsonArray want = F["ngrams"].toArray()[(int)i].toArray();
		bool same = (int)got.size() == want.size();
		for (int k = 0; same && k < want.size(); k++) {
			same = U32(got[k]) == want[k].toString();
		}
		CHECK(same, "ngrams of doc %zu: %zu vs %d", i, got.size(), (int)want.size());
	}
	TfidfCharWb vec;
	vec.Fit(docs);
	for (size_t i = 0; i < docs.size(); i++) {
		QJsonObject want = F["tfidf"].toArray()[(int)i].toObject();
		SparseRow row = vec.Transform(docs[i]);
		CHECK((int)row.size() == want.size(), "tfidf nnz doc %zu: %zu vs %d", i, row.size(), (int)want.size());
		double maxErr = 0;
		/* compare as sorted value multisets (vocabulary order differs) */
		std::vector<double> a, b;
		for (auto [idx, val] : row) {
			a.push_back(val);
		}
		for (auto it = want.begin(); it != want.end(); ++it) {
			b.push_back(it.value().toDouble());
		}
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		for (size_t k = 0; k < a.size() && k < b.size(); k++) {
			maxErr = std::max(maxErr, std::abs(a[k] - b[k]));
		}
		CHECK(maxErr < 1e-9, "tfidf values doc %zu: max err %g", i, maxErr);
	}

	/* --- Learner parity ---------------------------------------------------- */
	std::vector<Features> test;
	for (const QJsonValue &t : F["test"].toArray()) {
		test.push_back(ToFeatures(t.toObject()));
	}

	const QString learnDir = scratch.filePath("learner");
	QDir(learnDir).removeRecursively();
	QDir().mkpath(learnDir);
	{
		Learner learner(learnDir, {"admin chat", "report"});
		std::vector<Prediction> empty = learner.Predict(test);
		double err = 0;
		for (size_t i = 0; i < test.size(); i++) {
			err = std::max(err, std::abs(empty[i].prob - F["empty_probs"].toArray()[(int)i].toDouble()));
		}
		CHECK(err < 1e-12, "untrained predictions: max err %g", err);

		QJsonArray train = F["train"].toArray();
		for (int i = 0; i < train.size(); i += 8) {
			std::vector<Features> feats;
			std::vector<bool> labels;
			for (int k = i; k < std::min<int>(i + 8, (int)train.size()); k++) {
				feats.push_back(ToFeatures(train[k].toObject()));
				labels.push_back(train[k].toObject()["label"].toInt() != 0);
			}
			learner.AddScreen(feats, labels);
		}
		CHECK(learner.Screens() == 20, "screens %d", learner.Screens());
		CHECK(learner.HasModel(), "no model");
		std::vector<Prediction> got = learner.Predict(test);
		double maxErr = 0, tightErr = 0;
		int flips = 0;
		for (size_t i = 0; i < test.size(); i++) {
			const double want = F["probs"].toArray()[(int)i].toDouble();
			maxErr = std::max(maxErr, std::abs(got[i].prob - want));
			tightErr = std::max(tightErr,
					    std::abs(got[i].prob - F["probs_tight"].toArray()[(int)i].toDouble()));
			flips += got[i].Censor() != (want >= 0.5);
			CHECK(got[i].source == F["sources"].toArray()[(int)i].toString(), "source %zu", i);
		}
		printf("learner: max prob diff vs sklearn %.2e (fully converged: %.2e), %d decision flips\n", maxErr,
		       tightErr, flips);
		CHECK(tightErr < 1e-5, "trained predictions vs converged sklearn: max err %g", tightErr);
		CHECK(maxErr < 1e-2, "trained predictions vs default sklearn: max err %g", maxErr);
		CHECK(flips == 0, "decision flips %d", flips);
	}
	{
		/* Reloading training.jsonl gives the same model */
		Learner reloaded(learnDir, {"admin chat", "report"});
		CHECK(reloaded.Screens() == 20 && reloaded.Samples() == 160, "reload %d/%d", reloaded.Screens(),
		      reloaded.Samples());
		std::vector<Prediction> got = reloaded.Predict(test);
		double maxErr = 0;
		for (size_t i = 0; i < test.size(); i++) {
			maxErr = std::max(maxErr, std::abs(got[i].prob - F["probs"].toArray()[(int)i].toDouble()));
		}
		CHECK(maxErr < 1e-2, "reloaded predictions: max err %g", maxErr);
	}
	{
		/* Our training.jsonl lines match Python's json.dumps byte for byte
		 * (screen ids aside) */
		QFile py(data.filePath("training.jsonl"));
		(void)py.open(QIODevice::ReadOnly);
		QList<QByteArray> lines = py.readAll().split('\n');
		int same = 0, total = 0;
		for (const QByteArray &line : lines) {
			if (line.trimmed().isEmpty()) {
				continue;
			}
			total++;
			QJsonObject o = QJsonDocument::fromJson(line).object();
			QByteArray mine = TrainingLine(ToFeatures(o), o["label"].toInt(), o["screen"].toString());
			if (mine.trimmed() != line.trimmed()) {
				printf("python: %s\nmine:   %s\n", line.constData(), mine.constData());
			} else {
				same++;
			}
		}
		CHECK(same == total, "training.jsonl lines identical %d/%d", same, total);
	}

	/* --- Definitions --------------------------------------------------------- */
	{
		QString err;
		std::optional<Definitions> real = ReadObx(data.filePath("obscura-defs-v1.obx"), &err);
		CHECK(real.has_value(), "published defs-v1 rejected: %s", qPrintable(err));
		if (real) {
			CHECK(real->defsVersion == 1, "version %d", real->defsVersion);
			CHECK(real->channelStats.at("admin chat")[1] == 2, "admin chat stats");
			CHECK(real->seeds.contains("report"), "seeds");
		}

		const QByteArray testPub = F["test_public_key"].toString().toLatin1();
		QFile tf(data.filePath("test-defs.obx"));
		(void)tf.open(QIODevice::ReadOnly);
		const QByteArray blob = tf.readAll();
		std::optional<Definitions> t = LoadsObx(blob, &err, true, testPub);
		CHECK(t.has_value(), "test defs: %s", qPrintable(err));
		CHECK(!LoadsObx(blob, &err, true).has_value(), "test defs accepted with the release key");
		if (t) {
			const QByteArray canon = CanonicalJson(t->Payload());
			CHECK(canon == F["payload_canonical"].toString().toLatin1(), "canonical json:\n%s\n%s",
			      canon.constData(), F["payload_canonical"].toString().toLatin1().constData());
		}

		const QByteArray pem = F["test_private_pem"].toString().toLatin1();
		const QByteArray msg = F["payload_canonical"].toString().toLatin1();
		std::optional<QByteArray> sig = Sign(pem, msg);
		CHECK(sig && *sig == F["payload_signature"].toString().toLatin1(), "signature differs from Python");
		CHECK(VerifySignature(testPub, msg, F["payload_signature"].toString().toLatin1()), "verify");
		CHECK(!VerifySignature(testPub, msg + " ", F["payload_signature"].toString().toLatin1()),
		      "tampered ok");

		/* Round trip through our writer */
		Definitions d;
		d.defsVersion = 9;
		d.channelStats["admin chat"] = {1, 40};
		d.channelStats["\xc3\xa9mote"] = {3, 0};
		d.seeds = {"report"};
		const QString path = scratch.filePath("roundtrip.obx");
		CHECK(WriteObx(path, d, pem, &err), "write: %s", qPrintable(err));
		QFile rf(path);
		(void)rf.open(QIODevice::ReadOnly);
		std::optional<Definitions> back = LoadsObx(rf.readAll(), &err, true, testPub);
		CHECK(back && back->defsVersion == 9 && back->channelStats.at("admin chat")[1] == 40, "round trip: %s",
		      qPrintable(err));

		const QByteArray releases = R"([
			{"draft": false, "prerelease": true, "assets": [{"name": "obscura-defs-v3.obx", "browser_download_url": "u3"}]},
			{"draft": true, "prerelease": false, "assets": [{"name": "obscura-defs-v9.obx", "browser_download_url": "u9"}]},
			{"draft": false, "prerelease": false, "assets": [{"name": "obscura.exe", "browser_download_url": "x"},
				{"name": "obscura-defs-v2.obx", "browser_download_url": "u2"}]}])";
		auto up = FindDefsUpdate(releases, 1, true);
		CHECK(up && up->version == 3 && up->url == "u3", "defs update pick");
		up = FindDefsUpdate(releases, 1, false);
		CHECK(up && up->version == 2, "defs update without prereleases");
		CHECK(!FindDefsUpdate(releases, 3, true), "no newer defs");
	}

	/* --- Naming ---------------------------------------------------------------- */
	{
		QDateTime cap(QDate(2026, 9, 12), QTime(11, 41, 45), QTimeZone::UTC);
		CHECK(TimestampSuffix(1789231305, cap, "utc") == "2026-09-12_16-41-45_1789231305", "%s",
		      qPrintable(TimestampSuffix(1789231305, cap, "utc")));
		CHECK(TimestampSuffix(std::nullopt, cap, "utc") == "2026-09-12_11-41-45", "fallback suffix");
		const QString folder = scratch.filePath("names");
		QDir(folder).removeRecursively();
		QDir().mkpath(folder);
		QString p1 = OutputPath(folder, "shot_2026-01-01_00-00-00_1789231305", "S");
		CHECK(QFileInfo(p1).fileName() == "shot_S.png", "%s", qPrintable(p1));
		(void)QFile(p1).open(QIODevice::WriteOnly);
		CHECK(QFileInfo(OutputPath(folder, "shot", "S")).fileName() == "shot_S (2).png", "unique");
		CHECK(OurSuffixRe().match("fivem_2026-09-12_11-41-45").hasMatch(), "suffix re");
		CHECK(IsImageFile("a.JPEG") && !IsImageFile("a.txt"), "image ext");
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
