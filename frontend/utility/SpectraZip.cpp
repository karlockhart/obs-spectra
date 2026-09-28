#include "SpectraZip.hpp"

#include <zlib.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace SpectraZip {

namespace {

constexpr uint32_t EOCD_SIG = 0x06054b50;
constexpr uint32_t ZIP64_LOCATOR_SIG = 0x07064b50;
constexpr uint32_t ZIP64_EOCD_SIG = 0x06064b50;
constexpr uint32_t CENTRAL_SIG = 0x02014b50;
constexpr uint32_t LOCAL_SIG = 0x04034b50;
constexpr size_t CHUNK = 1 << 20;

uint16_t U16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t U32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t U64(const unsigned char *p)
{
	return (uint64_t)U32(p) | ((uint64_t)U32(p + 4) << 32);
}

struct Entry {
	std::string name;
	uint16_t flags = 0;
	uint16_t method = 0;
	uint32_t crc = 0;
	uint64_t compressed = 0;
	uint64_t size = 0;
	uint64_t offset = 0;
};

bool ReadAt(std::ifstream &in, uint64_t offset, void *buf, size_t len)
{
	in.clear();
	in.seekg((std::streamoff)offset, std::ios::beg);
	in.read((char *)buf, (std::streamsize)len);
	return (size_t)in.gcount() == len;
}

/* The path an entry extracts to, or empty when its name is unsafe */
fs::path SafePath(const fs::path &dest, const std::string &name, bool utf8)
{
	std::string clean = name;
	std::replace(clean.begin(), clean.end(), '\\', '/');
	if (clean.empty() || clean[0] == '/' || clean.find(':') != std::string::npos) {
		return {};
	}

	fs::path result = dest;
	size_t start = 0;
	while (start <= clean.size()) {
		size_t end = clean.find('/', start);
		if (end == std::string::npos) {
			end = clean.size();
		}
		std::string part = clean.substr(start, end - start);
		if (part == "..") {
			return {};
		}
		if (!part.empty() && part != ".") {
			/* names are UTF-8 when flagged, else code page 437 */
			UINT codePage = utf8 ? CP_UTF8 : 437;
			int len = MultiByteToWideChar(codePage, 0, part.data(), (int)part.size(), nullptr, 0);
			std::wstring wide((size_t)len, L' ');
			MultiByteToWideChar(codePage, 0, part.data(), (int)part.size(), wide.data(), len);
			if (wide.empty()) {
				return {};
			}
			result /= fs::path(wide);
		}
		start = end + 1;
	}
	return result == dest ? fs::path() : result;
}

std::string ReadCentralDirectory(std::ifstream &in, uint64_t fileSize, std::vector<Entry> &entries)
{
	/* the end of central directory record is in the last 64 KiB + 22 bytes */
	uint64_t tail = std::min<uint64_t>(fileSize, 65535 + 22);
	std::vector<unsigned char> buf((size_t)tail);
	if (tail < 22 || !ReadAt(in, fileSize - tail, buf.data(), buf.size())) {
		return "not a zip file";
	}
	ptrdiff_t eocd = -1;
	for (ptrdiff_t i = (ptrdiff_t)tail - 22; i >= 0; i--) {
		if (U32(&buf[i]) == EOCD_SIG) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		return "not a zip file (no end of central directory)";
	}

	const unsigned char *e = &buf[eocd];
	uint64_t count = U16(e + 10);
	uint64_t cdSize = U32(e + 12);
	uint64_t cdOffset = U32(e + 16);

	if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF) {
		uint64_t eocdPos = fileSize - tail + (uint64_t)eocd;
		unsigned char loc[20];
		if (eocdPos < 20 || !ReadAt(in, eocdPos - 20, loc, sizeof(loc)) || U32(loc) != ZIP64_LOCATOR_SIG) {
			return "broken zip64 archive (no locator)";
		}
		unsigned char z[56];
		if (!ReadAt(in, U64(loc + 8), z, sizeof(z)) || U32(z) != ZIP64_EOCD_SIG) {
			return "broken zip64 archive (no end of central directory)";
		}
		count = U64(z + 32);
		cdSize = U64(z + 40);
		cdOffset = U64(z + 48);
	}

	if (cdOffset + cdSize > fileSize || cdSize > (256ull << 20)) {
		return "broken zip file (central directory out of range)";
	}
	std::vector<unsigned char> cd((size_t)cdSize);
	if (!ReadAt(in, cdOffset, cd.data(), cd.size())) {
		return "broken zip file (can't read the central directory)";
	}

	size_t pos = 0;
	for (uint64_t i = 0; i < count; i++) {
		if (pos + 46 > cd.size() || U32(&cd[pos]) != CENTRAL_SIG) {
			return "broken zip file (bad central directory entry)";
		}
		const unsigned char *h = &cd[pos];
		Entry entry;
		entry.flags = U16(h + 8);
		entry.method = U16(h + 10);
		entry.crc = U32(h + 16);
		entry.compressed = U32(h + 20);
		entry.size = U32(h + 24);
		uint16_t nameLen = U16(h + 28);
		uint16_t extraLen = U16(h + 30);
		uint16_t commentLen = U16(h + 32);
		entry.offset = U32(h + 42);
		if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) {
			return "broken zip file (central directory entry too long)";
		}
		entry.name.assign((const char *)h + 46, nameLen);

		/* zip64 extra field: the 64-bit values of whichever fields are 0xFFFFFFFF */
		const unsigned char *extra = h + 46 + nameLen;
		size_t x = 0;
		while (x + 4 <= extraLen) {
			uint16_t id = U16(extra + x);
			uint16_t len = U16(extra + x + 2);
			if (x + 4 + len > extraLen) {
				break;
			}
			if (id == 0x0001) {
				const unsigned char *f = extra + x + 4;
				size_t left = len;
				auto take = [&](uint64_t &field) {
					if (field == 0xFFFFFFFF && left >= 8) {
						field = U64(f);
						f += 8;
						left -= 8;
					}
				};
				take(entry.size);
				take(entry.compressed);
				take(entry.offset);
			}
			x += 4 + len;
		}

		entries.push_back(std::move(entry));
		pos += 46 + nameLen + extraLen + commentLen;
	}
	return {};
}

} // namespace

std::string Extract(const fs::path &zip, const fs::path &dest, const Progress &progress)
{
	std::ifstream in(zip, std::ios::binary);
	if (!in) {
		return "can't open the zip file";
	}
	in.seekg(0, std::ios::end);
	uint64_t fileSize = (uint64_t)in.tellg();

	std::vector<Entry> entries;
	std::string error = ReadCentralDirectory(in, fileSize, entries);
	if (!error.empty()) {
		return error;
	}

	std::error_code ec;
	fs::create_directories(dest, ec);
	if (ec) {
		return "can't create the folder to extract to: " + ec.message();
	}

	uint64_t total = 0;
	for (const Entry &entry : entries) {
		total += entry.size;
	}
	uint64_t done = 0;
	if (progress && !progress(done, total)) {
		return "cancelled";
	}

	std::vector<unsigned char> inBuf(CHUNK);
	std::vector<unsigned char> outBuf(CHUNK);

	for (const Entry &entry : entries) {
		bool utf8 = (entry.flags & 0x800) != 0;
		fs::path target = SafePath(dest, entry.name, utf8);
		if (target.empty()) {
			return "unsafe name in the zip: " + entry.name;
		}
		if (entry.flags & 0x1) {
			return "encrypted entry in the zip: " + entry.name;
		}

		bool isDir = !entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\');
		if (isDir) {
			fs::create_directories(target, ec);
			if (ec) {
				return "can't create " + entry.name + ": " + ec.message();
			}
			continue;
		}
		if (entry.method != 0 && entry.method != 8) {
			return "unsupported compression in the zip: " + entry.name;
		}

		unsigned char local[30];
		if (!ReadAt(in, entry.offset, local, sizeof(local)) || U32(local) != LOCAL_SIG) {
			return "broken zip file (bad local header for " + entry.name + ")";
		}
		uint64_t dataPos = entry.offset + 30 + U16(local + 26) + U16(local + 28);
		if (dataPos + entry.compressed > fileSize) {
			return "broken zip file (" + entry.name + " is cut short)";
		}

		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			return "can't create the folder for " + entry.name + ": " + ec.message();
		}
		std::ofstream out(target, std::ios::binary | std::ios::trunc);
		if (!out) {
			return "can't write " + entry.name;
		}

		in.clear();
		in.seekg((std::streamoff)dataPos, std::ios::beg);

		z_stream zs = {};
		bool deflated = entry.method == 8;
		if (deflated && inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
			return "zlib failed to start";
		}
		std::unique_ptr<z_stream, void (*)(z_stream *)> guard(deflated ? &zs : nullptr,
								      [](z_stream *s) { inflateEnd(s); });

		uint32_t crc = crc32(0L, Z_NULL, 0);
		uint64_t written = 0;
		uint64_t left = entry.compressed;
		bool ended = !deflated && left == 0;

		while (left > 0 || (deflated && !ended)) {
			size_t want = (size_t)std::min<uint64_t>(left, inBuf.size());
			if (want) {
				in.read((char *)inBuf.data(), (std::streamsize)want);
				if ((size_t)in.gcount() != want) {
					return "broken zip file (can't read " + entry.name + ")";
				}
				left -= want;
			}

			if (!deflated) {
				crc = crc32(crc, inBuf.data(), (uInt)want);
				out.write((const char *)inBuf.data(), (std::streamsize)want);
				written += want;
				done += want;
			} else {
				zs.next_in = inBuf.data();
				zs.avail_in = (uInt)want;
				do {
					zs.next_out = outBuf.data();
					zs.avail_out = (uInt)outBuf.size();
					int rc = inflate(&zs, Z_NO_FLUSH);
					if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
						return "broken compressed data in " + entry.name;
					}
					size_t got = outBuf.size() - zs.avail_out;
					crc = crc32(crc, outBuf.data(), (uInt)got);
					out.write((const char *)outBuf.data(), (std::streamsize)got);
					written += got;
					done += got;
					if (rc == Z_STREAM_END) {
						ended = true;
						break;
					}
					if (rc == Z_BUF_ERROR && zs.avail_in == 0 && left == 0) {
						return "broken compressed data in " + entry.name + " (cut short)";
					}
				} while (zs.avail_out == 0 || zs.avail_in > 0);
			}

			if (!out) {
				return "can't write " + entry.name + " (disk full?)";
			}
			if (progress && !progress(done, total)) {
				return "cancelled";
			}
			if (ended) {
				break;
			}
		}

		out.close();
		if (!out) {
			return "can't write " + entry.name + " (disk full?)";
		}
		if (written != entry.size || crc != entry.crc) {
			return "the zip is damaged: " + entry.name + " doesn't match its checksum";
		}
	}

	if (progress && !progress(total, total)) {
		return "cancelled";
	}
	return {};
}

} // namespace SpectraZip
