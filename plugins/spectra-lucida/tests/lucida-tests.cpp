/*
 * Lucida core tests: ports of lucida/tests/test_store.py and
 * test_recorder.py, plus regression tests for the behaviours Spectra fixes.
 *
 *   lucida-tests <scratch dir>
 */

#include "../core/recorder.hpp"
#include "../core/store.hpp"
#include "../core/tagger.hpp"
#include "../core/video.hpp"

#include <sqlite3.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdio>
#include <functional>

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

	explicit Rig(const std::vector<QStringList> &script) : store(NewDb()), ocr(script)
	{
		store.Open();
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
		long long fid = s.AttachFrame(ids, "shot.jpg", 1920, 1080, 1789231300);
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
		s.AttachFrame(oldIds, "old.jpg", 1920, 1080, oldTs);
		long long kept = s.AttachFrame(newIds, "new.jpg", 1920, 1080, newTs);
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
		CHECK(s.AttachFrame(ids, "shot.jpg", 1920, 1080, 1789231400) > 0);
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
			s.AttachFrame(ids, "shot.jpg", 1920, 1080, 1789231300);
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
		s.AttachFrame(ids, "orphan.jpg", 1920, 1080, oldTs);
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
		long long frameId = s.AttachFrame(ids, "C:/frames/x.jpg", 1920, 1080, NowSecs());
		auto frames = s.Frames();
		CHECK(frames.size() == 1 && frames[0].lines == 2 && frames[0].labels == QStringList{"bodycam-rp"});
		Query q;
		q.frameId = frameId;
		CHECK(s.Find(q).size() == 2);
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

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
