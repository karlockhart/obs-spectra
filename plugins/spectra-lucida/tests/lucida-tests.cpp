/*
 * Lucida core tests: ports of lucida/tests/test_store.py and
 * test_recorder.py, plus regression tests for the behaviours Spectra fixes.
 *
 *   lucida-tests <scratch dir>
 */

#include "../core/carnivore.hpp"
#include "../core/cloud.hpp"
#include "../core/lan.hpp"
#include "../core/lan-storage.hpp"
#include "../core/prisma.hpp"
#include "../core/profiles.hpp"
#include "../core/recorder.hpp"
#include "../core/store.hpp"
#include "../core/tagger.hpp"
#include "../core/video.hpp"

#include <monocypher-ed25519.h>
#include <sqlite3.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QPainter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <functional>
#include <future>
#include <map>

using namespace lucida;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                                   \
	do {                                                                          \
		checks++;                                                             \
		if (!(cond)) {                                                        \
			failures++;                                                   \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                     \
	} while (0)

static QString scratch;
static int testNo = 0;

static QString NewDb()
{
	QString dir = QDir(scratch).filePath(QStringLiteral("t%1").arg(++testNo));
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);
	return QDir(dir).filePath("log.db");
}

static void Test(const char *name, const std::function<void()> &fn)
{
	int before = failures;
	fn();
	printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

static long long NowSecs()
{
	return QDateTime::currentSecsSinceEpoch();
}

/* FakeEntry.make */
static spectra::ChatEntry Entry(const char *clock, const QString &body, const char *channel = "say",
				float score = 0.95f)
{
	spectra::ChatEntry e;
	if (clock) {
		e.time = QString::fromLatin1(clock);
	}
	e.body = body;
	e.channel = QString::fromLatin1(channel);
	spectra::OcrBox b;
	b.score = score;
	spectra::Row r;
	r.boxes.push_back(b);
	e.rows.push_back(r);
	return e;
}

static QStringList Bodies(const std::vector<LogLine> &lines)
{
	QStringList out;
	for (const LogLine &l : lines) {
		out << l.body;
	}
	return out;
}

/* ---- recorder fixtures ---- */

constexpr int kWidth = 520, kHeight = 240, kPitch = 24;

static spectra::Image DrawFrame(const QStringList &lines)
{
	QImage q(kWidth, kHeight, QImage::Format_RGB888);
	q.fill(QColor(24, 24, 24));
	QPainter p(&q);
	p.setPen(QColor(235, 230, 230));
	QFont f = p.font();
	f.setPixelSize(12);
	p.setFont(f);
	for (int i = 0; i < lines.size(); i++) {
		p.drawText(8, 24 + i * kPitch, lines[i]);
	}
	p.end();
	spectra::Image img(kWidth, kHeight, 3);
	for (int y = 0; y < kHeight; y++) {
		const uchar *src = q.constScanLine(y);
		uint8_t *dst = img.row(y);
		for (int x = 0; x < kWidth; x++) {
			dst[x * 3 + 0] = src[x * 3 + 2];
			dst[x * 3 + 1] = src[x * 3 + 1];
			dst[x * 3 + 2] = src[x * 3 + 0];
		}
	}
	return img;
}

class FakeOcr : public spectra::OcrEngine {
public:
	explicit FakeOcr(std::vector<QStringList> s) : script(std::move(s)) {}
	std::vector<QStringList> script;
	int reads = 0;
	bool clipFirst = false;

	std::vector<spectra::OcrBox> Read(const spectra::Image &, float) override
	{
		const QStringList &lines = script[std::min<size_t>(reads, script.size() - 1)];
		reads++;
		std::vector<spectra::OcrBox> boxes;
		for (int i = 0; i < lines.size(); i++) {
			spectra::OcrBox b;
			b.text = lines[i].toStdString();
			b.score = 0.95f;
			b.x0 = 8;
			b.y0 = 10.f + i * kPitch;
			b.x1 = 8.f + 8 * lines[i].size();
			b.y1 = 24.f + i * kPitch;
			boxes.push_back(b);
		}
		if (clipFirst && !boxes.empty()) {
			boxes[0].y0 = 0.0f;
			boxes[0].y1 = 12.0f;
		}
		return boxes;
	}
	std::string RecognizeLine(const spectra::Image &) override { return "30 | 1789231305"; }
};

struct Rig {
	Store store;
	FakeOcr ocr;
	std::vector<spectra::Image> frames;
	size_t next = 0;
	bool noWindow = false;
	RecorderConfig cfg;
	std::unique_ptr<Recorder> recorder;

	explicit Rig(const std::vector<QStringList> &script, bool keepFrames = false) : store(NewDb()), ocr(script)
	{
		store.Open();
		cfg.keepFrames = keepFrames;
		cfg.framesDir = QFileInfo(store.Path()).absoluteDir().filePath("frames");
		for (const QStringList &lines : script) {
			frames.push_back(DrawFrame(lines));
		}
		cfg.adaptive = true;
		cfg.interval = 5.0;
		cfg.minInterval = 2.0;
		cfg.maxInterval = 30.0;
		cfg.chatRegion = {0.0, 0.0, 1.0, 1.0};
		cfg.hudRegion = {0.4, 0.0, 0.6, 0.06};
		recorder = std::make_unique<Recorder>(cfg, store, ocr, [this]() -> std::optional<GrabbedFrame> {
			if (noWindow || next >= frames.size()) {
				return std::nullopt;
			}
			return GrabbedFrame{frames[next++], QStringLiteral("test")};
		});
	}
};

/* ---- carnivore fixtures: text anywhere on a 1920x1080 screen ---- */

constexpr int kScreenW = 1920, kScreenH = 1080, kLineH = 14, kLinePitch = 24;

static spectra::OcrBox Box(const QString &text, float x0, float y0, float x1 = -1)
{
	spectra::OcrBox b;
	b.text = text.toStdString();
	b.score = 0.95f;
	b.x0 = x0;
	b.y0 = y0;
	b.x1 = x1 >= 0 ? x1 : x0 + 8.f * text.size();
	b.y1 = y0 + kLineH;
	return b;
}

/* Lines left-aligned at x (a chat box) */
static std::vector<spectra::OcrBox> LeftLines(const QStringList &lines, float x, float y)
{
	std::vector<spectra::OcrBox> out;
	for (int i = 0; i < lines.size(); i++) {
		out.push_back(Box(lines[i], x, y + i * kLinePitch));
	}
	return out;
}

/* Lines right-aligned at x1 (a kill feed) */
static std::vector<spectra::OcrBox> RightLines(const QStringList &lines, float x1, float y)
{
	std::vector<spectra::OcrBox> out;
	for (int i = 0; i < lines.size(); i++) {
		out.push_back(Box(lines[i], x1 - 8.f * lines[i].size(), y + i * kLinePitch, x1));
	}
	return out;
}

static std::vector<spectra::OcrBox> Join(std::initializer_list<std::vector<spectra::OcrBox>> parts)
{
	std::vector<spectra::OcrBox> out;
	for (const auto &p : parts) {
		out.insert(out.end(), p.begin(), p.end());
	}
	return out;
}

static spectra::Image DrawBoxes(const std::vector<spectra::OcrBox> &boxes)
{
	QImage q(kScreenW, kScreenH, QImage::Format_RGB888);
	q.fill(QColor(40, 60, 40));
	QPainter p(&q);
	p.setPen(QColor(235, 230, 230));
	QFont f = p.font();
	f.setPixelSize(12);
	p.setFont(f);
	for (const spectra::OcrBox &b : boxes) {
		p.drawText((int)b.x0, (int)b.y1 - 2, QString::fromStdString(b.text));
	}
	p.end();
	spectra::Image img(kScreenW, kScreenH, 3);
	for (int y = 0; y < kScreenH; y++) {
		const uchar *src = q.constScanLine(y);
		uint8_t *dst = img.row(y);
		for (int x = 0; x < kScreenW; x++) {
			dst[x * 3 + 0] = src[x * 3 + 2];
			dst[x * 3 + 1] = src[x * 3 + 1];
			dst[x * 3 + 2] = src[x * 3 + 0];
		}
	}
	return img;
}

/* Returns scripted boxes, one list per read */
class BoxOcr : public spectra::OcrEngine {
public:
	explicit BoxOcr(std::vector<std::vector<spectra::OcrBox>> s) : script(std::move(s)) {}
	std::vector<std::vector<spectra::OcrBox>> script;
	int reads = 0;

	std::vector<spectra::OcrBox> Read(const spectra::Image &, float) override
	{
		return script[std::min<size_t>(reads++, script.size() - 1)];
	}
	std::string RecognizeLine(const spectra::Image &) override { return "30 | 1789231305"; }
};

static QStringList EntryBodies(const std::vector<spectra::ChatEntry> &entries, const QString &region)
{
	QStringList out;
	for (const spectra::ChatEntry &e : entries) {
		if (e.region == region) {
			out << e.body;
		}
	}
	return out;
}

static const QStringList kFeed{"Alice killed Bob", "Carl killed Dave with a pistol", "Erin killed Frank"};

/* ---- Prisma fixtures ---- */

/* RFC 8032 test 1 key as PKCS#8, as Prisma's keygen writes it */
static const char *kTestPem = "-----BEGIN PRIVATE KEY-----\n"
			      "MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n"
			      "-----END PRIVATE KEY-----\n";
static const char *kTestPublic = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

static PrismaCredentials TestCredentials(const QString &url = "https://prisma.test")
{
	QJsonObject o{{"url", url + "/"},
		      {"client_id", "c_desktop"},
		      {"kid", "k1"},
		      {"audience", "prisma:test"},
		      {"private_key", QString::fromLatin1(kTestPem)}};
	return *PrismaCredentials::FromJson(QJsonDocument(o).toJson());
}

/* An in-process stand-in for Prisma's Lucida plugin */
struct FakePrisma {
	int tokens = 0;
	int failNext = 0; /* answer this many requests with a 503 */
	int rejectToken = 0;
	bool plain422 = false;                /* a 422 without FastAPI's per-line detail */
	int lostRecords = 0;                  /* /uploaded answers 404 this many times */
	std::map<QString, QJsonObject> lines; /* by local_id */
	std::map<QString, QJsonObject> sessions;
	std::map<QString, QByteArray> images;
	std::map<QString, QJsonObject> frames;
	QStringList calls;

	HttpResponse Json(int status, const QJsonObject &o = {})
	{
		return {status, QJsonDocument(o).toJson(QJsonDocument::Compact), {}};
	}

	HttpResponse operator()(const HttpRequest &r)
	{
		const QUrl url(r.url);
		const QString path = url.path();
		calls << QString::fromLatin1(r.method) + ' ' + path;
		if (path == "/v1/token") {
			tokens++;
			return Json(200, {{"access_token", QStringLiteral("t%1").arg(tokens)}, {"expires_in", 900}});
		}
		if (url.host() == "storage.test") {
			/* the presigned POST: the file part is last */
			const QByteArray marker = "filename=\"";
			const int at = r.body.indexOf(marker);
			const QString name = QString::fromUtf8(r.body.mid(
				at + marker.size(), r.body.indexOf('"', at + marker.size()) - at - marker.size()));
			const int start = r.body.indexOf("\r\n\r\n", at) + 4;
			const int end = r.body.lastIndexOf("\r\n--");
			images[name] = r.body.mid(start, end - start);
			return {204, {}, {}};
		}
		if (failNext > 0) {
			failNext--;
			return Json(503, {{"detail", "busy"}});
		}
		const QByteArray auth = [&] {
			for (const auto &[k, v] : r.headers) {
				if (k == "Authorization") {
					return v;
				}
			}
			return QByteArray();
		}();
		if (rejectToken > 0 && auth == "Bearer t1") {
			rejectToken--;
			return Json(401, {{"detail", "token revoked"}});
		}
		const QJsonObject body = QJsonDocument::fromJson(r.body).object();
		const QString base = "/v1/lucida/sources/me";
		if (path == base) {
			return Json(200, {{"source", "c_desktop"}});
		}
		if (path == base + "/lines") {
			const QJsonArray list = body.value("lines").toArray();
			QJsonArray detail;
			for (int i = 0; i < list.size(); i++) {
				if (list[i].toObject().value("body").toString().contains("REFUSE")) {
					detail.append(QJsonObject{{"loc", QJsonArray{"body", "lines", i, "body"}},
								  {"msg", "refused"}});
				}
			}
			if (!detail.isEmpty()) {
				return plain422 ? Json(422, {{"detail", "bad line"}}) : Json(422, {{"detail", detail}});
			}
			for (const QJsonValue &v : list) {
				lines[QString::number(v.toObject().value("local_id").toInteger())] = v.toObject();
			}
			return Json(200, {{"written", list.size()}});
		}
		if (path.startsWith(base + "/sessions/")) {
			sessions[path.section('/', -1)] = body;
			return Json(200);
		}
		if (path.startsWith(base + "/frames/") && path.endsWith("/uploaded")) {
			const QString key = path.section('/', -2, -2);
			if (lostRecords > 0) {
				lostRecords--;
				return Json(404, {{"detail", "no such frame; PUT it first"}});
			}
			return images.count(key) ? Json(200, {{"uploaded", true}}) : Json(409, {{"detail", "not yet"}});
		}
		if (path.startsWith(base + "/frames/")) {
			const QString key = path.section('/', -1);
			frames[key] = body;
			return Json(200,
				    {{"frame", QJsonObject{}},
				     {"upload", QJsonObject{{"url", "https://storage.test/bucket"},
							    {"fields", QJsonObject{{"key", key},
										   {"Content-Type",
										    body.value("content_type")}}}}}});
		}
		return Json(404, {{"detail", "no route"}});
	}
};

/* Sightings of lines without positions, for tests that only need the links */
static std::vector<Sighting> Seen(const std::vector<long long> &ids)
{
	std::vector<Sighting> out;
	for (long long id : ids) {
		out.push_back({id, 0, std::nullopt});
	}
	return out;
}

static spectra::ChatEntry At(spectra::ChatEntry e, int y)
{
	e.rects = {{10, y, 300, y + 20}};
	return e;
}

static const QStringList kChat{"[16:38:10] [Admin Chat] Bob: come to the docks", "[16:38:12] Harry says: on my way",
			       "[16:38:14] ((Sally)): brb one sec"};
static const QStringList kUntimed{"A message with no timestamp", "* Colt sends ren the poster", "(( brb one sec ))"};
static const QStringList kWrapped{
	"[16:38:10] Alistair Moore (52): PD to SD show one unit entering your juro to retrieve",
	"a cruiser from Route 68 bank"};

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	scratch = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::temp().filePath("lucida-tests");
	QDir().mkpath(scratch);

	/* ---- test_store.py ---- */

	Test("normalise_and_key", [] {
		CHECK(Normalise("  [16:38] Bob: Hello, THERE! ") == "16 38 bob hello there");
		CHECK(DedupKey(QStringLiteral("16:38:10"), "Hi") == DedupKey(QStringLiteral("16:38:10"), "hi!"));
		CHECK(DedupKey(QStringLiteral("16:38:10"), "Hi") != DedupKey(QStringLiteral("16:38:11"), "Hi"));
	});

	Test("a_line_seen_in_many_frames_is_stored_once", [] {
		Store s(NewDb());
		CHECK(s.Open());
		std::vector<spectra::ChatEntry> e{Entry("16:38:10", "Bob says: hello there")};
		CHECK(s.AddFrame(e, 1789231300).size() == 1);
		for (int t = 1; t < 6; t++) {
			CHECK(s.AddFrame(e, 1789231300 + t * 5).empty());
		}
		auto lines = s.Recent();
		CHECK(lines.size() == 1 && lines[0].frames == 6);
	});

	Test("ocr_noise_does_not_create_a_second_line", [] {
		Store s(NewDb());
		s.Open();
		s.AddFrame({Entry("16:38:10", "Bob says: meet me at the docks")}, 1789231300);
		s.AddFrame({Entry("16:38:10", "Bob says: meet me at the dock5")}, 1789231305);
		CHECK(s.Recent().size() == 1);
	});

	Test("a_better_reading_replaces_the_stored_one", [] {
		Store s(NewDb());
		s.Open();
		s.AddFrame({Entry("16:38:10", "Bob says: meet me at the dock5", "say", 0.55f)}, 1789231300);
		s.AddFrame({Entry("16:38:10", "Bob says: meet me at the docks", "say", 0.98f)}, 1789231305);
		auto lines = s.Recent();
		CHECK(lines.size() == 1 && lines[0].body.endsWith("docks") && std::fabs(lines[0].score - 0.98) < 1e-6);
	});

	Test("the_same_text_returning_after_the_window_is_a_new_line", [] {
		Store s(NewDb(), 60);
		s.Open();
		s.AddFrame({Entry("16:38:10", "hello")}, 1789231300);
		s.AddFrame({Entry("16:38:10", "hello")}, 1789231300 + 600);
		CHECK(s.Recent().size() == 2);
	});

	Test("lines_come_back_in_time_order", [] {
		Store s(NewDb());
		s.Open();
		s.AddFrame({Entry("16:38:10", "first"), Entry("16:38:11", "second")}, 1789231300);
		s.AddFrame({Entry("16:38:20", "third")}, 1789231320);
		CHECK((Bodies(s.Recent()) == QStringList{"first", "second", "third"}));
	});

	Test("search_finds_a_line", [] {
		Store s(NewDb());
		s.Open();
		CHECK(s.HasFts());
		s.AddFrame({Entry("16:38:10", "Report from Harry about a hacker", "report")}, 1789231300);
		s.AddFrame({Entry("16:38:20", "unrelated chatter")}, 1789231320);
		CHECK((Bodies(s.Search("hacker")) == QStringList{"Report from Harry about a hacker"}));
		CHECK(!s.Search("hacker", 50, "report").empty());
		CHECK(s.Search("nothingatall").empty());
	});

	Test("dedup_window_survives_a_restart", [] {
		QString path = NewDb();
		long long now = NowSecs();
		{
			Store first(path);
			first.Open();
			first.AddFrame({Entry("16:38:10", "still on screen")}, now);
		}
		Store second(path);
		second.Open();
		CHECK(second.AddFrame({Entry("16:38:10", "still on screen")}, now + 5).empty());
		CHECK(second.Recent().size() == 1);
	});

	Test("prune_drops_old_lines", [] {
		Store s(NewDb());
		s.Open();
		s.AddFrame({Entry("16:38:10", "ancient history")}, NowSecs() - 40 * 86400);
		s.AddFrame({Entry("16:38:10", "todays news")}, NowSecs());
		CHECK(s.Prune(30) == 1);
		CHECK((Bodies(s.Recent()) == QStringList{"todays news"}));
		CHECK(s.Prune(0) == 0);
	});

	Test("stats_reports_the_dedup_ratio", [] {
		Store s(NewDb());
		s.Open();
		for (int t = 0; t < 4; t++) {
			s.AddFrame({Entry("16:38:10", "hello")}, 1789231300 + t * 5);
		}
		Stats st = s.GetStats();
		CHECK(st.lines == 1 && st.sightings == 4);
	});

	Test("a_frame_is_linked_to_the_lines_read_from_it", [] {
		Store s(NewDb());
		s.Open();
		auto ids = s.AddFrame({Entry("16:38:10", "first"), Entry("16:38:11", "second")}, 1789231300);
		long long fid = s.AttachFrame(Seen(ids), "shot.jpg", 1920, 1080, 1789231300);
		auto f = s.GetFrame(fid);
		CHECK(f && f->width == 1920 && f->path == "shot.jpg");
		CHECK((Bodies(s.FrameLines(fid)) == QStringList{"first", "second"}));
		bool allLinked = true;
		for (const LogLine &l : s.Recent()) {
			allLinked = allLinked && l.frameId == fid;
		}
		CHECK(allLinked);
		CHECK(s.AddFrame({Entry("16:38:10", "first")}, 1789231305).empty());
		CHECK(s.FrameLines(fid)[0].frameId == fid);
	});

	Test("a_screenshot_lists_every_line_on_it_where_it_is_there", [] {
		Store s(NewDb());
		s.Open();
		std::vector<Sighting> seen1;
		s.AddFrame({At(Entry("16:38:10", "first"), 100), At(Entry("16:38:11", "second"), 120)}, 1789231300,
			   "wall", {}, &seen1);
		long long f1 = s.AttachFrame(seen1, "one.jpg", 1920, 1080, 1789231300);
		/* chat scrolled: the old lines moved up and one arrived below them */
		std::vector<Sighting> seen2;
		auto added = s.AddFrame({At(Entry("16:38:10", "first"), 60), At(Entry("16:38:11", "second"), 80),
					 At(Entry("16:38:20", "third"), 100)},
					1789231320, "wall", {}, &seen2);
		CHECK(added.size() == 1 && seen2.size() == 3);
		long long f2 = s.AttachFrame(seen2, "two.jpg", 1920, 1080, 1789231320);
		auto lines = s.FrameLines(f2);
		CHECK((Bodies(lines) == QStringList{"first", "second", "third"}));
		CHECK((lines[0].rect == spectra::Rect{10, 60, 300, 80}));
		CHECK((s.FrameLines(f1)[0].rect == spectra::Rect{10, 100, 300, 120}));
		/* a line's own screenshot stays the first one it was in */
		CHECK(lines[0].frameId == f1 && lines[2].frameId == f2);
		Query q;
		q.frameId = f2;
		CHECK((Bodies(s.Find(q)) == QStringList{"first", "second", "third"}));
		auto frames = s.Frames();
		CHECK(frames.size() == 2 && frames[0].frame.id == f2 && frames[0].lines == 3);
	});

	Test("pruning_a_screenshot_moves_its_lines_to_the_next_one", [] {
		Store s(NewDb());
		s.Open();
		long long oldTs = NowSecs() - 40 * 86400, newTs = NowSecs();
		std::vector<Sighting> seen1, seen2;
		s.AddFrame({Entry("16:38:10", "still here")}, oldTs, "wall", {}, &seen1);
		s.AttachFrame(seen1, "old.jpg", 1920, 1080, oldTs);
		s.AddFrame({Entry("16:38:10", "still here")}, oldTs + 5, "wall", {}, &seen2);
		long long kept = s.AttachFrame(seen2, "new.jpg", 1920, 1080, newTs);
		CHECK((s.PruneFrames(30) == QStringList{"old.jpg"}));
		CHECK(s.Recent()[0].frameId == kept);
		CHECK(s.FrameLines(kept).size() == 1);
	});

	Test("screenshots_from_before_frame_lines_keep_their_lines", [] {
		QString path = NewDb();
		long long fid;
		{
			Store s(path);
			s.Open();
			auto e = Entry("16:38:10", "from an older log");
			e.rects = {{10, 20, 300, 40}};
			fid = s.AttachFrame(Seen(s.AddFrame({e}, 1789231300)), "shot.jpg", 1920, 1080, 1789231300);
			sqlite3_exec(s.Db(), "DELETE FROM frame_lines", nullptr, nullptr, nullptr);
		}
		Store s(path);
		CHECK(s.Open());
		auto lines = s.FrameLines(fid);
		CHECK(lines.size() == 1 && lines[0].rect == (spectra::Rect{10, 20, 300, 40}));
	});

	Test("entry_rects_are_kept_for_the_viewer", [] {
		Store s(NewDb());
		s.Open();
		auto e = Entry("16:38:10", "hello");
		e.rects = {{10, 20, 300, 40}, {10, 40, 260, 60}};
		s.AddFrame({e}, 1789231300);
		CHECK((s.Recent()[0].rect == spectra::Rect{10, 20, 300, 60}));
	});

	Test("pruning_frames_returns_the_files_to_delete", [] {
		Store s(NewDb());
		s.Open();
		long long oldTs = NowSecs() - 40 * 86400, newTs = NowSecs();
		auto oldIds = s.AddFrame({Entry("16:38:10", "ancient")}, oldTs);
		auto newIds = s.AddFrame({Entry("16:38:11", "recent")}, newTs);
		s.AttachFrame(Seen(oldIds), "old.jpg", 1920, 1080, oldTs);
		long long kept = s.AttachFrame(Seen(newIds), "new.jpg", 1920, 1080, newTs);
		CHECK((s.PruneFrames(30) == QStringList{"old.jpg"}));
		CHECK(s.GetFrame(kept).has_value());
		for (const LogLine &l : s.Recent()) {
			if (l.body == "ancient") {
				CHECK(!l.frameId);
			}
		}
	});

	Test("a_database_written_by_0_1_0_is_migrated", [] {
		QString path = NewDb();
		sqlite3 *db = nullptr;
		sqlite3_open(path.toUtf8().constData(), &db);
		sqlite3_exec(db,
			     "CREATE TABLE lines (id INTEGER PRIMARY KEY, session_id INTEGER, sort_ts INTEGER NOT NULL,"
			     " seq INTEGER NOT NULL DEFAULT 0, ts_source TEXT NOT NULL DEFAULT 'wall', clock TEXT,"
			     " channel TEXT, tags TEXT, body TEXT NOT NULL, dedup_key TEXT NOT NULL,"
			     " score REAL NOT NULL DEFAULT 0, frames INTEGER NOT NULL DEFAULT 1,"
			     " first_seen REAL NOT NULL, last_seen REAL NOT NULL);"
			     "INSERT INTO lines(sort_ts, clock, channel, tags, body, dedup_key, score, first_seen,"
			     " last_seen) VALUES (1789231300, '16:38:10', 'say', '[]', 'from the old version',"
			     " '16:38:10|from the old version', 0.9, 0, 0);",
			     nullptr, nullptr, nullptr);
		sqlite3_close(db);
		Store s(path);
		CHECK(s.Open());
		auto l = s.Recent()[0];
		CHECK(l.body == "from the old version" && !l.frameId && !l.rect);
		auto ids = s.AddFrame({Entry("16:39:00", "from the new one")}, 1789231400);
		CHECK(s.AttachFrame(Seen(ids), "shot.jpg", 1920, 1080, 1789231400) > 0);
	});

	Test("the_database_is_in_wal_mode_before_any_schema_is_written", [] {
		QString path = NewDb();
		{
			Store s(path);
			s.Open();
			CHECK(s.Pragma("journal_mode").toLower() == "wal");
			s.AddFrame({Entry("16:38:10", "hello")}, 1789231300);
		}
		Store r(path);
		r.Open();
		CHECK(r.Pragma("journal_mode").toLower() == "wal");
		CHECK(r.Pragma("integrity_check") == "ok");
	});

	Test("two_open_stores_do_not_corrupt_the_file", [] {
		QString path = NewDb();
		{
			Store a(path), b(path);
			a.Open();
			b.Open();
			a.AddFrame({Entry("16:38:10", "from the first")}, 1789231300);
			b.AddFrame({Entry("16:38:20", "from the second")}, 1789231320);
			CHECK(a.Pragma("integrity_check") == "ok");
		}
		Store c(path);
		c.Open();
		QStringList bodies = Bodies(c.Recent());
		CHECK(bodies.contains("from the first") && bodies.contains("from the second"));
	});

	Test("repair_rebuilds_a_log_and_keeps_the_original", [] {
		QString path = NewDb();
		{
			Store s(path);
			s.Open();
			auto ids = s.AddFrame({Entry("16:38:10", "keep me"), Entry("16:38:12", "and me")}, 1789231300);
			s.AttachFrame(Seen(ids), "shot.jpg", 1920, 1080, 1789231300);
		}
		QString error;
		auto result = Store::Repair(path, &error);
		CHECK(result.has_value());
		if (result) {
			CHECK(result->copied["lines"] == 2 && result->copied["frames"] == 1);
			CHECK(result->integrity == "ok");
			CHECK(QFile::exists(result->backup));
		}
		Store r(path);
		r.Open();
		CHECK((Bodies(r.Recent()) == QStringList{"keep me", "and me"}));
		CHECK(r.Recent()[0].frameId.has_value());
		CHECK((Bodies(r.Search("keep")) == QStringList{"keep me"}));
	});

	Test("repair_refuses_while_the_log_is_open_elsewhere", [] {
		QString path = NewDb();
		Store s(path);
		s.Open();
		s.AddFrame({Entry("16:38:10", "hello")}, 1789231300);
		sqlite3_exec(s.Db(), "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr);
		QString error;
		CHECK(!Store::Repair(path, &error));
		CHECK(error.contains("open in another program"));
		sqlite3_exec(s.Db(), "ROLLBACK", nullptr, nullptr, nullptr);
	});

	Test("a_damaged_search_index_does_not_break_searching", [] {
		QString path = NewDb();
		{
			Store s(path);
			s.Open();
			s.AddFrame({Entry("16:38:10", "Report from Harry about a hacker")}, 1789231300);
			CHECK(s.HasFts());
		}
		sqlite3 *db = nullptr;
		sqlite3_open(path.toUtf8().constData(), &db);
		sqlite3_exec(db, "DROP TABLE lines_fts_data", nullptr, nullptr, nullptr);
		sqlite3_close(db);
		Store d(path);
		d.Open();
		CHECK(!d.HasFts());
		CHECK((Bodies(d.Search("hacker")) == QStringList{"Report from Harry about a hacker"}));
	});

	Test("a_bad_search_query_returns_nothing_instead_of_raising", [] {
		Store s(NewDb());
		s.Open();
		s.AddFrame({Entry("16:38:10", "hello")}, 1789231300);
		CHECK(s.Search("\"unbalanced").empty());
	});

	/* ---- Spectra fixes ---- */

	Test("fix_a_bad_query_keeps_full_text_search", [] {
		Store s(NewDb());
		s.Open();
		s.AddFrame({Entry("16:38:10", "hello world")}, 1789231300);
		s.Search("\"unbalanced");
		CHECK(s.HasFts());
		s.AddFrame({Entry("16:38:20", "added after the bad query")}, 1789231320);
		CHECK((Bodies(s.Search("added")) == QStringList{"added after the bad query"}));
	});

	Test("fix_a_line_that_stays_on_screen_is_not_logged_twice", [] {
		Store s(NewDb(), 60);
		s.Open();
		for (int t = 0; t <= 600; t += 30) { /* visible for 10 minutes, window 1 minute */
			s.AddFrame({Entry("16:38:10", "long lived")}, 1789231300 + t);
		}
		CHECK(s.Recent().size() == 1);
	});

	Test("fix_pruning_reports_orphaned_screenshots", [] {
		Store s(NewDb());
		s.Open();
		long long oldTs = NowSecs() - 40 * 86400;
		auto ids = s.AddFrame({Entry("16:38:10", "ancient")}, oldTs);
		s.AttachFrame(Seen(ids), "orphan.jpg", 1920, 1080, oldTs);
		QStringList orphans;
		CHECK(s.Prune(30, &orphans) == 1);
		CHECK((orphans == QStringList{"orphan.jpg"}));
	});

	/* ---- test_recorder.py ---- */

	Test("an_unchanged_frame_costs_no_ocr", [] {
		Rig r({kChat, kChat});
		Tick first = r.recorder->Step(), second = r.recorder->Step();
		CHECK(first.changed && !second.changed);
		CHECK(r.ocr.reads == 1);
		CHECK(second.interval > first.interval);
	});

	Test("new_lines_are_logged_once_each", [] {
		QStringList scrolled = kChat.mid(1) + QStringList{"[16:38:20] Harry says: im here now"};
		Rig r({kChat, scrolled});
		r.recorder->Step();
		Tick t = r.recorder->Step();
		QStringList bodies = Bodies(r.store.Recent());
		CHECK(t.added == 1);
		CHECK(bodies.size() == 4);
		CHECK(bodies.filter("im here now").size() == 1);
		CHECK(bodies.removeDuplicates() == 0);
	});

	Test("the_hud_clock_orders_the_log", [] {
		Rig r({kChat});
		Tick t = r.recorder->Step();
		CHECK(t.tsSource == "hud" && t.frameTs == 1789231305LL);
		auto lines = r.store.Recent();
		bool ts = true;
		for (const LogLine &l : lines) {
			ts = ts && l.sortTs == 1789231305LL;
		}
		CHECK(ts);
		CHECK(lines.size() == 3 && lines[0].seq == 0 && lines[1].seq == 1 && lines[2].seq == 2);
	});

	Test("a_complete_turnover_shortens_the_interval", [] {
		QStringList other{"[16:39:01] Chen says: totally different", "[16:39:03] [OOC] someone: and another",
				  "[16:39:05] Mia says: nothing in common"};
		Rig r({kChat, other});
		r.recorder->Step();
		double before = r.recorder->Interval();
		Tick t = r.recorder->Step();
		CHECK(t.turnover && t.added == 3);
		CHECK(std::fabs(r.recorder->Interval() - std::max(2.0, before / 2)) < 1e-9);
	});

	Test("no_window_means_no_work", [] {
		Rig r({kChat});
		r.noWindow = true;
		Tick t = r.recorder->Step();
		CHECK(!t.foundWindow && r.ocr.reads == 0);
		CHECK(t.interval == r.cfg.idleInterval);
	});

	Test("untimed_lines_are_logged_separately_and_labelled_by_their_content", [] {
		Rig r({kUntimed});
		Tick t = r.recorder->Step();
		CHECK(t.added == 3);
		QMap<QString, QString> logged;
		bool untimed = true;
		for (const LogLine &l : r.store.Recent()) {
			logged[l.body] = l.channel;
			untimed = untimed && !l.clock;
		}
		CHECK(logged.contains("A message with no timestamp"));
		CHECK(logged["* Colt sends ren the poster"] == "emote");
		CHECK(logged["(( brb one sec ))"] == "ooc");
		CHECK(untimed);
	});

	Test("a_line_that_wrapped_stays_one_entry", [] {
		Rig r({kWrapped});
		Tick t = r.recorder->Step();
		auto lines = r.store.Recent();
		CHECK(t.added == 1);
		CHECK(!lines.empty() && lines[0].body.endsWith("Route 68 bank"));
		CHECK(!lines.empty() && lines[0].clock == QStringLiteral("16:38:10"));
	});

	Test("untimed_lines_still_deduplicate_across_frames", [] {
		Rig r({kUntimed, kUntimed + QStringList{"one more, also untimed"}});
		r.recorder->Step();
		Tick t = r.recorder->Step();
		CHECK(t.added == 1);
		CHECK(r.store.Recent().size() == (size_t)kUntimed.size() + 1);
	});

	Test("a_row_clipped_by_the_top_of_the_region_is_dropped", [] {
		Rig r({kUntimed});
		r.ocr.clipFirst = true;
		r.recorder->Step();
		QStringList bodies = Bodies(r.store.Recent());
		CHECK(!bodies.contains("A message with no timestamp"));
		CHECK(bodies.size() == kUntimed.size() - 1);
	});

	Test("each_kept_screenshot_boxes_the_lines_drawn_on_it", [] {
		/* chat scrolls: the first line leaves the top, two arrive below */
		const QStringList second{kChat[1], kChat[2], "[16:38:20] Bob says: here now",
					 "[16:38:22] Harry says: see you"};
		const std::vector<QStringList> script{kChat, second};
		Rig r(script, true);
		r.recorder->Step();
		r.recorder->Step();
		auto frames = r.store.Frames(); /* newest first */
		CHECK(frames.size() == 2);
		if (frames.size() != 2) {
			return;
		}
		std::map<QString, QImage> crops[2]; /* body -> its box cut from the kept screenshot */
		for (int n = 0; n < 2; n++) {
			const Frame &frame = frames[1 - n].frame;
			QImage shot(frame.path);
			CHECK(!shot.isNull() && shot.width() == kWidth);
			auto lines = r.store.FrameLines(frame.id);
			CHECK(lines.size() == (size_t)script[n].size());
			for (size_t i = 0; i < lines.size() && i < (size_t)script[n].size(); i++) {
				/* the line drawn in row i, boxed around row i and no other */
				CHECK(script[n][(int)i].endsWith(lines[i].body));
				const int centre = 17 + (int)i * kPitch;
				CHECK(lines[i].rect && lines[i].rect->y0 <= centre && centre <= lines[i].rect->y1);
				CHECK(lines[i].rect && lines[i].rect->y1 < centre + kPitch &&
				      lines[i].rect->y0 > centre - kPitch);
				if (lines[i].rect) {
					const spectra::Rect &b = *lines[i].rect;
					crops[n][lines[i].body] = shot.copy(0, b.y0, kWidth, b.y1 - b.y0)
									  .convertToFormat(QImage::Format_Grayscale8);
				}
			}
		}
		/* the same line looks the same in both screenshots, and unlike its neighbours */
		auto diff = [](const QImage &a, const QImage &b) {
			const int h = std::min(a.height(), b.height());
			double sum = 0.0;
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < a.width(); x++) {
					sum += std::abs(a.constScanLine(y)[x] - b.constScanLine(y)[x]);
				}
			}
			return sum / std::max(1, h * a.width());
		};
		int compared = 0;
		for (const auto &[body, crop] : crops[1]) {
			auto same = crops[0].find(body);
			if (same == crops[0].end()) {
				continue;
			}
			compared++;
			const double match = diff(crop, same->second);
			for (const auto &[other, otherCrop] : crops[0]) {
				if (other != body) {
					CHECK(match < diff(crop, otherCrop));
				}
			}
		}
		CHECK(compared == 2);
	});

	/* ---- Spectra: tag rules ---- */

	Test("tag_triggers_ignore_case_and_separators", [] {
		Tagger t({{"bodycam-rp", {"bodycam"}}}, false);
		CHECK(t.Tags("turning my Bodycam on") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("my body cam is on") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("body-cam footage") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("BODY_CAM, now!") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("nobody came to the party").isEmpty());
		CHECK(t.Tags("bodycams everywhere").isEmpty());
	});

	Test("tag_triggers_support_wildcards_and_commands", [] {
		Tagger t({{"admin duty", {"/aduty*"}}, {"bodycam-rp", {"body?am*"}}}, false);
		CHECK(t.Tags("[Admin] Bob used /aduty") == QStringList{"admin duty"});
		CHECK(t.Tags("/aduty2 on") == QStringList{"admin duty"});
		CHECK(t.Tags("aduty").isEmpty());
		CHECK(t.Tags("my bodycams are on") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("body camera rolling") == QStringList{"bodycam-rp"});
	});

	Test("tag_typo_tolerance_only_for_long_plain_triggers", [] {
		Tagger t({{"bodycam-rp", {"bodycam"}}, {"cuff", {"cuffs"}}}, true);
		CHECK(t.Tags("bodycan on") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("bodycams on") == QStringList{"bodycam-rp"});
		CHECK(t.Tags("cufs").isEmpty());
		CHECK(t.Tags("nobody came").isEmpty());
	});

	Test("tag_rules_round_trip_through_json", [] {
		const QList<TagRule> rules = DefaultTagRules();
		bool ok = false;
		CHECK(TagRulesFromJson(TagRulesToJson(rules), &ok) == rules);
		CHECK(ok);
		TagRulesFromJson("not json", &ok);
		CHECK(!ok);
	});

	Test("lines_are_labelled_searched_and_relabelled", [] {
		Store s(NewDb());
		CHECK(s.Open());
		Tagger bodycam({{"bodycam-rp", {"bodycam"}}}, false);
		s.labeler = [&](const QString &body) {
			return bodycam.Tags(body);
		};
		const long long ts = NowSecs();
		s.AddFrame({Entry("16:38:10", "turning my body cam on"), Entry("16:38:11", "hello there", "ooc")}, ts);
		auto lines = s.Recent();
		CHECK(lines.size() == 2 && lines[0].labels == QStringList{"bodycam-rp"} && lines[1].labels.isEmpty());
		CHECK(s.Labels() == QStringList{"bodycam-rp"});

		Query q;
		q.label = "bodycam-rp";
		CHECK(s.Find(q).size() == 1);
		q = Query();
		q.channel = "ooc";
		CHECK(s.Find(q).size() == 1 && s.Find(q)[0].body == "hello there");
		q = Query();
		q.text = "hello";
		CHECK(s.Find(q).size() == 1);
		q.from = ts + 10;
		CHECK(s.Find(q).empty());

		Tagger greeting({{"greeting", {"hello"}}}, false);
		s.labeler = [&](const QString &body) {
			return greeting.Tags(body);
		};
		CHECK(s.Relabel() == 2);
		CHECK(s.Labels() == QStringList{"greeting"});
		CHECK(s.Relabel() == 0);
	});

	Test("lines_keep_colour_first_seen_and_video", [] {
		Store s(NewDb());
		CHECK(s.Open());
		spectra::ChatEntry e = Entry("16:38:10", "Bob says: hi");
		e.colour[0] = 0.5f;
		auto ids = s.AddFrame({e}, NowSecs());
		CHECK(ids.size() == 1);
		s.SetVideo(ids, VideoSpot{"C:/loop/2026-09-23 14-05-00.mkv", 42.5});
		std::optional<LogLine> l = s.Line(ids[0]);
		CHECK(l && l->colour && std::fabs((*l->colour)[0] - 0.5) < 1e-6);
		CHECK(l && l->firstSeen > 0);
		CHECK(l && l->video && l->video->path.endsWith("14-05-00.mkv") && l->video->offset == 42.5);
	});

	Test("frames_list_their_lines_and_labels", [] {
		Store s(NewDb());
		CHECK(s.Open());
		Tagger t({{"bodycam-rp", {"bodycam"}}}, false);
		s.labeler = [&](const QString &body) {
			return t.Tags(body);
		};
		auto ids = s.AddFrame({Entry("16:38:10", "bodycam on"), Entry("16:38:11", "hi")}, NowSecs());
		long long frameId = s.AttachFrame(Seen(ids), "C:/frames/x.jpg", 1920, 1080, NowSecs());
		auto frames = s.Frames();
		CHECK(frames.size() == 1 && frames[0].lines == 2 && frames[0].labels == QStringList{"bodycam-rp"});
		Query q;
		q.frameId = frameId;
		CHECK(s.Find(q).size() == 2);
	});

	/* ---- Spectra: carnivore mode ---- */

	Test("carnivore_finds_each_block_of_text", [] {
		const auto boxes = Join({LeftLines(kChat, 10, 20),
					 RightLines(kFeed, 1900, 20),
					 {Box("100", 1800, 1000), Box("Vinewood", 20, 1040)}});
		std::vector<TextBlock> blocks = FindTextBlocks(boxes);
		CHECK(blocks.size() == 4);
		/* screen order: the two top blocks left to right, then the bottom ones */
		CHECK(blocks.size() == 4 && blocks[0].boxes.size() == 3 && blocks[0].rect.x0 == 10);
		CHECK(blocks.size() == 4 && blocks[1].boxes.size() == 3 && blocks[1].rect.x1 == 1900);
	});

	Test("carnivore_reads_chat_feeds_and_labels_but_not_hud_numbers", [] {
		const auto boxes =
			Join({LeftLines(kChat, 10, 20),
			      RightLines(kFeed, 1900, 20),
			      {Box("100", 1800, 1000), Box("Vinewood", 20, 1040), Box("30 | 1789231305", 900, 2)}});
		BoxOcr ocr({boxes});
		RegionTracker tracker({}, {{"chat", spectra::kDefaultChatRegion, RegionMode::Read}});
		int blocks = 0;
		const spectra::Rect hud = spectra::kDefaultHudRegion.ToPixels(kScreenW, kScreenH);
		auto entries = ReadScreen(DrawBoxes(boxes), ocr, tracker, 1000.0, {hud}, &blocks);
		CHECK(blocks == 2);
		CHECK(EntryBodies(entries, "chat").size() == 3);
		/* untimed feed rows of similar width are still one line each */
		CHECK(EntryBodies(entries, "top-right") == kFeed);
		/* street names and other labels are kept, as "other" */
		CHECK(EntryBodies(entries, "other") == QStringList{"Vinewood"});
		CHECK(entries.size() == 7);
		for (size_t i = 0; i < entries.size(); i++) {
			CHECK(entries[i].index == (int)i && !entries[i].rects.empty());
		}
		/* only the feed is learned: the chat box is drawn, "other" is no region */
		auto dirty = tracker.TakeDirty();
		CHECK(dirty.size() == 1 && dirty[0]->name == "top-right");
		CHECK(tracker.TakeDirty().empty());
	});

	Test("carnivore_joins_a_wrapped_sentence", [] {
		const auto boxes = LeftLines({"Server restarts in ten minutes, please find a safe place to park your",
					      "vehicle before then", "Next announcement follows"},
					     700, 500);
		BoxOcr ocr({boxes});
		RegionTracker tracker;
		auto entries = ReadScreen(DrawBoxes(boxes), ocr, tracker, 1000.0);
		CHECK(entries.size() == 2);
		CHECK(entries.size() == 2 && entries[0].body.endsWith("your vehicle before then"));
		CHECK(entries.size() == 2 && entries[0].region == "centre");
	});

	Test("carnivore_regions_keep_their_names", [] {
		CHECK(RegionTracker::PlaceName({0.0, 0.0, 0.2, 0.1}) == "top-left");
		CHECK(RegionTracker::PlaceName({0.4, 0.4, 0.6, 0.6}) == "centre");
		CHECK(RegionTracker::PlaceName({0.4, 0.8, 0.6, 0.9}) == "bottom");
		CHECK(RegionTracker::PlaceName({0.8, 0.4, 0.9, 0.6}) == "right");

		std::vector<spectra::Row> numbers{spectra::Row::FromBox(Box("100", 0, 0)),
						  spectra::Row::FromBox(Box("$ 12,500", 0, 24))};
		std::vector<spectra::Row> labels{spectra::Row::FromBox(Box("Carcer Way", 0, 0)),
						 spectra::Row::FromBox(Box("Vinewood Blvd", 0, 24))};
		std::vector<spectra::Row> text{spectra::Row::FromBox(Box("Alice killed Bob", 0, 0))};
		CHECK(Classify(numbers) == BlockKind::None);
		CHECK(Classify(labels) == BlockKind::Labels);
		CHECK(Classify(text) == BlockKind::Text);

		RegionTracker t({}, {{"chat", spectra::kDefaultChatRegion, RegionMode::Read}});
		CHECK(t.Assign({10, 20, 400, 82}, kScreenW, kScreenH, 1).name == "chat");
		CHECK(t.Assign({1650, 20, 1900, 82}, kScreenW, kScreenH, 1).name == "top-right");
		CHECK(t.Assign({1650, 200, 1900, 262}, kScreenW, kScreenH, 1).name == "top-right 2");
		/* a feed that grows stays the same region, which widens */
		CHECK(t.Assign({1600, 20, 1900, 130}, kScreenW, kScreenH, 2).name == "top-right");
		const ScreenRegion &feed = t.Regions()[0];
		CHECK(std::fabs(feed.area.left - 1600.0 / kScreenW) < 1e-9 && feed.lastSeen == 2 &&
		      feed.firstSeen == 1);
		CHECK(t.Regions().size() == 2);
	});

	Test("carnivore_mode_logs_lines_with_their_region", [] {
		const auto first = Join({LeftLines(kChat.mid(0, 2), 10, 20), RightLines(kFeed.mid(0, 2), 1900, 20)});
		const auto second = Join({LeftLines(kChat, 10, 20), RightLines(kFeed.mid(1, 2), 1900, 20)});
		Store store(NewDb());
		CHECK(store.Open());
		BoxOcr ocr({first, second});
		std::vector<spectra::Image> frames{DrawBoxes(first), DrawBoxes(second)};
		size_t next = 0;
		RecorderConfig cfg;
		cfg.carnivore = true;
		cfg.keepFrames = true;
		cfg.framesDir = QFileInfo(store.Path()).absoluteDir().filePath("frames");
		auto grab = [&]() -> std::optional<GrabbedFrame> {
			if (next >= frames.size()) {
				return std::nullopt;
			}
			return GrabbedFrame{frames[next++], QStringLiteral("test")};
		};
		{
			Recorder recorder(cfg, store, ocr, grab);
			Tick a = recorder.Step();
			CHECK(a.added == 4 && a.regions == 2 && a.frameTs == 1789231305LL);
			Tick b = recorder.Step();
			CHECK(b.added == 2 && b.regions == 2);
			recorder.Close();
		}
		CHECK(store.Recent().size() == 6);
		Query q;
		q.region = "top-right";
		CHECK(Bodies(store.Find(q)) == kFeed);
		q.region = "chat";
		CHECK(store.Find(q).size() == 3);
		CHECK((store.RegionNames() == QStringList{"chat", "top-right"}));
		auto regions = store.ScreenRegions();
		/* the chat box is drawn, so only the feed is a learned region */
		CHECK(regions.size() == 1 && regions[0].id > 0 && regions[0].name == "top-right");

		/* screenshots list every line, each with its region */
		auto shots = store.Frames();
		CHECK(shots.size() == 2);
		if (!shots.empty()) {
			auto lines = store.FrameLines(shots[0].frame.id);
			CHECK(lines.size() == 5);
			CHECK(std::all_of(lines.begin(), lines.end(),
					  [](const LogLine &l) { return !l.region.isEmpty() && l.rect.has_value(); }));
		}

		/* after a restart the regions keep their names */
		const auto third = Join(
			{RightLines({"Gina killed Hank"}, 1900, 20),
			 LeftLines({"Welcome to the server", "Press F1 for help", "Have fun out there"}, 1500, 600)});
		BoxOcr again({third});
		frames = {DrawBoxes(third)};
		next = 0;
		Recorder recorder(cfg, store, again, grab);
		Tick c = recorder.Step();
		CHECK(c.added == 4);
		q.region = "top-right";
		CHECK(store.Find(q).size() == 4);
		CHECK(store.ScreenRegions().size() == 2);
	});

	Test("drawn_regions_name_ignore_and_other_text", [] {
		const auto feed = RightLines(kFeed, 1900, 20);
		const auto map = LeftLines({"Carcer Way", "Vinewood Blvd"}, 60, 900);
		const auto ad = LeftLines({"Buy the new Pegassi Zentorno today"}, 800, 500);
		const auto news = LeftLines({"Weazel News says the port is closed"}, 60, 400);
		const auto boxes = Join({feed, map, ad, news});
		const std::vector<RegionRule> drawn{{"kill feed", {0.8, 0.0, 1.0, 0.15}, RegionMode::Read},
						    {"minimap", {0.0, 0.8, 0.25, 1.0}, RegionMode::Ignore},
						    {"ads", {0.4, 0.4, 0.7, 0.55}, RegionMode::Other}};
		{
			BoxOcr ocr({boxes});
			RegionTracker tracker({}, drawn);
			auto entries = ReadScreen(DrawBoxes(boxes), ocr, tracker, 1000.0);
			CHECK(EntryBodies(entries, "kill feed") == kFeed);
			CHECK(EntryBodies(entries, "other") == QStringList{"Buy the new Pegassi Zentorno today"});
			/* the minimap is ignored; the news, outside every drawn region, is learned */
			CHECK(entries.size() == 5);
			CHECK(tracker.Regions().size() == 1 && tracker.Regions()[0].name == "left");
		}
		{
			/* only the drawn regions: nothing is learned, the rest is "other" */
			BoxOcr ocr({boxes});
			RegionTracker tracker({}, drawn, true);
			auto entries = ReadScreen(DrawBoxes(boxes), ocr, tracker, 1000.0);
			CHECK(EntryBodies(entries, "other").size() == 2);
			CHECK(tracker.Regions().empty() && tracker.TakeDirty().empty());
		}
		{
			/* labels inside a drawn region take its name */
			std::vector<RegionRule> named = drawn;
			named[1].mode = RegionMode::Read;
			BoxOcr ocr({boxes});
			RegionTracker tracker({}, named);
			auto entries = ReadScreen(DrawBoxes(boxes), ocr, tracker, 1000.0);
			CHECK((EntryBodies(entries, "minimap") == QStringList{"Carcer Way", "Vinewood Blvd"}));
		}
		/* a drawn region replaces the learned one of the same name */
		RegionTracker tracker(
			{{1, "kill feed", {0.0, 0.0, 0.1, 0.1}, 1, 1}, {2, "left", {0.0, 0.3, 0.2, 0.4}, 1, 1}}, drawn);
		CHECK(tracker.Regions().size() == 1 && tracker.Regions()[0].name == "left");
		CHECK(WithChatBox(drawn, spectra::kDefaultChatRegion).size() == 4);
		CHECK(WithChatBox({{"chat", {0, 0, 1, 1}}}, spectra::kDefaultChatRegion).size() == 1);
	});

	Test("game_profiles_round_trip_and_match", [] {
		const QString path = QFileInfo(NewDb()).absoluteDir().filePath("game-profiles.json");
		GameProfiles missing;
		CHECK(missing.Load(path) && missing.profiles.empty() && missing.AnyExecutable().isEmpty());

		LucidaProfile l;
		l.carnivore = true;
		l.onlyDrawn = true;
		l.readHud = false;
		l.chatRegion = {0.01, 0.02, 0.4, 0.3};
		l.regions = {{"kill feed", {0.8, 0.0, 1.0, 0.15}, RegionMode::Read},
			     {"minimap", {0.0, 0.8, 0.25, 1.0}, RegionMode::Ignore}};
		GameProfile fivem;
		fivem.name = "FiveM";
		fivem.executable = "FiveM.*GTAProcess\\.exe";
		fivem.SetLucida(l);
		/* another feature's section is kept as it is */
		fivem.sections.insert("loop", QJsonObject{{"quotaGb", 100}});
		GameProfile other;
		other.name = "No pattern";
		GameProfiles saved;
		saved.profiles = {fivem, other};
		CHECK(saved.Save(path));

		GameProfiles loaded;
		CHECK(loaded.Load(path) && loaded.profiles.size() == 2);
		const GameProfile *p = loaded.Match("FiveM_b3095_GTAProcess.exe");
		CHECK(p && p->name == "FiveM");
		CHECK(loaded.Match("fivem_b3095_gtaprocess.EXE") == p);
		CHECK(!loaded.Match("notepad.exe"));
		CHECK(!other.Matches("anything.exe"));
		CHECK(p && p->sections.value("loop").toObject().value("quotaGb").toInt() == 100);
		auto back = p ? p->Lucida() : std::nullopt;
		CHECK(back && back->carnivore && back->onlyDrawn && !back->readHud);
		CHECK(back && back->regions == l.regions && back->chatRegion.right == 0.4);
		CHECK(!other.Lucida());
		CHECK(loaded.AnyExecutable() == "(?:FiveM.*GTAProcess\\.exe)");
		CHECK(ModeFromKey(ModeKey(RegionMode::Ignore)) == RegionMode::Ignore &&
		      ModeFromKey("nonsense") == RegionMode::Read);
	});

	Test("the_recorder_reads_each_game_with_its_profile", [] {
		const auto screen = Join({LeftLines(kChat, 10, 20), RightLines(kFeed, 1900, 20)});
		Store store(NewDb());
		CHECK(store.Open());
		BoxOcr ocr({screen});
		const spectra::Image img = DrawBoxes(screen);
		std::vector<GrabbedFrame> frames{{img, "Game", "FiveM_GTAProcess.exe"},
						 {DrawBoxes(RightLines(kFeed, 1900, 40)), "Game", "notepad.exe"}};
		size_t next = 0;
		RecorderConfig cfg; /* the settings: chat box mode */
		cfg.readHud = false;
		Recorder recorder(cfg, store, ocr, [&]() -> std::optional<GrabbedFrame> {
			if (next >= frames.size()) {
				return std::nullopt;
			}
			return frames[next++];
		});
		LucidaProfile l;
		l.carnivore = true;
		l.readHud = false;
		l.regions = {{"kill feed", {0.8, 0.0, 1.0, 0.15}, RegionMode::Read}};
		GameProfile fivem;
		fivem.name = "FiveM";
		fivem.executable = "GTAProcess";
		fivem.SetLucida(l);
		recorder.profileFor = [&](const QString &exe) -> std::optional<GameProfile> {
			return fivem.Matches(exe) ? std::optional<GameProfile>(fivem) : std::nullopt;
		};
		Tick a = recorder.Step();
		CHECK(a.profile == "FiveM" && recorder.Config().carnivore && a.regions == 2);
		Query q;
		q.region = "kill feed";
		CHECK(Bodies(store.Find(q)) == kFeed);
		q.region = "chat";
		CHECK(store.Find(q).size() == 3);
		/* another game: back to the settings */
		recorder.Step();
		CHECK(!recorder.Config().carnivore && recorder.Config().regions.empty());
	});

	Test("the_chat_box_mode_leaves_region_empty", [] {
		Rig rig({kChat});
		rig.recorder->Step();
		auto lines = rig.store.Recent();
		CHECK(lines.size() == 3 && lines[0].region.isEmpty());
		CHECK(rig.store.RegionNames().isEmpty() && rig.store.ScreenRegions().empty());
	});

	/* ---- Spectra: backing up to Prisma ---- */

	Test("prisma_signs_its_token_request", [] {
		auto seed = Ed25519SeedFromPem(QString::fromLatin1(kTestPem));
		CHECK(seed.has_value());
		CHECK(!Ed25519SeedFromPem("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----"));
		const std::array<uint8_t, 32> pub = Ed25519PublicKey(*seed);
		CHECK(QByteArray(reinterpret_cast<const char *>(pub.data()), 32).toHex() == kTestPublic);

		const PrismaCredentials c = TestCredentials();
		CHECK(c.url == "https://prisma.test" && c.clientId == "c_desktop");
		CHECK(!PrismaCredentials::FromJson("{\"url\": \"x\"}"));

		const QByteArray jwt = BuildAssertion(c, 1789000000, 120, "nonce");
		const QList<QByteArray> parts = jwt.split('.');
		CHECK(parts.size() == 3);
		if (parts.size() == 3) {
			auto decode = [](const QByteArray &b) {
				return QByteArray::fromBase64(b, QByteArray::Base64UrlEncoding |
									 QByteArray::OmitTrailingEquals);
			};
			const QJsonObject header = QJsonDocument::fromJson(decode(parts[0])).object();
			const QJsonObject claims = QJsonDocument::fromJson(decode(parts[1])).object();
			CHECK(header.value("alg") == "EdDSA" && header.value("kid") == "k1");
			CHECK(claims.value("iss") == "c_desktop" && claims.value("sub") == "c_desktop");
			CHECK(claims.value("aud") == "prisma:test" && claims.value("jti") == "nonce");
			CHECK(claims.value("iat").toInteger() == 1789000000 &&
			      claims.value("exp").toInteger() == 1789000120);
			const QByteArray input = parts[0] + '.' + parts[1];
			const QByteArray sig = decode(parts[2]);
			CHECK(sig.size() == 64 &&
			      crypto_ed25519_check(reinterpret_cast<const uint8_t *>(sig.constData()), pub.data(),
						   reinterpret_cast<const uint8_t *>(input.constData()),
						   input.size()) == 0);
		}
		/* a fresh jti each time */
		CHECK(BuildAssertion(c, 1789000000).split('.')[1] != BuildAssertion(c, 1789000000).split('.')[1]);

		const QString dir = QFileInfo(NewDb()).absolutePath();
		CHECK(FindCredentials({dir}).isEmpty());
		QFile f(QDir(dir).filePath("prisma-c_desktop.json"));
		CHECK(f.open(QIODevice::WriteOnly));
		f.write("{}");
		f.close();
		CHECK(FindCredentials({QString(), dir}).endsWith("prisma-c_desktop.json"));
	});

	Test("prisma_client_keeps_and_renews_its_token", [] {
		FakePrisma fake;
		PrismaClient client(TestCredentials(), [&](const HttpRequest &r) { return fake(r); });
		double now = 1789000000;
		client.clock = [&] {
			return now;
		};
		CHECK(client.Put("/v1/lucida/sources/me", QJsonObject()).Ok());
		CHECK(client.Put("/v1/lucida/sources/me", QJsonObject()).Ok());
		CHECK(fake.tokens == 1);
		now += 900 - 30; /* inside the last minute: renewed */
		CHECK(client.Put("/v1/lucida/sources/me", QJsonObject()).Ok() && fake.tokens == 2);
		/* a revoked token is replaced once */
		FakePrisma revoked;
		revoked.rejectToken = 5;
		PrismaClient again(TestCredentials(), [&](const HttpRequest &r) { return revoked(r); });
		CHECK(again.Put("/v1/lucida/sources/me", QJsonObject()).Ok() && revoked.tokens == 2);
		/* failures say why */
		fake.failNext = 1;
		const PrismaResult busy = client.Put("/v1/lucida/sources/me", QJsonObject());
		CHECK(busy.status == 503 && busy.Describe() == "HTTP 503: busy");
		/* the other error shapes: a 422's issues, OAuth's and API Gateway's */
		auto describe = [](int status, const char *json) {
			PrismaResult r;
			r.status = status;
			r.body = QJsonDocument::fromJson(json).object();
			return r.Describe();
		};
		CHECK(describe(422, R"({"detail": [{"loc": ["body", "lines", 7, "tags"], "msg": "too many"}]})") ==
		      "HTTP 422: too many (body.lines.7.tags)");
		CHECK(describe(401, R"({"error": "invalid_client", "error_description": "unknown kid"})") ==
		      "HTTP 401: unknown kid");
		CHECK(describe(429, R"({"message": "Too Many Requests"})") == "HTTP 429: Too Many Requests");
		CHECK(describe(500, "{}") == "HTTP 500");
		PrismaClient down(TestCredentials(),
				  [](const HttpRequest &) { return HttpResponse{0, {}, "unreachable"}; });
		CHECK(down.Get("/v1/lucida/sources").Describe() == "unreachable");
	});

	Test("cloud_sync_backs_up_lines_sessions_and_screenshots_once", [] {
		Store s(NewDb());
		CHECK(s.Open());
		Tagger t({{"bodycam-rp", {"bodycam"}}}, false);
		s.labeler = [&](const QString &body) {
			return t.Tags(body);
		};
		const long long session = s.StartSession("FiveM", 1920, 1080);
		spectra::ChatEntry a = Entry("16:38:10", "Bob says: bodycam on", "say", 0.6f);
		a.region = "chat";
		a.rects = {{10, 20, 300, 40}};
		auto ids = s.AddFrame({a, Entry("16:38:11", "Harry says: hi")}, 1789231300, "hud", session);
		const QString shot = QDir(QFileInfo(s.Path()).absolutePath()).filePath("1789231300.jpg");
		{
			QFile f(shot);
			CHECK(f.open(QIODevice::WriteOnly));
			f.write("\xff\xd8 jpeg");
		}
		s.AttachFrame(Seen(ids), shot, 1920, 1080, 1789231300, session);
		s.AttachFrame({}, QDir(QFileInfo(s.Path()).absolutePath()).filePath("gone.jpg"), 1920, 1080, 1789231301,
			      session);

		FakePrisma fake;
		PrismaClient client(TestCredentials(), [&](const HttpRequest &r) { return fake(r); });
		CloudSync sync(s, client);
		/* a frame record that vanished before the upload was confirmed is sent again */
		fake.lostRecords = 1;
		SyncReport r = sync.Step();
		CHECK(!r.error.isEmpty() && r.sessions == 1 && r.frames == 0 && r.refused == 0 && r.more);
		r = sync.Step();
		CHECK(r.error.isEmpty() && r.lines == 2 && r.sessions == 0 && r.frames == 1 && r.refused == 1 &&
		      !r.more);
		/* screenshots go before the lines that point at them */
		CHECK(fake.calls.indexOf(QRegularExpression(".*/frames/1789231300$")) <
		      fake.calls.indexOf(QRegularExpression(".*/lines$")));
		CHECK(fake.images["1789231300"] == "\xff\xd8 jpeg");
		CHECK(fake.frames["1789231300"].value("session_key").toString().size() >= 10);
		const QJsonObject line = fake.lines[QString::number(ids[0])];
		CHECK(line.value("body") == "Bob says: bodycam on" && line.value("clock") == "16:38:10");
		CHECK(line.value("frame_key") == "1789231300" && line.value("region") == "chat");
		CHECK(line.value("labels").toArray() == QJsonArray{"bodycam-rp"});
		CHECK(line.value("rect").toArray() == (QJsonArray{10, 20, 300, 40}));
		CHECK(line.value("session_key") == fake.frames["1789231300"].value("session_key"));
		CHECK(line.value("ts_source") == "hud" && !line.value("dedup_key").toString().isEmpty());

		/* nothing changed: nothing sent */
		fake.calls.clear();
		r = sync.Step();
		CHECK(r.lines == 0 && r.frames == 0 && r.sessions == 0 && fake.calls.isEmpty());
		/* a better reading and an ended session are sent again; another sighting is not */
		s.AddFrame({Entry("16:38:10", "Bob says: bodycam on!", "say", 0.99f),
			    Entry("16:38:11", "Harry says: hi")},
			   1789231305, "hud", session);
		s.EndSession(session);
		r = sync.Step();
		CHECK(r.lines == 1 && r.sessions == 1);
		CHECK(fake.sessions.begin()->second.contains("ended"));

		/* a line Prisma refuses does not hold up the rest */
		s.AddFrame({Entry("16:39:00", "REFUSE me"), Entry("16:39:01", "fine")}, 1789231400, "hud", session);
		r = sync.Step();
		CHECK(r.lines == 1 && r.refused == 1 && !r.more);
		/* an outage stops the pass and leaves everything waiting */
		s.AddFrame({Entry("16:40:00", "later")}, 1789231500, "hud", session);
		fake.failNext = 1;
		PrismaClient fresh(TestCredentials(), [&](const HttpRequest &req) { return fake(req); });
		CloudSync afterRestart(s, fresh);
		r = afterRestart.Step();
		CHECK(!r.error.isEmpty() && r.lines == 0 && r.more);
		r = afterRestart.Step();
		CHECK(r.error.isEmpty() && r.lines == 1 && s.Unsynced().lines == 0);

		/* another gateway or client: everything is sent again */
		PrismaCredentials other = TestCredentials("https://elsewhere.test");
		fake.plain422 = true; /* the one-at-a-time fallback */
		PrismaClient moved(other, [&](const HttpRequest &req) { return fake(req); });
		CloudSync movedSync(s, moved);
		r = movedSync.Step(10, 10);
		CHECK(r.lines == 4 && r.frames == 1 && r.refused == 2);
	});

	Test("cloud_sync_against_a_real_prisma", [] {
		/* LUCIDA_PRISMA_CREDENTIALS=<a credentials file> (e.g. a local Prisma) */
		const QString path = qEnvironmentVariable("LUCIDA_PRISMA_CREDENTIALS");
		if (path.isEmpty()) {
			printf("     (skipped: LUCIDA_PRISMA_CREDENTIALS is not set)\n");
			return;
		}
		QString error;
		auto creds = PrismaCredentials::FromFile(path, &error);
		CHECK(creds.has_value());
		if (!creds) {
			printf("     %s\n", qPrintable(error));
			return;
		}
		Store s(NewDb());
		CHECK(s.Open());
		const long long session = s.StartSession("FiveM", 1920, 1080);
		const long long ts = NowSecs();
		spectra::ChatEntry a = Entry("16:38:10", "Pier 400 heist tonight, bring bodycams");
		a.region = "chat";
		a.rects = {{10, 20, 300, 40}};
		auto ids = s.AddFrame({a, Entry("16:38:11", "Harry says: helo wrld")}, ts, "hud", session);
		const QString shot = QDir(QFileInfo(s.Path()).absolutePath()).filePath(QString::number(ts) + ".jpg");
		QImage red(64, 32, QImage::Format_RGB888);
		red.fill(Qt::red);
		CHECK(red.save(shot, "JPG"));
		s.AttachFrame(Seen(ids), shot, 64, 32, ts, session);

		PrismaClient client(*creds);
		CloudSync sync(s, client);
		sync.machine = "lucida-tests";
		SyncReport r = sync.Step();
		printf("     sent %d line(s), %d screenshot(s), %d session(s); %s\n", r.lines, r.frames, r.sessions,
		       r.error.isEmpty() ? "no error" : qPrintable(r.error));
		CHECK(r.error.isEmpty() && r.lines == 2 && r.frames == 1 && r.sessions == 1 && r.refused == 0);

		PrismaResult lines =
			client.Get("/v1/lucida/sources/me/lines", QUrlQuery{{"from_ts", QString::number(ts)}});
		CHECK(lines.Ok() && lines.body.toObject().value("lines").toArray().size() == 2);
		PrismaResult frame = client.Get("/v1/lucida/sources/me/frames/" + QString::number(ts));
		CHECK(frame.Ok() && frame.body.toObject().value("uploaded").toBool());
		const HttpResponse image = client.Fetch(frame.body.toObject().value("url").toString());
		QFile original(shot);
		CHECK(original.open(QIODevice::ReadOnly) && image.status == 200 && image.body == original.readAll());
	});

	/* ---- Spectra: loop recording lookup ---- */

	Test("a_moment_is_found_in_its_loop_segment", [] {
		QString dir = QDir(scratch).filePath("loop");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);
		const QDateTime base = QDateTime::currentDateTime().addSecs(-600);
		auto name = [&](int offset) {
			return QDir(dir).filePath(base.addSecs(offset).toString("yyyy-MM-dd HH-mm-ss") + ".mkv");
		};
		for (int offset : {0, 120, 240}) {
			QFile f(name(offset));
			CHECK(f.open(QIODevice::WriteOnly));
			f.write("x");
		}
		const double t0 = base.toSecsSinceEpoch();
		auto spot = LocateVideo(dir, t0 + 130, false);
		CHECK(spot && spot->path.endsWith(QFileInfo(name(120)).fileName()) &&
		      std::fabs(spot->offset - 10) < 1.1);
		CHECK(!LocateVideo(dir, t0 - 60, false));
		/* the newest segment: open-ended while recording, else until last written */
		CHECK(LocateVideo(dir, t0 + 900, true).has_value());
		CHECK(!LocateVideo(dir, t0 + 3600, false));
		CHECK(FormatOffset(83) == "1:23" && FormatOffset(3723) == "1:02:03");
	});

	/* ---- sharing on the local network ---- */

	Test("lan_keys_are_kept_protected_and_shown_short", [] {
		const QString path = QDir(QFileInfo(NewDb()).absolutePath()).filePath("lan-identity.json");
		QString error;
		auto made = lan::Identity::LoadOrCreate(path, &error);
		CHECK(made.has_value());
		if (!made) {
			return;
		}
		auto again = lan::Identity::LoadOrCreate(path, &error);
		CHECK(again && again->pub == made->pub && again->secret == made->secret);
		QFile f(path);
		CHECK(f.open(QIODevice::ReadOnly));
		const QByteArray stored = f.readAll();
		/* the secret is not in the file as it is */
		CHECK(!stored.contains(QByteArray((const char *)made->secret.data(), 32).toBase64()));
		CHECK(made->Id().size() == 43 && lan::KeyFromId(made->Id()) == made->pub);
		CHECK(!lan::KeyFromId("not a key").has_value());
		const QString fp = lan::Fingerprint(made->pub);
		CHECK(fp.size() == 19 && fp[4] == '-' && fp[9] == '-' && fp[14] == '-');
		CHECK(lan::FormatPairCode(42) == "000 042");
	});

	Test("lan_channel_proves_both_keys_and_seals_messages", [] {
		const lan::Identity server = lan::Identity::Generate(), client = lan::Identity::Generate();
		/* the server side runs on its own thread with its own listener */
		std::promise<quint16> listening;
		auto served = std::async(
			std::launch::async,
			[&](lan::Identity key, bool genuine) {
				QTcpServer listener;
				listener.listen(QHostAddress::LocalHost, 0);
				listening.set_value(listener.serverPort());
				if (!listener.waitForNewConnection(5000)) {
					return QString("no connection");
				}
				QTcpSocket *s = listener.nextPendingConnection();
				std::unique_ptr<lan::Channel> ch =
					lan::Channel::Accept(*s, genuine ? key : lan::Identity::Generate(), 3000);
				if (!genuine) {
					return QString();
				}
				if (!ch || ch->PeerId() != client.Id()) {
					return QString("bad handshake");
				}
				auto m = ch->ReceiveJson(5000);
				ch->SendJson(QJsonObject{{"echo", m ? m->value("say").toString() : QString()}});
				ch->Send(QByteArray(lan::Channel::kMaxMessage, 'x'));
				s->waitForBytesWritten(5000);
				s->disconnectFromHost();
				return QString();
			},
			server, true);
		QTcpSocket socket;
		socket.connectToHost(QHostAddress::LocalHost, listening.get_future().get());
		CHECK(socket.waitForConnected(5000));
		auto ch = lan::Channel::Connect(socket, client, server.pub, 5000);
		CHECK(ch && ch->PeerId() == server.Id());
		if (ch) {
			CHECK(ch->SendJson(QJsonObject{{"say", "hello"}}));
			auto reply = ch->ReceiveJson(5000);
			CHECK(reply && reply->value("echo").toString() == "hello");
			auto big = ch->Receive(5000);
			CHECK(big && big->size() == lan::Channel::kMaxMessage);
		}
		CHECK(served.get().isEmpty());

		/* a client expecting another key refuses whoever answers */
		std::promise<quint16> listening2;
		auto impostor = std::async(std::launch::async, [&] {
			QTcpServer listener;
			listener.listen(QHostAddress::LocalHost, 0);
			listening2.set_value(listener.serverPort());
			if (listener.waitForNewConnection(5000)) {
				QTcpSocket *s = listener.nextPendingConnection();
				lan::Channel::Accept(*s, lan::Identity::Generate(), 2000);
			}
		});
		QTcpSocket socket2;
		socket2.connectToHost(QHostAddress::LocalHost, listening2.get_future().get());
		CHECK(socket2.waitForConnected(5000));
		QString error;
		CHECK(!lan::Channel::Connect(socket2, client, server.pub, 2000, &error));
		CHECK(error.contains("different install"));
		socket2.abort();
		impostor.wait();
	});

	Test("lan_installs_find_each_other_pair_and_share_the_log_and_clips", [] {
		const QString dbA = NewDb(), dbB = NewDb();
		const QString dirB = QFileInfo(dbB).absolutePath();
		const QString clipsB = QDir(dirB).filePath("clips");
		QDir().mkpath(clipsB);
		{
			QFile clip(QDir(clipsB).filePath("Big Moment.mp4"));
			CHECK(clip.open(QIODevice::WriteOnly));
			QByteArray data(3 * lan::kChunk + 1234, '\0');
			for (int i = 0; i < data.size(); i++) {
				data[i] = char(i * 7);
			}
			clip.write(data);
			QFile notes(QDir(clipsB).filePath("notes.txt"));
			CHECK(notes.open(QIODevice::WriteOnly));
			notes.write("secret");
		}
		/* B's log: two lines on one screenshot */
		const QString shotPath = QDir(dirB).filePath("shot.jpg");
		{
			QImage img(64, 36, QImage::Format_RGB32);
			img.fill(Qt::darkBlue);
			CHECK(img.save(shotPath));
			Store s(dbB);
			s.Open();
			std::vector<Sighting> seen;
			s.AddFrame({At(Entry("16:38:10", "meet at the docks"), 100),
				    At(Entry("16:38:11", "on my way"), 120)},
				   1789231300, "wall", {}, &seen);
			s.AttachFrame(seen, shotPath, 64, 36, 1789231300);
		}
		Store(dbA).Open();

		std::atomic<int> askedB{0};
		std::atomic<bool> acceptB{true};
		QString codeA, codeB;
		std::mutex codes;
		lan::Provider providerA{[dbA] { return dbA; },
					[] { return QString(); },
					[] { return QJsonObject(); },
					{}};
		lan::Provider providerB{
			[dbB] { return dbB; }, [clipsB] { return clipsB; },
			[] { return QJsonObject{{"game", "FiveM_GTAProcess.exe"}, {"recording", true}}; },
			[&](const lan::PairPrompt &p) {
				askedB++;
				{
					std::lock_guard<std::mutex> lock(codes);
					codeB = p.code;
				}
				std::promise<bool> answer;
				answer.set_value(acceptB.load());
				return answer.get_future();
			}};
		const QString dir = QFileInfo(dbA).absolutePath();
		lan::Service a(lan::Identity::Generate(), QDir(dir).filePath("a-peers.json"), providerA);
		lan::Service b(lan::Identity::Generate(), QDir(dir).filePath("b-peers.json"), providerB);
		for (lan::Service *svc : {&a, &b}) {
			svc->broadcast = false;
			svc->beaconMs = 100;
			svc->statusMs = 300;
			svc->expireMs = 1500;
		}
		a.extraTargets = {{QHostAddress(QHostAddress::LocalHost), 47702}};
		b.extraTargets = {{QHostAddress(QHostAddress::LocalHost), 47701}};
		QString error;
		CHECK(a.Start("Alpha PC", &error, 0, 47701));
		CHECK(b.Start("Bravo\nPC", &error, 0, 47702));

		auto waitFor = [](const std::function<bool()> &cond) {
			for (int i = 0; i < 100 && !cond(); i++) {
				QThread::msleep(50);
			}
			return cond();
		};
		CHECK(waitFor([&] { return a.FindPeer(b.Self().Id()).has_value(); }));
		std::optional<lan::Peer> peerB = a.FindPeer(b.Self().Id());
		CHECK(peerB && peerB->name == "BravoPC" && !peerB->paired && peerB->port == b.Port());
		if (!peerB) {
			return;
		}

		/* nothing but hello before pairing */
		lan::Result r = a.Request(*peerB, QJsonObject{{"op", "lines"}});
		CHECK(!r.ok && r.notPaired);
		r = a.Request(*peerB, QJsonObject{{"op", "hello"}});
		CHECK(r.ok && r.body.value("name").toString() == "BravoPC" && !r.body.contains("game"));

		/* B says no */
		acceptB = false;
		r = a.Pair(*peerB, [&](const QString &code) {
			std::lock_guard<std::mutex> lock(codes);
			codeA = code;
			return true;
		});
		CHECK(!r.ok && !a.IsPaired(b.Self().Id()) && !b.IsPaired(a.Self().Id()));
		CHECK(codeA.size() == 7 && codeA == codeB);

		/* A's person sees different codes and cancels */
		acceptB = true;
		r = a.Pair(*peerB, [](const QString &) { return false; });
		CHECK(!r.ok && !b.IsPaired(a.Self().Id()));

		/* both say yes */
		r = a.Pair(*peerB, [&](const QString &code) {
			std::lock_guard<std::mutex> lock(codes);
			codeA = code;
			return true;
		});
		CHECK(r.ok && a.IsPaired(b.Self().Id()));
		CHECK(waitFor([&] { return b.IsPaired(a.Self().Id()); }));
		CHECK(codeA == codeB && askedB == 3);
		CHECK(a.Paired().size() == 1 && a.Paired()[0].name == "BravoPC");

		/* what B reports about itself */
		CHECK(waitFor([&] {
			auto p = a.FindPeer(b.Self().Id());
			return p && p->reachable && p->game == "FiveM_GTAProcess.exe";
		}));
		peerB = a.FindPeer(b.Self().Id());
		CHECK(peerB && peerB->recording && peerB->pairedThere && peerB->paired);
		if (!peerB) {
			return;
		}

		/* B's log */
		r = a.Request(*peerB, QJsonObject{{"op", "lines"}, {"text", "docks"}});
		QJsonArray lines = r.body.value("lines").toArray();
		CHECK(r.ok && lines.size() == 1 &&
		      lines[0].toObject().value("body").toString() == "meet at the docks" &&
		      lines[0].toObject().value("source").toString() == b.Self().Id() &&
		      lines[0].toObject().value("has_frame").toBool());
		r = a.Request(*peerB, QJsonObject{{"op", "lines"}});
		CHECK(r.ok && r.body.value("lines").toArray().size() == 2);

		/* a line's screenshot, with where the line is on it */
		QByteArray shot;
		r = a.Request(*peerB, QJsonObject{{"op", "shot"}, {"line", lines[0].toObject().value("line_id")}},
			      [&](const QByteArray &chunk) {
				      shot += chunk;
				      return true;
			      });
		QFile shotFile(shotPath);
		CHECK(shotFile.open(QIODevice::ReadOnly));
		CHECK(r.ok && shot == shotFile.readAll());
		CHECK(r.body.value("rect").toArray().size() == 4 && r.body.value("rect").toArray()[1].toInt() == 100);
		r = a.Request(*peerB, QJsonObject{{"op", "shot"}, {"line", "999"}});
		CHECK(!r.ok && r.error.contains("screenshot"));

		/* B's clips: only the videos, downloadable from anywhere in the file */
		r = a.Request(*peerB, QJsonObject{{"op", "clips"}});
		QJsonArray clips = r.body.value("clips").toArray();
		CHECK(r.ok && clips.size() == 1 && clips[0].toObject().value("name").toString() == "Big Moment.mp4" &&
		      clips[0].toObject().value("size").toInteger() == 3 * lan::kChunk + 1234);
		QFile original(QDir(clipsB).filePath("Big Moment.mp4"));
		CHECK(original.open(QIODevice::ReadOnly));
		const QByteArray whole = original.readAll();
		QByteArray got;
		r = a.Request(*peerB, QJsonObject{{"op", "clip"}, {"name", "Big Moment.mp4"}, {"offset", 1000}},
			      [&](const QByteArray &chunk) {
				      got += chunk;
				      return true;
			      });
		CHECK(r.ok && got == whole.mid(1000) && r.body.value("size").toInteger() == whole.size());
		/* stopping part way */
		int chunks = 0;
		r = a.Request(*peerB, QJsonObject{{"op", "clip"}, {"name", "Big Moment.mp4"}},
			      [&](const QByteArray &) { return ++chunks < 2; });
		CHECK(!r.ok && chunks == 2);
		for (const char *name :
		     {"notes.txt", "../log.db", "..\\log.db", "C:log.db", "", ".hidden.mp4", "missing.mp4"}) {
			r = a.Request(*peerB, QJsonObject{{"op", "clip"}, {"name", name}});
			CHECK(!r.ok && r.error.contains("clip"));
		}
		CHECK(!lan::ClipPath(clipsB, "../clips/Big Moment.mp4"));
		CHECK(lan::ClipPath(clipsB, "Big Moment.mp4").has_value());

		/* B forgets A: A is told it has to pair again */
		b.Forget(a.Self().Id());
		r = a.Request(*peerB, QJsonObject{{"op", "lines"}});
		CHECK(!r.ok && r.notPaired);
		CHECK(waitFor([&] {
			auto p = a.FindPeer(b.Self().Id());
			return p && !p->pairedThere;
		}));

		/* pairings are kept */
		{
			lan::Service again(lan::Identity::Generate(), QDir(dir).filePath("a-peers.json"), providerA);
			CHECK(again.IsPaired(b.Self().Id()));
		}

		/* B leaving is noticed at once */
		b.Stop();
		CHECK(waitFor([&] { return !a.FindPeer(b.Self().Id()).has_value(); }));
		a.Stop();
		CHECK(a.Peers().empty());
	});

	auto waitUntil = [](const std::function<bool()> &cond) {
		for (int i = 0; i < 100 && !cond(); i++) {
			QThread::msleep(50);
		}
		return cond();
	};
	auto quick = [](lan::Service &svc) {
		svc.broadcast = false;
		svc.beaconMs = 100;
		svc.statusMs = 300;
		svc.expireMs = 1500;
	};
	auto yes = [](const lan::PairPrompt &) {
		std::promise<bool> answer;
		answer.set_value(true);
		return answer.get_future();
	};

	Test("lan_installs_broadcasts_miss_still_find_each_other", [&] {
		const QString dir = QFileInfo(NewDb()).absolutePath();
		lan::Provider provider{{}, {}, {}, yes};
		lan::Service a(lan::Identity::Generate(), QDir(dir).filePath("a.json"), provider);
		lan::Service b(lan::Identity::Generate(), QDir(dir).filePath("b.json"), provider);
		quick(a);
		quick(b);
		QString error;
		CHECK(a.Start("Alpha", &error, 0, 47711));
		CHECK(b.Start("Bravo", &error, 0, 47712));

		/* by address: each lists the other, though no beacon goes anywhere */
		lan::Result r = a.AddAddress("127.0.0.1", b.Port());
		CHECK(r.ok && r.body.value("id").toString() == b.Self().Id() &&
		      r.body.value("name").toString() == "Bravo");
		CHECK(a.FindPeer(b.Self().Id()).has_value());
		CHECK(waitUntil([&] { return b.FindPeer(a.Self().Id()).has_value(); }));
		std::optional<lan::Peer> onB = b.FindPeer(a.Self().Id());
		CHECK(onB && onB->name == "Alpha" && onB->port == a.Port());
		CHECK(!a.AddAddress("127.0.0.1", a.Port()).ok);
		CHECK(!a.AddAddress("127.0.0.1", 1).ok);
		CHECK(!a.AddAddress("no-such-pc.invalid", b.Port()).ok);

		/* paired PCs remember where the other one is */
		std::optional<lan::Peer> peerB = a.FindPeer(b.Self().Id());
		lan::Result paired = peerB ? a.Pair(*peerB, [](const QString &) { return true; }) : lan::Result();
		if (!paired.ok) {
			fprintf(stderr, "  pairing failed: %s\n", paired.error.toUtf8().constData());
		}
		CHECK(paired.ok);
		CHECK(a.Paired().size() == 1 && a.Paired()[0].port == b.Port() && !a.Paired()[0].address.isNull());
		{
			lan::Service again(lan::Identity::Generate(), QDir(dir).filePath("a.json"), provider);
			CHECK(again.Paired().size() == 1 && again.Paired()[0].port == b.Port());
		}
		/* and it stays listed while it answers, beacons or not */
		QThread::msleep(2000);
		CHECK(a.FindPeer(b.Self().Id()).has_value());
		a.Stop();
		b.Stop();

		/* one way only: C's beacons reach D, D's reach nobody; D answers C */
		lan::Service c(lan::Identity::Generate(), QDir(dir).filePath("c.json"), provider);
		lan::Service d(lan::Identity::Generate(), QDir(dir).filePath("d.json"), provider);
		quick(c);
		quick(d);
		c.extraTargets = {{QHostAddress(QHostAddress::LocalHost), 47714}};
		CHECK(c.Start("Charlie", &error, 0, 47713));
		CHECK(d.Start("Delta", &error, 0, 47714));
		CHECK(waitUntil([&] { return d.FindPeer(c.Self().Id()).has_value(); }));
		CHECK(waitUntil([&] { return c.FindPeer(d.Self().Id()).has_value(); }));
		c.Stop();
		d.Stop();
		CHECK(!lan::Service::LocalAddresses().contains("127.0.0.1"));
	});

	Test("lan_a_paired_pc_that_moved_is_found_through_the_others", [&] {
		const QString dir = QFileInfo(NewDb()).absolutePath();
		lan::Provider provider{{}, {}, {}, yes};
		const lan::Identity idG = lan::Identity::Generate();
		lan::Service e(lan::Identity::Generate(), QDir(dir).filePath("e.json"), provider);
		lan::Service f(lan::Identity::Generate(), QDir(dir).filePath("f.json"), provider);
		auto g = std::make_unique<lan::Service>(idG, QDir(dir).filePath("g.json"), provider);
		for (lan::Service *svc : {&e, &f, g.get()}) {
			quick(*svc);
		}
		QString error;
		CHECK(e.Start("Echo", &error, 0, 47731));
		CHECK(f.Start("Foxtrot", &error, 0, 47732));
		CHECK(g->Start("Golf", &error, 0, 47733));
		auto pair = [](lan::Service &from, lan::Service &to) {
			if (!from.AddAddress("127.0.0.1", to.Port()).ok) {
				return false;
			}
			std::optional<lan::Peer> p = from.FindPeer(to.Self().Id());
			return p && from.Pair(*p, [](const QString &) { return true; }).ok;
		};
		CHECK(pair(e, f));
		CHECK(pair(*g, e));
		CHECK(pair(*g, f));
		const quint16 oldPort = g->Port();

		/* Golf comes back somewhere else and only tells Foxtrot */
		g->Stop();
		g = std::make_unique<lan::Service>(idG, QDir(dir).filePath("g.json"), provider);
		quick(*g);
		g->Forget(e.Self().Id());
		CHECK(g->Start("Golf", &error, 0, 47734));
		CHECK(g->Port() != oldPort);
		auto at = [&](lan::Service &svc) {
			std::optional<lan::Peer> p = svc.FindPeer(idG.Id());
			return p ? p->port : quint16(0);
		};
		bool found = false;
		for (int i = 0; i < 200 && !found; i++) {
			found = at(f) == g->Port() && at(e) == g->Port();
			QThread::msleep(50);
		}
		CHECK(at(f) == g->Port());
		CHECK(at(e) == g->Port()); /* from Foxtrot's gossip */
		CHECK(e.Paired().size() == 2);
		for (lan::Service *svc : {&e, &f, g.get()}) {
			svc->Stop();
		}
	});

	Test("lan_storage_pc_keeps_segments_and_the_log", [&] {
		CHECK(lan::IsSegmentName("2026-09-20 10-00-00.mkv"));
		CHECK(!lan::IsSegmentName("../2026-09-20 10-00-00.mkv"));
		CHECK(!lan::IsSegmentName("notes.mkv"));
		CHECK(!lan::IsSegmentName("2026-09-20 10-00-00.mp4"));

		const QString dbS = NewDb();
		const QString dir = QFileInfo(dbS).absolutePath();
		const QString loop = QDir(dir).filePath("loop");
		const QString storage = QDir(dir).filePath("storage");
		QDir().mkpath(loop);
		constexpr int kSegment = 300 * 1024;
		auto segment = [&](const QString &name, char fill) {
			QFile f(QDir(loop).filePath(name));
			CHECK(f.open(QIODevice::WriteOnly));
			QByteArray data(kSegment, fill);
			for (int i = 0; i < data.size(); i += 997) {
				data[i] = char(i);
			}
			f.write(data);
			return f.fileName();
		};
		const QString seg1 = segment("2026-09-20 10-00-00.mkv", 'a');
		const QString seg2 = segment("2026-09-20 10-02-00.mkv", 'b');
		const QString seg3 = segment("2026-09-20 10-04-00.mkv", 'c');
		auto same = [](const QString &x, const QString &y) {
			QFile a(x), b(y);
			return a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && a.readAll() == b.readAll();
		};

		/* the source's log: a session, two lines on a screenshot */
		const QString shot = QDir(dir).filePath("1789231300.jpg");
		{
			QImage img(64, 36, QImage::Format_RGB32);
			img.fill(Qt::darkGreen);
			CHECK(img.save(shot));
		}
		Store s(dbS);
		CHECK(s.Open());
		const long long session = s.StartSession("FiveM_GTAProcess.exe", 1920, 1080);
		std::vector<Sighting> seen;
		const std::vector<long long> ids = s.AddFrame({At(Entry("16:38:10", "meet at the docks"), 100),
							       At(Entry("16:38:11", "on my way"), 120)},
							      1789231300, "wall", session, &seen);
		s.AttachFrame(seen, shot, 64, 36, 1789231300, session);
		CHECK(ids.size() == 2);

		lan::StorageNode node(storage, 700 * 1024); /* room for two segments */
		std::atomic<int> arrived{0};
		node.changed = [&] {
			arrived++;
		};
		lan::Provider nodeProvider{{},
					   {},
					   [] { return QJsonObject{{"storage", true}}; },
					   yes,
					   [&](lan::Channel &ch, const QString &id, const QString &name,
					       const QJsonObject &req) { node.Answer(ch, id, name, req); }};
		lan::Provider plain{{}, {}, {}, yes};
		lan::Service n(lan::Identity::Generate(), QDir(dir).filePath("n.json"), nodeProvider);
		lan::Service src(lan::Identity::Generate(), QDir(dir).filePath("s.json"), plain);
		lan::Service stranger(lan::Identity::Generate(), QDir(dir).filePath("x.json"), plain);
		for (lan::Service *svc : {&n, &src, &stranger}) {
			quick(*svc);
		}
		QString error;
		CHECK(n.Start("Storage", &error, 0, 47721));
		CHECK(src.Start("Gamer", &error, 0, 47722));
		CHECK(stranger.Start("Stranger", &error, 0, 47723));
		CHECK(src.AddAddress("127.0.0.1", n.Port()).ok);
		std::optional<lan::Peer> peerN = src.FindPeer(n.Self().Id());
		CHECK(peerN && src.Pair(*peerN, [](const QString &) { return true; }).ok);
		CHECK(waitUntil([&] {
			auto p = src.FindPeer(n.Self().Id());
			return p && p->storage && p->reachable;
		}));
		peerN = src.FindPeer(n.Self().Id());
		if (!peerN) {
			return;
		}

		/* only paired PCs may store, and only sensible names */
		CHECK(stranger.AddAddress("127.0.0.1", n.Port()).ok);
		std::optional<lan::Peer> strangerSeesN = stranger.FindPeer(n.Self().Id());
		CHECK(strangerSeesN &&
		      stranger.Request(*strangerSeesN, QJsonObject{{"op", "store_segment"}, {"segment", "x"}})
			      .notPaired);
		lan::Result r = src.Request(*peerN, QJsonObject{{"op", "store_segment"},
								{"segment", "../2026-09-20 10-00-00.mkv"},
								{"size", 10}});
		CHECK(!r.ok && r.error.contains("understand"));

		/* while recording, the newest segment is still being written */
		lan::StorageClient client(src, QDir(dir).filePath("sent.json"));
		QStringList waiting = client.Waiting(loop, true);
		CHECK(waiting.size() == 2 && QFileInfo(waiting.value(0)).fileName() == "2026-09-20 10-02-00.mkv");
		lan::PushReport total;
		for (int i = 0; i < 10; i++) {
			lan::PushReport p = client.Step(*peerN, &s, loop, true);
			CHECK(p.error.isEmpty());
			total.lines += p.lines;
			total.frames += p.frames;
			total.sessions += p.sessions;
			total.segments += p.segments;
			if (!p.more) {
				break;
			}
		}
		CHECK(total.lines == 2 && total.frames == 1 && total.sessions == 1 && total.segments == 2);
		CHECK(client.Waiting(loop, true).isEmpty());
		std::vector<lan::StoredSource> sources = node.Sources();
		CHECK(sources.size() == 1 && sources[0].name == "Gamer" && sources[0].segments == 2);
		if (sources.size() != 1) {
			return;
		}
		const QString stored = sources[0].LoopFolder();
		CHECK(same(QDir(stored).filePath("2026-09-20 10-00-00.mkv"), seg1));
		CHECK(same(QDir(stored).filePath("2026-09-20 10-02-00.mkv"), seg2));
		CHECK(arrived > 0);

		/* the log's copy keeps the ids, the screenshot and where each line is on it */
		{
			Store copy(sources[0].LogPath());
			CHECK(copy.Open());
			const std::vector<LogLine> lines = copy.Find(Query());
			CHECK(lines.size() == 2 && lines[0].id == ids[0] && lines[0].body == "meet at the docks");
			CHECK(lines.size() == 2 && lines[0].frameId.has_value());
			if (lines.size() == 2 && lines[0].frameId) {
				std::optional<Frame> f = copy.GetFrame(*lines[0].frameId);
				CHECK(f && same(f->path, shot) && f->path.startsWith(sources[0].folder));
				const std::vector<LogLine> onShot = copy.FrameLines(*lines[0].frameId);
				CHECK(onShot.size() == 2 && onShot[1].rect && onShot[1].rect->y0 == 120);
			}
			CHECK(copy.Search("docks").size() == 1);
		}
		/* Prisma's record is its own */
		CHECK(s.Unsynced(SyncTarget::Cloud).lines == 2 && s.Unsynced(SyncTarget::Lan).lines == 0);

		/* a line's video spot is sent again, as the storage PC's segment */
		s.SetVideo({ids[1]}, VideoSpot{seg2, 42.5});
		CHECK(s.Unsynced(SyncTarget::Lan).lines == 1 && s.Unsynced(SyncTarget::Cloud).lines == 2);
		lan::PushReport p = client.Step(*peerN, &s, loop, true);
		CHECK(p.error.isEmpty() && p.lines == 1);
		{
			Store copy(sources[0].LogPath());
			copy.Open();
			std::optional<LogLine> l = copy.Line(ids[1]);
			CHECK(l && l->video &&
			      QDir::cleanPath(l->video->path) ==
				      QDir::cleanPath(QDir(stored).filePath("2026-09-20 10-02-00.mkv")) &&
			      l->video->offset == 42.5);
		}

		/* recording stopped: the last segment goes on from where a cut-off
		 * send left it, and the oldest makes room for it */
		{
			QFile original(seg3);
			CHECK(original.open(QIODevice::ReadOnly));
			QFile part(QDir(stored).filePath("2026-09-20 10-04-00.mkv.part"));
			CHECK(part.open(QIODevice::WriteOnly));
			part.write(original.read(1000));
		}
		qint64 firstSent = -1;
		p = client.Step(*peerN, &s, loop, false, [&](const QString &, qint64 sent, qint64) {
			if (firstSent < 0) {
				firstSent = sent;
			}
			return true;
		});
		CHECK(p.error.isEmpty() && p.segments == 1 && firstSent == 1000 + lan::kChunk);
		CHECK(same(QDir(stored).filePath("2026-09-20 10-04-00.mkv"), seg3));
		CHECK(!QFileInfo::exists(QDir(stored).filePath("2026-09-20 10-00-00.mkv")));
		CHECK(!QFileInfo::exists(QDir(stored).filePath("2026-09-20 10-04-00.mkv.part")));
		CHECK(node.UsedBytes() <= 700 * 1024);

		/* full: something older than all it has is not wanted */
		segment("2026-09-19 09-00-00.mkv", 'd');
		p = client.Step(*peerN, &s, loop, false);
		CHECK(p.error.isEmpty() && p.skipped == 1 && p.segments == 0);
		CHECK(!QFileInfo::exists(QDir(stored).filePath("2026-09-19 09-00-00.mkv")));
		CHECK(client.Waiting(loop, false).isEmpty());

		/* what was sent is remembered, and forgotten with another storage PC */
		{
			lan::StorageClient again(src, QDir(dir).filePath("sent.json"));
			CHECK(again.Waiting(loop, false).isEmpty());
		}

		/* a PC that does not store says so */
		CHECK(src.AddAddress("127.0.0.1", stranger.Port()).ok);
		std::optional<lan::Peer> strangerPeer = src.FindPeer(stranger.Self().Id());
		CHECK(strangerPeer && src.Pair(*strangerPeer, [](const QString &) { return true; }).ok);
		strangerPeer = src.FindPeer(stranger.Self().Id());
		if (strangerPeer) {
			lan::StorageClient other(src, QDir(dir).filePath("sent.json"));
			p = other.Step(*strangerPeer, &s, loop, false);
			CHECK(!p.error.isEmpty() && p.error.contains("store"));
			CHECK(s.Unsynced(SyncTarget::Lan).lines == 2); /* starts again for the new one */
		}
		for (lan::Service *svc : {&n, &src, &stranger}) {
			svc->Stop();
		}
	});

	Test("lan_storage_pc_transcribes_and_each_segment_is_in_the_log_once", [&] {
		const QString dbS = NewDb();
		const QString dir = QFileInfo(dbS).absolutePath();
		const QString loop = QDir(dir).filePath("loop");
		QDir().mkpath(loop);
		auto segment = [&](const QString &name) {
			QFile f(QDir(loop).filePath(name));
			CHECK(f.open(QIODevice::WriteOnly));
			f.write(QByteArray(50 * 1024, 'x'));
			return QDir::cleanPath(QFileInfo(f.fileName()).absoluteFilePath());
		};
		const QString done = segment("2026-09-21 20-00-00.mkv");  /* transcribed here already */
		const QString stale = segment("2026-09-21 20-02-00.mkv"); /* an old transcript, of another size */
		const QString fresh = segment("2026-09-21 20-04-00.mkv");
		Store s(dbS);
		CHECK(s.Open());
		s.SetSegmentSpeech(done, 50 * 1024, {SpeechLine{1.0, "me", "said here", 0.9, VideoSpot{done, 1.0}}});
		s.SetSegmentSpeech(stale, 1234,
				   {SpeechLine{2.0, "me", "old reading one", 0.5, VideoSpot{stale, 2.0}},
				    SpeechLine{3.0, "me", "old reading two", 0.5, VideoSpot{stale, 3.0}}});

		lan::StorageNode node(QDir(dir).filePath("storage"), 1ull << 30);
		lan::Provider nodeProvider{{},
					   {},
					   [] { return QJsonObject{{"storage", true}}; },
					   yes,
					   [&](lan::Channel &ch, const QString &id, const QString &name,
					       const QJsonObject &req) { node.Answer(ch, id, name, req); }};
		lan::Provider plain{{}, {}, {}, yes};
		lan::Service n(lan::Identity::Generate(), QDir(dir).filePath("n.json"), nodeProvider);
		lan::Service src(lan::Identity::Generate(), QDir(dir).filePath("s.json"), plain);
		quick(n);
		quick(src);
		QString error;
		CHECK(n.Start("Storage", &error, 0, 47741));
		CHECK(src.Start("Gamer", &error, 0, 47742));
		CHECK(src.AddAddress("127.0.0.1", n.Port()).ok);
		std::optional<lan::Peer> peerN = src.FindPeer(n.Self().Id());
		CHECK(peerN && src.Pair(*peerN, [](const QString &) { return true; }).ok);
		peerN = src.FindPeer(n.Self().Id());
		if (!peerN) {
			return;
		}

		lan::StorageClient client(src, QDir(dir).filePath("sent.json"));
		client.speech = true;
		for (int i = 0; i < 10; i++) {
			const lan::PushReport p = client.Step(*peerN, &s, loop, false);
			CHECK(p.error.isEmpty());
			if (!p.more) {
				break;
			}
		}
		std::vector<lan::StoredSource> sources = node.Sources();
		CHECK(sources.size() == 1 && sources[0].speech && sources[0].segments == 3);
		if (sources.size() != 1) {
			return;
		}
		/* what was transcribed here is not transcribed there */
		{
			Store there(sources[0].SpeechLogPath());
			CHECK(there.Open());
			const auto seg = there.GetSpeechSegment(
				QDir::cleanPath(QDir(sources[0].LoopFolder()).filePath("2026-09-21 20-00-00.mkv")));
			CHECK(seg && seg->state == "done" && seg->lines == 0);
			CHECK(!there.GetSpeechSegment(
				QDir::cleanPath(QDir(sources[0].LoopFolder()).filePath("2026-09-21 20-04-00.mkv"))));
		}

		/* the storage PC transcribes the other two (as its speech worker would) */
		{
			Store there(sources[0].SpeechLogPath());
			CHECK(there.Open());
			for (const char *name : {"2026-09-21 20-02-00.mkv", "2026-09-21 20-04-00.mkv"}) {
				const QString p = QDir::cleanPath(QDir(sources[0].LoopFolder()).filePath(name));
				there.SetSegmentSpeech(p, 50 * 1024,
						       {SpeechLine{10.0, "teamspeak",
								   QStringLiteral("heard in %1").arg(name), 0.8,
								   VideoSpot{p, 4.5}}});
			}
		}
		lan::PushReport p = client.Step(*peerN, &s, loop, false);
		CHECK(p.error.isEmpty() && p.transcripts == 2);

		/* each segment's speech is in the log once, pointing at this PC's file */
		auto speechOf = [&](const QString &segment) {
			return s.SegmentSpeech(segment);
		};
		CHECK(speechOf(done).size() == 1 && speechOf(done)[0].body == "said here");
		const std::vector<SpeechLine> replaced = speechOf(stale);
		CHECK(replaced.size() == 1 && replaced[0].body == "heard in 2026-09-21 20-02-00.mkv" &&
		      replaced[0].speaker == "teamspeak" && replaced[0].video.path == stale &&
		      replaced[0].video.offset == 4.5);
		CHECK(speechOf(fresh).size() == 1);
		CHECK(s.Search("old reading").empty());
		const auto freshSeg = s.GetSpeechSegment(fresh);
		CHECK(freshSeg && freshSeg->size == 50 * 1024 && freshSeg->state == "done");

		/* asking again changes nothing */
		p = client.Step(*peerN, &s, loop, false);
		CHECK(p.error.isEmpty() && p.transcripts == 0);
		CHECK(speechOf(stale).size() == 1 && speechOf(fresh).size() == 1);

		/* the returned lines go back to the storage PC's copy like any other */
		for (int i = 0; i < 5 && s.Unsynced(SyncTarget::Lan).lines > 0; i++) {
			client.Step(*peerN, &s, loop, false);
		}
		{
			Store copy(sources[0].LogPath());
			CHECK(copy.Open());
			CHECK(copy.Search("heard").size() == 2 && copy.Search("old reading").empty());
		}

		/* turned off: the storage PC stops transcribing for it */
		client.speech = false;
		segment("2026-09-21 20-06-00.mkv");
		p = client.Step(*peerN, &s, loop, false);
		CHECK(p.error.isEmpty() && p.segments == 1);
		CHECK(!node.Sources()[0].speech);
		n.Stop();
		src.Stop();
	});

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
