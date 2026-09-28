/*
 * Tests for the in-app updater: version ordering, picking a release and its
 * zip, the zip extractor, and the download job end to end against local
 * files, including spectra-updater.exe swapping a fake install.
 *
 *   spectra-update-tests <scratch dir> [--zip <release zip>] [--network <channel>]
 *
 * --zip extracts a real release zip. --network downloads the newest release
 * of the channel ("stable" or "rc") from GitHub into <scratch>\net\install.update\new,
 * for the updater's end-to-end test.
 */

#include "../../utility/SpectraReleases.hpp"
#include "../../utility/SpectraUpdateJob.hpp"
#include "../../utility/SpectraZip.hpp"
#include "../updater-common.hpp"

#include <zlib.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <curl/curl.h>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace SpectraReleases;
namespace fs = std::filesystem;

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

static void Test(const char *name, const std::function<void()> &fn)
{
	int before = failures;
	fn();
	printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
	fflush(stdout);
}

static QString Dir(const QString &name)
{
	QString dir = QDir(scratch).filePath(name);
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);
	return dir;
}

static bool WriteFile(const QString &path, const QByteArray &data)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		return false;
	}
	f.write(data);
	return true;
}

static QByteArray ReadFile(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		return {};
	}
	return f.readAll();
}

/* --- a small zip writer --------------------------------------------------------- */

struct ZipEntry {
	std::string name;
	QByteArray data;
	bool deflate = true;
	bool badCrc = false;
};

static void Put16(QByteArray &b, uint16_t v)
{
	b.append((char)(v & 0xff));
	b.append((char)(v >> 8));
}

static void Put32(QByteArray &b, uint32_t v)
{
	Put16(b, (uint16_t)(v & 0xffff));
	Put16(b, (uint16_t)(v >> 16));
}

static void Put64(QByteArray &b, uint64_t v)
{
	Put32(b, (uint32_t)(v & 0xffffffff));
	Put32(b, (uint32_t)(v >> 32));
}

static QByteArray RawDeflate(const QByteArray &data)
{
	z_stream zs = {};
	deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
	QByteArray out(deflateBound(&zs, (uLong)data.size()) + 16, '\0');
	zs.next_in = (Bytef *)data.data();
	zs.avail_in = (uInt)data.size();
	zs.next_out = (Bytef *)out.data();
	zs.avail_out = (uInt)out.size();
	deflate(&zs, Z_FINISH);
	out.resize((qsizetype)zs.total_out);
	deflateEnd(&zs);
	return out;
}

/* zip64: write every size and offset as 0xFFFFFFFF with a zip64 extra field,
 * and a zip64 end of central directory */
static QByteArray MakeZip(const std::vector<ZipEntry> &entries, bool zip64 = false)
{
	QByteArray zip, cd;
	for (const ZipEntry &e : entries) {
		bool dir = !e.name.empty() && e.name.back() == '/';
		QByteArray payload = dir ? QByteArray() : (e.deflate ? RawDeflate(e.data) : e.data);
		uint16_t method = (!dir && e.deflate) ? 8 : 0;
		uint32_t crc = crc32(0, (const Bytef *)e.data.data(), (uInt)e.data.size());
		if (e.badCrc) {
			crc ^= 0x1234;
		}
		uint64_t offset = (uint64_t)zip.size();

		Put32(zip, 0x04034b50);
		Put16(zip, 20);
		Put16(zip, 0x800);
		Put16(zip, method);
		Put32(zip, 0);
		Put32(zip, crc);
		Put32(zip, (uint32_t)payload.size());
		Put32(zip, (uint32_t)e.data.size());
		Put16(zip, (uint16_t)e.name.size());
		Put16(zip, 0);
		zip.append(e.name.data(), (qsizetype)e.name.size());
		zip.append(payload);

		QByteArray extra;
		if (zip64) {
			Put16(extra, 0x0001);
			Put16(extra, 24);
			Put64(extra, (uint64_t)e.data.size());
			Put64(extra, (uint64_t)payload.size());
			Put64(extra, offset);
		}
		Put32(cd, 0x02014b50);
		Put16(cd, 45);
		Put16(cd, 45);
		Put16(cd, 0x800);
		Put16(cd, method);
		Put32(cd, 0);
		Put32(cd, crc);
		Put32(cd, zip64 ? 0xFFFFFFFF : (uint32_t)payload.size());
		Put32(cd, zip64 ? 0xFFFFFFFF : (uint32_t)e.data.size());
		Put16(cd, (uint16_t)e.name.size());
		Put16(cd, (uint16_t)extra.size());
		Put16(cd, 0);
		Put16(cd, 0);
		Put16(cd, 0);
		Put32(cd, 0);
		Put32(cd, zip64 ? 0xFFFFFFFF : (uint32_t)offset);
		cd.append(e.name.data(), (qsizetype)e.name.size());
		cd.append(extra);
	}

	uint64_t cdOffset = (uint64_t)zip.size();
	zip.append(cd);
	if (zip64) {
		uint64_t eocd64 = (uint64_t)zip.size();
		Put32(zip, 0x06064b50);
		Put64(zip, 44);
		Put16(zip, 45);
		Put16(zip, 45);
		Put32(zip, 0);
		Put32(zip, 0);
		Put64(zip, entries.size());
		Put64(zip, entries.size());
		Put64(zip, (uint64_t)cd.size());
		Put64(zip, cdOffset);
		Put32(zip, 0x07064b50);
		Put32(zip, 0);
		Put64(zip, eocd64);
		Put32(zip, 1);
	}
	Put32(zip, 0x06054b50);
	Put16(zip, 0);
	Put16(zip, 0);
	Put16(zip, zip64 ? 0xFFFF : (uint16_t)entries.size());
	Put16(zip, zip64 ? 0xFFFF : (uint16_t)entries.size());
	Put32(zip, zip64 ? 0xFFFFFFFF : (uint32_t)cd.size());
	Put32(zip, zip64 ? 0xFFFFFFFF : (uint32_t)cdOffset);
	Put16(zip, 0);
	return zip;
}

static fs::path P(const QString &path)
{
	return fs::path(QDir::toNativeSeparators(path).toStdWString());
}

/* A package with an obs-spectra.exe, like a release zip's contents */
static std::vector<ZipEntry> PackageEntries(const QByteArray &exeContent)
{
	QByteArray big;
	for (int i = 0; i < 200000; i++) {
		big.append(QByteArray::number(i * 7919 % 1000003));
	}
	return {
		{"bin/", {}, false},
		{"bin/64bit/obs-spectra.exe", exeContent, true},
		{"bin/64bit/lucida-viewer.exe", "viewer", false},
		{"data/obs-studio/big.bin", big, true},
		{"data/obs-studio/empty.txt", QByteArray(), true},
		{"obs-plugins/64bit/plugin.dll", "plugin", true},
		{"portable_mode.txt", QByteArray(), false},
	};
}

/* --- tests ---------------------------------------------------------------------- */

static void TestVersions()
{
	Test("versions: parse", []() {
		Version v = ParseVersion("32.2.2-spectra.3-rc.6");
		CHECK(v.major == 32 && v.minor == 2 && v.patch == 2 && v.spectra == 3 && v.rc == 6 && !v.beta);
		CHECK(v.prerelease());
		CHECK(v.Base() == "32.2.2");
		Version f = ParseVersion("32.2.2-spectra.2");
		CHECK(f.spectra == 2 && f.rc == 0 && !f.prerelease());
		CHECK(ParseVersion("32.2.1-beta2").beta == 2);
		CHECK(ParseVersion("32.2.1-rc1").rc == 1);
		CHECK(ParseVersion("v32.1.0").major == 32);
		CHECK(!ParseVersion("nightly").valid());
		CHECK(!ParseVersion("").valid());
	});

	Test("versions: order", []() {
		auto lt = [](const char *a, const char *b) {
			return ParseVersion(a) < ParseVersion(b) && !(ParseVersion(b) < ParseVersion(a));
		};
		CHECK(lt("32.2.2-spectra.2", "32.2.2-spectra.3-rc.1"));
		CHECK(lt("32.2.2-spectra.3-rc.1", "32.2.2-spectra.3-rc.6"));
		CHECK(lt("32.2.2-spectra.3-rc.6", "32.2.2-spectra.3-rc.10")); /* numeric, not text */
		CHECK(lt("32.2.2-spectra.3-rc.10", "32.2.2-spectra.3"));      /* a final beats its RCs */
		CHECK(lt("32.2.2-spectra.3", "32.2.2-spectra.4-rc.1"));
		CHECK(lt("32.2.2-spectra.9", "32.2.3-spectra.1-rc.1")); /* base version first */
		CHECK(lt("32.2.9-spectra.1", "32.10.0-spectra.1"));
		CHECK(lt("32.2.1", "32.2.1-spectra.1"));
		CHECK(lt("32.2.1-beta3", "32.2.1-rc1"));
		CHECK(lt("32.2.1-rc1", "32.2.1"));
		CHECK(ParseVersion("32.2.2-spectra.3-rc.6") == ParseVersion("32.2.2-spectra.3-rc.6"));
		CHECK(!(ParseVersion("32.2.2-spectra.3") == ParseVersion("32.2.2-spectra.3-rc.1")));
	});

	Test("versions: release tags vs developer builds", []() {
		CHECK(IsReleaseTag("32.2.2-spectra.2"));
		CHECK(IsReleaseTag("32.2.2-spectra.3-rc.6"));
		CHECK(IsReleaseTag("32.2.1"));
		CHECK(IsReleaseTag("32.2.1-beta2"));
		CHECK(!IsReleaseTag("32.2.2-spectra.2-5-gabc1234"));
		CHECK(!IsReleaseTag("32.2.2-spectra.3-rc.6-1-g0123456-modified"));
		CHECK(!IsReleaseTag("32.2.2-spectra.2-dirty"));
		CHECK(!IsReleaseTag(""));
	});

	Test("channel setting", []() {
		CHECK(ChannelFromSetting("rc", "32.2.2-spectra.2") == Channel::ReleaseCandidates);
		CHECK(ChannelFromSetting("stable", "32.2.2-spectra.3-rc.6") == Channel::Stable);
		/* unset: an RC build stays on RCs, a release on stable */
		CHECK(ChannelFromSetting("", "32.2.2-spectra.3-rc.6") == Channel::ReleaseCandidates);
		CHECK(ChannelFromSetting("", "32.2.2-spectra.2") == Channel::Stable);
		CHECK(ChannelFromSetting("bogus", "32.2.2-spectra.2") == Channel::Stable);
		CHECK(ChannelSetting(Channel::ReleaseCandidates) == "rc");
		CHECK(ChannelSetting(Channel::Stable) == "stable");
	});
}

static QByteArray ReleaseJson(const char *tag, bool prerelease, bool draft, bool portable, bool regular,
			      bool checksums = true)
{
	QString assets;
	auto add = [&](const QString &name) {
		if (!assets.isEmpty()) {
			assets += ",";
		}
		assets += QStringLiteral("{\"name\":\"%1\",\"size\":1000,\"browser_download_url\":"
					 "\"https://example.invalid/%1\"}")
				  .arg(name);
	};
	if (portable) {
		add(QStringLiteral("OBS-Spectra-%1-Windows-x64-Portable.zip").arg(tag));
		if (checksums) {
			add(QStringLiteral("OBS-Spectra-%1-Windows-x64-Portable.zip.sha256").arg(tag));
		}
	}
	if (regular) {
		add(QStringLiteral("OBS-Spectra-%1-Windows-x64.zip").arg(tag));
		if (checksums) {
			add(QStringLiteral("OBS-Spectra-%1-Windows-x64.zip.sha256").arg(tag));
		}
	}
	return QStringLiteral("{\"tag_name\":\"%1\",\"html_url\":\"https://example.invalid/%1\",\"body\":\"notes\","
			      "\"prerelease\":%2,\"draft\":%3,\"assets\":[%4]}")
		.arg(QString::fromLatin1(tag), QString::fromLatin1(prerelease ? "true" : "false"),
		     QString::fromLatin1(draft ? "true" : "false"), assets)
		.toUtf8();
}

static QByteArray List(const std::vector<QByteArray> &releases)
{
	QByteArray json = "[";
	for (size_t i = 0; i < releases.size(); i++) {
		json += (i ? "," : "") + releases[i];
	}
	return json + "]";
}

static void TestReleases()
{
	Test("releases: parse", []() {
		QString error;
		QList<Release> r =
			ParseReleases(List({ReleaseJson("32.2.2-spectra.3-rc.6", true, false, true, true)}), &error);
		CHECK(error.isEmpty());
		CHECK(r.size() == 1);
		CHECK(r[0].tag == "32.2.2-spectra.3-rc.6");
		CHECK(r[0].prerelease && !r[0].draft);
		CHECK(r[0].assets.size() == 4);
		CHECK(r[0].assets[0].size == 1000);
		CHECK(r[0].version.rc == 6);
		CHECK(ParseReleases("{\"message\":\"API rate limit exceeded\"}", &error).isEmpty() && !error.isEmpty());
		CHECK(ParseReleases("not json", &error).isEmpty() && !error.isEmpty());
	});

	Test("releases: channels pick by version, not by list order", []() {
		/* GitHub lists newest first by date; a hotfix final can be published after an RC */
		QList<Release> r = ParseReleases(List({
			ReleaseJson("32.2.2-spectra.1", false, false, true, true),
			ReleaseJson("32.2.2-spectra.3-rc.10", true, false, true, true),
			ReleaseJson("32.2.2-spectra.3-rc.9", true, false, true, true),
			ReleaseJson("32.2.2-spectra.4-rc.1", true, true, true, true),     /* draft */
			ReleaseJson("32.2.2-spectra.3-rc.11", true, false, false, false), /* no zips yet */
			ReleaseJson("32.2.2-spectra.2", false, false, true, true),
			ReleaseJson("nightly", false, false, true, true),
		}));
		std::optional<Release> stable = SelectRelease(r, Channel::Stable);
		std::optional<Release> rc = SelectRelease(r, Channel::ReleaseCandidates);
		CHECK(stable && stable->tag == "32.2.2-spectra.2");
		CHECK(rc && rc->tag == "32.2.2-spectra.3-rc.10");
	});

	Test("releases: a final beats its own release candidates", []() {
		QList<Release> r = ParseReleases(List({
			ReleaseJson("32.2.2-spectra.3-rc.6", true, false, true, true),
			ReleaseJson("32.2.2-spectra.3", false, false, true, true),
			ReleaseJson("32.2.2-spectra.3-rc.7", true, false, true, true),
		}));
		CHECK(SelectRelease(r, Channel::ReleaseCandidates)->tag == "32.2.2-spectra.3");
		CHECK(SelectRelease(r, Channel::Stable)->tag == "32.2.2-spectra.3");
	});

	Test("releases: stable skips RC tags even when not flagged pre-release", []() {
		QList<Release> r = ParseReleases(List({
			ReleaseJson("32.2.2-spectra.3-rc.6", false, false, true, true),
			ReleaseJson("32.2.2-spectra.2", false, false, true, true),
		}));
		CHECK(SelectRelease(r, Channel::Stable)->tag == "32.2.2-spectra.2");
	});

	Test("releases: nothing to offer", []() {
		QList<Release> r = ParseReleases(List({ReleaseJson("32.2.2-spectra.3-rc.6", true, false, true, true)}));
		CHECK(!SelectRelease(r, Channel::Stable));
		CHECK(!SelectRelease({}, Channel::ReleaseCandidates));
	});

	Test("releases: asset for portable and installed-style installs", []() {
		Release both = ParseReleases(List({ReleaseJson("32.2.2-spectra.2", false, false, true, true)}))[0];
		std::optional<Download> p = SelectDownload(both, true);
		CHECK(p && p->portable && p->zip.name == "OBS-Spectra-32.2.2-spectra.2-Windows-x64-Portable.zip");
		CHECK(p && p->checksum.name == "OBS-Spectra-32.2.2-spectra.2-Windows-x64-Portable.zip.sha256");
		std::optional<Download> i = SelectDownload(both, false);
		CHECK(i && !i->portable && i->zip.name == "OBS-Spectra-32.2.2-spectra.2-Windows-x64.zip");
		CHECK(i && i->checksum.name == "OBS-Spectra-32.2.2-spectra.2-Windows-x64.zip.sha256");
		CHECK(i && i->zip.url == "https://example.invalid/OBS-Spectra-32.2.2-spectra.2-Windows-x64.zip");

		/* an old release with only the regular zip still updates a portable install */
		Release regularOnly =
			ParseReleases(List({ReleaseJson("32.2.1-spectra.1", false, false, false, true, false)}))[0];
		std::optional<Download> fallback = SelectDownload(regularOnly, true);
		CHECK(fallback && !fallback->portable && fallback->checksum.name.isEmpty());

		Release none = ParseReleases(List({ReleaseJson("32.2.1-spectra.1", false, false, false, false)}))[0];
		CHECK(!SelectDownload(none, true));
	});

	Test("sha256 files", []() {
		QString hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
		CHECK(ParseSha256((hex + "  OBS-Spectra-32.2.2-spectra.2-Windows-x64.zip\n").toUtf8()) == hex);
		CHECK(ParseSha256((hex.toUpper() + " *file.zip\r\n").toUtf8()) == hex);
		CHECK(ParseSha256(hex.toUtf8()) == hex);
		CHECK(ParseSha256("abc  file.zip").isEmpty());
		CHECK(ParseSha256("").isEmpty());
		CHECK(ParseSha256("<html>Not Found</html>").isEmpty());
	});

	Test("command-line quoting", []() {
		CHECK(SpectraUpdateJob::QuoteArguments({"--portable", "--multi"}) == "--portable --multi");
		CHECK(SpectraUpdateJob::QuoteArguments({"--profile", "My Profile"}) == "--profile \"My Profile\"");
		CHECK(SpectraUpdateJob::QuoteArguments({"a\"b"}) == "\"a\\\"b\"");
		CHECK(SpectraUpdateJob::QuoteArguments({"C:\\dir with space\\"}) == "\"C:\\dir with space\\\\\"");
		CHECK(SpectraUpdateJob::QuoteArguments({""}) == "\"\"");
	});
}

static void TestZip()
{
	Test("zip: extracts stored, deflated, empty and nested entries", []() {
		QString dir = Dir("zip-ok");
		std::vector<ZipEntry> entries = PackageEntries("new exe");
		WriteFile(dir + "/p.zip", MakeZip(entries));
		uint64_t lastDone = 0, lastTotal = 0;
		std::string err = SpectraZip::Extract(P(dir + "/p.zip"), P(dir + "/out"), [&](uint64_t d, uint64_t t) {
			lastDone = d;
			lastTotal = t;
			return true;
		});
		CHECK(err.empty());
		for (const ZipEntry &e : entries) {
			if (e.name.back() == '/') {
				CHECK(QFileInfo(dir + "/out/" + QString::fromStdString(e.name)).isDir());
			} else {
				CHECK(ReadFile(dir + "/out/" + QString::fromStdString(e.name)) == e.data);
			}
		}
		CHECK(lastTotal > 0 && lastDone == lastTotal);
		if (!err.empty()) {
			fprintf(stderr, "  %s\n", err.c_str());
		}
	});

	Test("zip: zip64 records", []() {
		QString dir = Dir("zip-64");
		std::vector<ZipEntry> entries = PackageEntries("zip64 exe");
		WriteFile(dir + "/p.zip", MakeZip(entries, true));
		std::string err = SpectraZip::Extract(P(dir + "/p.zip"), P(dir + "/out"));
		CHECK(err.empty());
		CHECK(ReadFile(dir + "/out/bin/64bit/obs-spectra.exe") == "zip64 exe");
		CHECK(ReadFile(dir + "/out/data/obs-studio/big.bin") == entries[3].data);
	});

	Test("zip: refuses names that leave the folder", []() {
		for (const char *name :
		     {"../evil.txt", "bin/../../evil.txt", "/evil.txt", "C:/evil.txt", "..\\evil.txt"}) {
			QString dir = Dir("zip-slip");
			WriteFile(dir + "/p.zip", MakeZip({{"ok.txt", "ok"}, {name, "evil"}}));
			std::string err = SpectraZip::Extract(P(dir + "/p.zip"), P(dir + "/out"));
			CHECK(!err.empty());
			CHECK(!QFileInfo::exists(dir + "/evil.txt"));
			CHECK(!QFileInfo::exists(scratch + "/evil.txt"));
		}
	});

	Test("zip: damaged archives fail", []() {
		QString dir = Dir("zip-bad");
		WriteFile(dir + "/crc.zip", MakeZip({{"a.txt", "hello hello hello", true, true}}));
		CHECK(SpectraZip::Extract(P(dir + "/crc.zip"), P(dir + "/out1")).find("checksum") != std::string::npos);

		QByteArray good = MakeZip(PackageEntries("x"));
		WriteFile(dir + "/cut.zip", good.left(good.size() / 2));
		CHECK(!SpectraZip::Extract(P(dir + "/cut.zip"), P(dir + "/out2")).empty());

		QByteArray flipped = good;
		qsizetype at =
			good.indexOf("data/obs-studio/big.bin") + 23 + 100; /* inside big.bin's compressed data */
		flipped[at] = (char)(flipped[at] ^ 0x55);
		WriteFile(dir + "/flip.zip", flipped);
		CHECK(!SpectraZip::Extract(P(dir + "/flip.zip"), P(dir + "/out3")).empty());

		WriteFile(dir + "/html.zip", "<html>Not Found</html>");
		CHECK(!SpectraZip::Extract(P(dir + "/html.zip"), P(dir + "/out4")).empty());
		CHECK(!SpectraZip::Extract(P(dir + "/missing.zip"), P(dir + "/out5")).empty());
	});

	Test("zip: cancel", []() {
		QString dir = Dir("zip-cancel");
		WriteFile(dir + "/p.zip", MakeZip(PackageEntries("x")));
		std::string err = SpectraZip::Extract(P(dir + "/p.zip"), P(dir + "/out"),
						      [](uint64_t d, uint64_t) { return d == 0; });
		CHECK(err == "cancelled");
	});
}

/* --- the job and the helper, end to end on local files -------------------------- */

struct JobResult {
	bool finished = false;
	bool ok = false;
	QString error;
	QList<SpectraUpdateJob::Stage> stages;
};

static JobResult RunJob(SpectraUpdateJob &job, int timeoutMs = 600000,
			const std::function<void(SpectraUpdateJob::Stage)> &onStage = nullptr)
{
	JobResult result;
	QEventLoop loop;
	QObject::connect(&job, &SpectraUpdateJob::Progress, &loop, [&](SpectraUpdateJob::Stage stage, qint64, qint64) {
		if (result.stages.isEmpty() || result.stages.last() != stage) {
			result.stages.append(stage);
			if (onStage) {
				onStage(stage);
			}
		}
	});
	QObject::connect(&job, &SpectraUpdateJob::Finished, &loop, [&](bool ok, const QString &error) {
		result.finished = true;
		result.ok = ok;
		result.error = error;
		loop.quit();
	});
	QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
	job.Start();
	if (!result.finished) {
		loop.exec();
	}
	return result;
}

static QString FileUrl(const QString &path)
{
	return QUrl::fromLocalFile(path).toString();
}

/* A fake install: an obs-spectra.exe that is only a text file, a config folder and user files */
static QString FakeInstall(const QString &root, bool portable)
{
	QString install = QDir(root).filePath("OBS Spectra");
	WriteFile(install + "/bin/64bit/obs-spectra.exe", "old exe");
	WriteFile(install + "/bin/64bit/old-only.dll", "removed in the new version");
	WriteFile(install + "/data/obs-studio/old.txt", "old data");
	WriteFile(install + "/config/obs-studio/global.ini", "[General]\nkeep=me\n");
	WriteFile(install + "/config/obs-studio/basic/profiles/Untitled/basic.ini", "profile");
	WriteFile(install + "/my notes.txt", "user file");
	WriteFile(install + "/recordings/clip.mp4", "user folder");
	WriteFile(install + "/Update-Portable.ps1", "old script");
	if (portable) {
		WriteFile(install + "/portable_mode.txt", "");
	}
	return install;
}

static SpectraReleases::Download LocalDownload(const QString &dir, const QByteArray &zip, const QString &hash)
{
	WriteFile(dir + "/OBS-Spectra-9.9.9-Windows-x64-Portable.zip", zip);
	WriteFile(dir + "/OBS-Spectra-9.9.9-Windows-x64-Portable.zip.sha256",
		  (hash + "  OBS-Spectra-9.9.9-Windows-x64-Portable.zip\n").toUtf8());
	SpectraReleases::Download d;
	d.zip = {"OBS-Spectra-9.9.9-Windows-x64-Portable.zip",
		 FileUrl(dir + "/OBS-Spectra-9.9.9-Windows-x64-Portable.zip"), zip.size()};
	d.checksum = {"OBS-Spectra-9.9.9-Windows-x64-Portable.zip.sha256",
		      FileUrl(dir + "/OBS-Spectra-9.9.9-Windows-x64-Portable.zip.sha256"), 0};
	d.portable = true;
	return d;
}

static QString Sha256(const QByteArray &data)
{
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

static bool WaitFor(const std::function<bool()> &cond, int ms)
{
	QElapsedTimer t;
	t.start();
	while (!cond()) {
		if (t.elapsed() > ms) {
			return false;
		}
		QThread::msleep(100);
	}
	return true;
}

static void TestJob()
{
	QString helper = QDir(QCoreApplication::applicationDirPath()).filePath("spectra-updater.exe");

	Test("job: download, verify, extract, then the helper swaps and keeps settings", [&]() {
		QString root = Dir("job-ok");
		QString install = FakeInstall(root, false); /* installed-style: the portable zip's marker must go */
		QByteArray zip = MakeZip(PackageEntries("new exe"));
		SpectraUpdateJob job(install, LocalDownload(root, zip, Sha256(zip)));
		JobResult r = RunJob(job);
		CHECK(r.finished && r.ok);
		if (!r.ok) {
			fprintf(stderr, "  %s\n", qPrintable(r.error));
			return;
		}
		CHECK(r.stages.contains(SpectraUpdateJob::Stage::Downloading));
		CHECK(r.stages.contains(SpectraUpdateJob::Stage::Extracting));
		CHECK(r.stages.last() == SpectraUpdateJob::Stage::Ready);
		CHECK(ReadFile(job.StagingDir() + "/bin/64bit/obs-spectra.exe") == "new exe");
		CHECK(!QFileInfo::exists(job.WorkDir() + "/OBS-Spectra-9.9.9-Windows-x64-Portable.zip"));
		CHECK(QFileInfo::exists(job.WorkDir() + "/spectra-update.txt"));
		/* the install is untouched until the helper runs */
		CHECK(ReadFile(install + "/bin/64bit/obs-spectra.exe") == "old exe");

		if (!QFileInfo::exists(helper)) {
			fprintf(stderr, "  no %s, skipping the swap\n", qPrintable(helper));
			failures++;
			return;
		}
		job.helperExtraArgs = {"--quiet", "--no-relaunch"};
		QString error;
		CHECK(job.LaunchHelper(helper, "9.9.9", "--portable", &error));
		CHECK(WaitFor([&]() { return ReadFile(install + "/bin/64bit/obs-spectra.exe") == "new exe"; }, 30000));
		CHECK(WaitFor([&]() { return ReadFile(job.LogPath()).contains("Exit code 0"); }, 30000));

		CHECK(ReadFile(install + "/config/obs-studio/global.ini") == "[General]\nkeep=me\n");
		CHECK(ReadFile(install + "/config/obs-studio/basic/profiles/Untitled/basic.ini") == "profile");
		CHECK(ReadFile(install + "/my notes.txt") == "user file");
		CHECK(ReadFile(install + "/recordings/clip.mp4") == "user folder");
		CHECK(ReadFile(install + "/bin/64bit/lucida-viewer.exe") == "viewer");
		CHECK(!QFileInfo::exists(install + "/bin/64bit/old-only.dll"));
		CHECK(!QFileInfo::exists(install + "/data/obs-studio/old.txt"));
		CHECK(!QFileInfo::exists(install + "/Update-Portable.ps1"));
		CHECK(!QFileInfo::exists(install + "/portable_mode.txt")); /* stays installed-style */
		/* only the helper's own copy may be left in the work folder while it exits */
		CHECK(WaitFor([&]() { return !QFileInfo::exists(job.WorkDir() + "/old"); }, 30000));
		CHECK(!QFileInfo::exists(job.StagingDir()));
		WaitFor(
			[&]() {
				return !QFileInfo::exists(job.WorkDir() + "/spectra-updater.exe") ||
				       QFile::remove(job.WorkDir() + "/spectra-updater.exe");
			},
			10000);
		SpectraUpdateJob::CleanUpLeftovers(install);
		CHECK(!QFileInfo::exists(job.WorkDir()));
		CHECK(ReadFile(job.LogPath()).contains("Kept config"));
	});

	Test("job: a portable install stays portable", [&]() {
		QString root = Dir("job-portable");
		QString install = FakeInstall(root, true);
		std::vector<ZipEntry> entries = PackageEntries("new exe");
		entries.pop_back(); /* a regular zip: no portable_mode.txt */
		QByteArray zip = MakeZip(entries);
		SpectraUpdateJob job(install, LocalDownload(root, zip, Sha256(zip)));
		JobResult r = RunJob(job);
		CHECK(r.ok);
		job.helperExtraArgs = {"--quiet", "--no-relaunch"};
		QString error;
		CHECK(r.ok && job.LaunchHelper(helper, "9.9.9", QString(), &error));
		CHECK(WaitFor([&]() { return ReadFile(job.LogPath()).contains("Exit code 0"); }, 30000));
		CHECK(QFileInfo::exists(install + "/portable_mode.txt"));
		CHECK(ReadFile(install + "/bin/64bit/obs-spectra.exe") == "new exe");
	});

	Test("job: a checksum mismatch changes nothing", []() {
		QString root = Dir("job-mismatch");
		QString install = FakeInstall(root, true);
		QByteArray zip = MakeZip(PackageEntries("new exe"));
		SpectraUpdateJob job(install, LocalDownload(root, zip, Sha256("something else")));
		JobResult r = RunJob(job);
		CHECK(r.finished && !r.ok && r.error.contains("checksum"));
		CHECK(!QFileInfo::exists(job.WorkDir()));
		CHECK(ReadFile(install + "/bin/64bit/obs-spectra.exe") == "old exe");
	});

	Test("job: no checksum file, no update", []() {
		QString root = Dir("job-nosum");
		QString install = FakeInstall(root, true);
		QByteArray zip = MakeZip(PackageEntries("new exe"));
		SpectraReleases::Download d = LocalDownload(root, zip, Sha256(zip));
		d.checksum = {};
		SpectraUpdateJob job(install, d);
		JobResult r = RunJob(job);
		CHECK(!r.ok && r.error.contains("checksum"));
		CHECK(!QFileInfo::exists(job.WorkDir()));
	});

	Test("job: a zip that isn't a package", []() {
		QString root = Dir("job-notpkg");
		QString install = FakeInstall(root, true);
		QByteArray zip = MakeZip({{"readme.txt", "hi"}});
		SpectraUpdateJob job(install, LocalDownload(root, zip, Sha256(zip)));
		JobResult r = RunJob(job);
		CHECK(!r.ok && r.error.contains("obs-spectra.exe"));
		CHECK(!QFileInfo::exists(job.WorkDir()));
	});

	Test("job: cancel", []() {
		QString root = Dir("job-cancel");
		QString install = FakeInstall(root, true);
		QByteArray zip = MakeZip(PackageEntries("new exe"));
		SpectraUpdateJob job(install, LocalDownload(root, zip, Sha256(zip)));
		JobResult r = RunJob(job, 600000, [&](SpectraUpdateJob::Stage s) {
			if (s == SpectraUpdateJob::Stage::Preparing) {
				job.Cancel();
			}
		});
		CHECK(r.finished && !r.ok && r.error == "cancelled");
		CHECK(!QFileInfo::exists(job.WorkDir()));
	});

	Test("job: leftovers from an earlier update", []() {
		QString root = Dir("job-leftover");
		QString install = FakeInstall(root, true);
		QByteArray zip = MakeZip(PackageEntries("new exe"));
		SpectraReleases::Download d = LocalDownload(root, zip, Sha256(zip));

		/* a folder of the same name that isn't Spectra's is never deleted */
		WriteFile(install + ".update/mine.txt", "not Spectra's");
		{
			SpectraUpdateJob job(install, d);
			JobResult r = RunJob(job);
			CHECK(!r.ok && r.error.contains("in the way"));
			CHECK(ReadFile(install + ".update/mine.txt") == "not Spectra's");
		}
		SpectraUpdateJob::CleanUpLeftovers(install);
		CHECK(QFileInfo::exists(install + ".update/mine.txt"));

		/* a backup that holds settings (a failed rollback) is never deleted */
		QDir(install + ".update").removeRecursively();
		WriteFile(install + ".update/spectra-update.txt", "marker");
		WriteFile(install + ".update/old/config/global.ini", "rescued settings");
		{
			SpectraUpdateJob job(install, d);
			JobResult r = RunJob(job);
			CHECK(!r.ok && r.error.contains("earlier update"));
		}
		SpectraUpdateJob::CleanUpLeftovers(install);
		CHECK(ReadFile(install + ".update/old/config/global.ini") == "rescued settings");

		/* program files left by a finished update are cleaned up */
		QDir(install + ".update").removeRecursively();
		WriteFile(install + ".update/spectra-update.txt", "marker");
		WriteFile(install + ".update/old/bin/64bit/obs-spectra.exe", "leftover");
		SpectraUpdateJob::CleanUpLeftovers(install);
		CHECK(!QFileInfo::exists(install + ".update"));
		{
			WriteFile(install + ".update/spectra-update.txt", "marker");
			WriteFile(install + ".update/old/bin/64bit/obs-spectra.exe", "leftover");
			SpectraUpdateJob job(install, d);
			JobResult r = RunJob(job);
			CHECK(r.ok);
			CHECK(!QFileInfo::exists(install + ".update/old"));
			job.Discard();
			CHECK(!QFileInfo::exists(install + ".update"));
		}
	});
}

/* --- optional: real releases ------------------------------------------------------ */

static void TestRealZip(const QString &zip)
{
	Test("zip: a real release zip", [&]() {
		QString dir = Dir("real-zip");
		QElapsedTimer t;
		t.start();
		std::string err = SpectraZip::Extract(P(zip), P(dir));
		CHECK(err.empty());
		if (!err.empty()) {
			fprintf(stderr, "  %s\n", err.c_str());
		}
		CHECK(QFileInfo::exists(dir + "/bin/64bit/obs-spectra.exe"));
		printf("     extracted in %.1f s\n", t.elapsed() / 1000.0);
	});
}

static void TestNetwork(const QString &channelName)
{
	Test("network: newest release from GitHub", [&]() {
		/* the list the way SpectraUpdateCheck gets it: libcurl */
		std::string body;
		CURL *curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL,
				 "https://api.github.com/repos/karlockhart/obs-spectra/releases?per_page=30");
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "OBS-Spectra-Updater-Tests");
		curl_easy_setopt(
			curl, CURLOPT_WRITEFUNCTION, +[](char *p, size_t s, size_t n, void *ud) {
				static_cast<std::string *>(ud)->append(p, s * n);
				return s * n;
			});
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
		CURLcode rc = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		CHECK(rc == CURLE_OK);
		QString error;
		QList<Release> releases = ParseReleases(QByteArray::fromStdString(body), &error);
		CHECK(!releases.isEmpty());
		Channel channel = ChannelFromSetting(channelName, QString());
		std::optional<Release> release = SelectRelease(releases, channel);
		CHECK(release.has_value());
		if (!release) {
			return;
		}
		printf("     %s channel: %s\n", qPrintable(channelName), qPrintable(release->tag));
		std::optional<Download> d = SelectDownload(*release, true);
		CHECK(d && d->portable && !d->checksum.name.isEmpty());
		if (!d) {
			return;
		}

		QString root = Dir("net");
		QString install = QDir(root).filePath("install");
		WriteFile(install + "/bin/64bit/obs-spectra.exe", "fake");
		SpectraUpdateJob job(install, *d);
		qint64 lastMb = -1;
		QObject::connect(&job, &SpectraUpdateJob::Progress, &job,
				 [&](SpectraUpdateJob::Stage stage, qint64 done, qint64 total) {
					 if (stage == SpectraUpdateJob::Stage::Downloading &&
					     done / (50 << 20) != lastMb) {
						 lastMb = done / (50 << 20);
						 printf("     %lld / %lld MB\n", done >> 20, total >> 20);
						 fflush(stdout);
					 }
				 });
		QElapsedTimer t;
		t.start();
		JobResult r = RunJob(job, 1800000);
		CHECK(r.ok);
		if (!r.ok) {
			fprintf(stderr, "  %s\n", qPrintable(r.error));
		}
		printf("     downloaded, verified and extracted %s in %.1f s to %s\n", qPrintable(d->zip.name),
		       t.elapsed() / 1000.0, qPrintable(QDir::toNativeSeparators(job.StagingDir())));
		CHECK(QFileInfo::exists(job.StagingDir() + "/bin/64bit/obs-spectra.exe"));
		CHECK(QFileInfo::exists(job.StagingDir() + "/portable_mode.txt"));
	});
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	curl_global_init(CURL_GLOBAL_ALL);
	QStringList args = app.arguments();
	if (args.size() < 2) {
		fprintf(stderr,
			"usage: spectra-update-tests <scratch dir> [--zip <release zip>] [--network <channel>]\n");
		return 2;
	}
	scratch = QDir(args[1]).absolutePath();
	QDir().mkpath(scratch);

	QString realZip, network;
	for (int i = 2; i + 1 < args.size(); i++) {
		if (args[i] == "--zip") {
			realZip = args[++i];
		} else if (args[i] == "--network") {
			network = args[++i];
		}
	}

	TestVersions();
	TestReleases();
	TestZip();
	TestJob();
	if (!realZip.isEmpty()) {
		TestRealZip(realZip);
	}
	if (!network.isEmpty()) {
		TestNetwork(network);
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
