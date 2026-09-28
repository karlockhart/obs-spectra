#pragma once

/*
 * Extracts a zip (stored or deflated entries, zip64 aware) into a folder, for
 * the in-app updater. C++, zlib and Win32 (for file names), so
 * spectra-update-tests can test it.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace SpectraZip {

/* Called as bytes are written: total is the sum of all entries' sizes.
 * Returning false cancels the extraction. */
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

/* Extracts every entry of zip into dest, which is created if needed. Entry
 * names that would land outside dest (absolute, "..", drive letters) fail the
 * whole extraction, as does a CRC or size mismatch. Returns an empty string on
 * success, else what went wrong ("cancelled" when progress returned false). */
std::string Extract(const std::filesystem::path &zip, const std::filesystem::path &dest,
		    const Progress &progress = nullptr);

} // namespace SpectraZip
