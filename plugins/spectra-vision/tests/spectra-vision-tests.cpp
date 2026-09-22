/*
 * spectra-vision tests: ports of Obscura's tests/test_core.py (chat parsing,
 * HUD clock, redaction, regions) and Lucida's gate tests, plus difflib and
 * reference values computed with the original Python code.
 *
 *   spectra-vision-tests <tests/data dir> [<synthetic screenshots dir>]
 *
 * The optional screenshot directory (full_1080.png etc. rendered by
 * Obscura's tests/synth.py) enables the OCR pipeline tests.
 */

#include <spectra-vision/chat.hpp>
#include <spectra-vision/imaging.hpp>
#include <spectra-vision/text.hpp>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <functional>

using namespace spectra;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		checks++;                                                             \
		if (!(cond)) {                                                        \
			failures++;                                                   \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                     \
	} while (0)

#define CHECK_EQ_STR(a, b)                                                                              \
	do {                                                                                            \
		checks++;                                                                               \
		QString _a = (a), _b = (b);                                                             \
		if (_a != _b) {                                                                         \
			failures++;                                                                     \
			fprintf(stderr, "  FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, qPrintable(_a), \
				qPrintable(_b));                                                        \
		}                                                                                       \
	} while (0)

static void Test(const char *name, const std::function<void()> &fn)
{
	int before = failures;
	fn();
	printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

static OcrBox Box(const QString &text, float score, float x0, float y0, float x1, float y1)
{
	OcrBox b;
	b.text = text.toStdString();
	b.score = score;
	b.x0 = x0;
	b.y0 = y0;
	b.x1 = x1;
	b.y1 = y1;
	return b;
}

static Row MakeRow(const QString &text, float y, float x0 = 33)
{
	return Row::FromBox(Box(text, 0.99f, x0, y, x0 + 7 * text.size(), y + 16));
}

static Image LoadImage(const QString &path)
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
	const QString dataDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("data");
	const QString shotsDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();

	/* ---- obscura tests/test_core.py ---- */

	Test("parse_body_tags_and_time", [] {
		auto p = ParseBody("[16:28:10] [Report] [Question] Awazki accepted report #1002");
		CHECK(p.time && *p.time == "16:28:10");
		CHECK((p.tags == QStringList{"report", "question"}));
		CHECK_EQ_STR(ChannelOf(p.body, p.tags, false), "report / question");
	});

	Test("parse_body_tolerates_scrollbar_junk_and_colon_tags", [] {
		auto p = ParseBody("|[16:38:47] [INFO]: PM to Celeste Bullfinch (44): (( Date");
		CHECK(p.time && *p.time == "16:38:47");
		CHECK_EQ_STR(ChannelOf(p.body, p.tags, false), "info / pm");
	});

	Test("timestamp_with_misread_bracket", [] {
		auto p = ParseBody("E17:16:12] [14 min] [Report] [Rule Breaker - Urgent!] [1009] Richard_Forth: hi");
		CHECK(p.time && *p.time == "17:16:12");
		CHECK_EQ_STR(ChannelOf(p.body, p.tags, false), "report / rule breaker urgent");
		auto q = ParseBody("R [17:16:12] [14 min] [Report] [Rule Breaker - Urgent!] [1009] Richard_Forth");
		CHECK(q.time && *q.time == "17:16:12" && q.tags.size() > 1 && q.tags[1] == "report");
		CHECK(!ParseBody("playing bumper cars plz look xD").time);
	});

	Test("volatile_tags_do_not_fragment_channels", [] {
		auto p = ParseBody("[17:16:12] [10 min] [Report] [Question] [1012] Harry_Knight: radio?");
		CHECK_EQ_STR(ChannelOf(p.body, p.tags, false), "report / question");
		auto q = ParseBody("[17:16:48] [Report] [Question] [ID: 1017] Player: (104) Anthony_Mercer:");
		CHECK_EQ_STR(ChannelOf(q.body, q.tags, false), "report / question");
	});

	Test("report_message_follow_up_joins_report_channel", [] {
		std::vector<Row> rows{MakeRow("[17:16:23] [Report] [Question] Message to player (22) Harry_Knight:",
					      30),
				      MakeRow("[17:16:23] Hey Harry", 52),
				      MakeRow("[17:16:50] * Celeste Bullfinch (44) retrieves a bodybag", 74)};
		Image img(800, 200, 3, 0);
		auto entries = BuildEntries(img, rows, RowRects(rows, 15, 0, 200));
		CHECK(entries.size() == 3);
		if (entries.size() == 3) {
			CHECK_EQ_STR(entries[0].channel, "report / question");
			CHECK_EQ_STR(entries[1].channel, "report / question / message");
			CHECK_EQ_STR(entries[2].channel, "emote");
		}
	});

	Test("far_right_hud_text_is_not_part_of_chat_row", [] {
		auto rows = GroupRows({Box("[17:16:12] [Report] hello", 0.99f, 33, 100, 400, 116),
				       Box("Mission Row", 0.9f, 745, 101, 800, 115)});
		CHECK(rows.size() == 1);
		if (rows.size() == 1) {
			CHECK_EQ_STR(rows[0].Text(), "[17:16:12] [Report] hello");
			CHECK(rows[0].x1 == 400);
		}
	});

	Test("tall_logo_does_not_merge_lines", [] {
		std::vector<OcrBox> boxes{Box("E", 0.6f, 15, 17, 30, 50)};
		QStringList expected;
		for (int i = 0; i < 5; i++) {
			QString t = QStringLiteral("[17:16:1%1] [Report] line %1").arg(i);
			boxes.push_back(Box(t, 0.99f, 33, 20.f + 22 * i, 400, 36.f + 22 * i));
			expected << t;
		}
		QStringList got;
		for (const Row &r : GroupRows(boxes)) {
			got << r.Text();
		}
		CHECK(got == expected);
	});

	Test("channel_without_tags", [] {
		CHECK_EQ_STR(ChannelOf("* Celeste Bullfinch (44) looks", {}, false), "emote");
		CHECK_EQ_STR(ChannelOf("You whisper to Celeste Bullfinch (44): hi", {}, false), "whisper");
		CHECK_EQ_STR(ChannelOf("Celeste Bullfinch (44) says: hi", {}, false), "say");
		CHECK_EQ_STR(ChannelOf("that happens now without f6", {}, true), "continuation");
	});

	Test("parse_unix_timestamp_variants", [] {
		CHECK(ParseUnixTimestamp("30 | 1789231305") == 1789231305LL);
		CHECK(ParseUnixTimestamp("3011789231305") == 1789231305LL);
		CHECK(ParseUnixTimestamp("30 1789 231305") == 1789231305LL);
		CHECK(!ParseUnixTimestamp("Downtown"));
		CHECK(!ParseUnixTimestamp("30 | 9999999999"));
	});

	Test("merge_rects_stacks_touching_lines", [] {
		auto m = MergeRects({{10, 0, 100, 22}, {10, 22, 60, 44}, {10, 80, 50, 100}});
		CHECK((m == std::vector<Rect>{{10, 0, 100, 44}, {10, 80, 50, 100}}));
	});

	Test("redact_is_opaque_and_ignores_text_colour", [] {
		Image img(100, 40, 3, 30);
		for (int y = 15; y < 25; y++) {
			for (int x = 20; x < 80; x++) {
				uint8_t *p = img.at(x, y);
				p[0] = p[1] = p[2] = 255;
			}
		}
		Image out = Redact(img, {{10, 5, 90, 35}});
		bool opaque = true, untouched = true;
		for (int y = 5; y < 35; y++) {
			for (int x = 10; x < 90; x++) {
				const uint8_t *p = out.at(x, y);
				opaque = opaque && p[0] == 30 && p[1] == 30 && p[2] == 30;
			}
		}
		for (int y = 0; y < 5; y++) {
			untouched = untouched && memcmp(out.row(y), img.row(y), (size_t)img.width * 3) == 0;
		}
		CHECK(opaque);
		CHECK(untouched);
	});

	Test("region_round_trip", [] {
		Region r = Region::FromPixels({15, 16, 787, 362}, 1920, 1080);
		CHECK((r.ToPixels(1920, 1080) == Rect{15, 16, 787, 362}));
		CHECK((kDefaultChatRegion.ToPixels(1920, 1080) == Rect{15, 16, 787, 362}));
		CHECK((kDefaultHudRegion.ToPixels(1920, 1080) == Rect{845, 0, 1075, 30}));
	});

	/* ---- difflib (reference ratios from CPython) ---- */

	Test("difflib_ratio_matches_python", [&] {
		QFile f(QDir(dataDir).filePath("difflib.json"));
		CHECK(f.open(QIODevice::ReadOnly));
		QJsonArray cases = QJsonDocument::fromJson(f.readAll()).array();
		CHECK(!cases.isEmpty());
		for (const QJsonValue &v : cases) {
			QJsonObject o = v.toObject();
			double got = SequenceRatio(o["a"].toString(), o["b"].toString());
			double want = o["ratio"].toDouble();
			if (std::fabs(got - want) > 1e-12) {
				fprintf(stderr, "    ratio(%d chars) = %.12f, python %.12f\n",
					(int)o["a"].toString().size(), got, want);
			}
			CHECK(std::fabs(got - want) <= 1e-12);
		}
		CHECK((GetCloseMatches("admin chot", {"admin chat", "admin system", "support chat"}, 1, 0.85) ==
		       QStringList{"admin chat"}));
		CHECK((GetCloseMatches("report / questio", {"report / question", "report"}, 1, 0.85) ==
		       QStringList{"report / question"}));
	});

	/* ---- lucida tests/test_gate.py ---- */

	Test("gate_first_frame_always_changed", [] {
		std::vector<float> s{1, 2, 3};
		CHECK(SignatureChanged(nullptr, s, 0.06).first);
	});

	Test("gate_flat_images_have_no_text", [] {
		for (uint8_t v : {(uint8_t)30, (uint8_t)220}) {
			Image flat(200, 60, 3, v);
			auto m = TextMask(flat);
			int sum = 0;
			for (uint8_t x : m) {
				sum += x;
			}
			CHECK(sum == 0);
		}
	});

	if (!shotsDir.isEmpty()) {
		/* ---- values from the Python code on synth.py screenshots ---- */

		Test("gate_matches_python_on_chat_crop", [&] {
			Image chat = LoadImage(QDir(shotsDir).filePath("chat_1080.png"));
			Image changed = LoadImage(QDir(shotsDir).filePath("chat_1080_changed.png"));
			CHECK(!chat.empty() && !changed.empty());
			auto m = TextMask(chat);
			long sum = 0;
			for (uint8_t x : m) {
				sum += x;
			}
			CHECK(sum == 22508);
			auto s1 = Signature(m, chat.width, chat.height);
			auto s2 = Signature(TextMask(changed), changed.width, changed.height);
			double d = SignatureDistance(s1, s2);
			if (std::fabs(d - 0.03378407284617424) > 1e-9) {
				fprintf(stderr, "    distance %.12f\n", d);
			}
			CHECK(std::fabs(d - 0.03378407284617424) < 1e-9);
		});

		Test("colour_features_match_python", [&] {
			Image full = LoadImage(QDir(shotsDir).filePath("full_1080.png"));
			auto c = ColourFeatures(full, {15, 16, 787, 50});
			CHECK(std::fabs(c[0] - 0.2608f) < 1e-6f && std::fabs(c[12] - 0.2608f) < 1e-6f);
			for (int i = 1; i < 12; i++) {
				CHECK(c[i] == 0.0f);
			}
		});

		/* ---- obscura tests/test_pipeline.py (channels of synth.ENTRIES) ---- */

		std::string error;
		OcrEngine::Options options;
		auto ocr = OcrEngine::Create(options, error);
		CHECK(ocr != nullptr);
		if (ocr) {
			const QStringList channels{"admin chat", "emote",        "support chat", "report / question",
						   "info / pm",  "admin system", "say",          "whisper",
						   "law"};
			for (int height : {1080, 1440}) {
				QString name = QStringLiteral("pipeline_finds_entries_and_timestamp_%1").arg(height);
				Test(qPrintable(name), [&] {
					Image img = LoadImage(
						QDir(shotsDir).filePath(QStringLiteral("full_%1.png").arg(height)));
					Analysis a = Analyze(img, *ocr, kDefaultChatRegion, kDefaultHudRegion, true);
					CHECK(a.unixTs == 1789231305LL);
					QStringList got;
					for (const ChatEntry &e : a.entries) {
						got << e.channel;
					}
					if (got != channels) {
						fprintf(stderr, "    channels: %s\n", qPrintable(got.join(" | ")));
					}
					CHECK(got == channels);
					if (a.entries.size() == 9) {
						CHECK(a.entries[4].rows.size() == 2); /* the INFO entry wraps */
					}
				});
			}
		}
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
