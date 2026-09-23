# Spectra third-party dependencies
#
# Prebuilt binaries and model files are downloaded at configure time (like
# OBS's own prebuilt dependencies) and verified against pinned SHA-256 hashes,
# instead of being committed to the repository.

include_guard(GLOBAL)

# spectra_fetch(<name> URL <url> SHA256 <hash> [EXTRACT])
#   Downloads <url> into .deps/spectra/<file>. With EXTRACT, the archive is
#   unpacked into .deps/spectra/<name>. Sets SPECTRA_<NAME>_PATH to the file
#   (or the extracted directory).
function(spectra_fetch name)
  cmake_parse_arguments(PARSE_ARGV 1 _SF "EXTRACT" "URL;SHA256" "")

  # Worked out here rather than at include time: include_guard(GLOBAL) means
  # only the first directory to include this file would see a variable set there.
  set(SPECTRA_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps/spectra")

  cmake_path(GET _SF_URL FILENAME file_name)
  set(file_path "${SPECTRA_DEPS_DIR}/${file_name}")
  string(TOUPPER "${name}" upper_name)
  string(REPLACE "-" "_" upper_name "${upper_name}")

  if(EXISTS "${file_path}")
    file(SHA256 "${file_path}" existing_hash)
    if(NOT existing_hash STREQUAL _SF_SHA256)
      file(REMOVE "${file_path}")
    endif()
  endif()

  if(NOT EXISTS "${file_path}")
    message(STATUS "Downloading ${_SF_URL}")
    file(
      DOWNLOAD "${_SF_URL}"
      "${file_path}.tmp"
      STATUS download_status
      EXPECTED_HASH SHA256=${_SF_SHA256}
      TLS_VERIFY ON
    )
    list(GET download_status 0 error_code)
    if(error_code)
      file(REMOVE "${file_path}.tmp")
      list(GET download_status 1 error_message)
      message(FATAL_ERROR "Unable to download ${_SF_URL}: ${error_message}")
    endif()
    file(RENAME "${file_path}.tmp" "${file_path}")
  endif()

  if(_SF_EXTRACT)
    set(extract_dir "${SPECTRA_DEPS_DIR}/${name}")
    set(stamp "${extract_dir}/.sha256")
    set(stamp_hash "")
    if(EXISTS "${stamp}")
      file(READ "${stamp}" stamp_hash)
    endif()
    if(NOT stamp_hash STREQUAL _SF_SHA256)
      file(REMOVE_RECURSE "${extract_dir}")
      file(ARCHIVE_EXTRACT INPUT "${file_path}" DESTINATION "${extract_dir}")
      file(WRITE "${stamp}" "${_SF_SHA256}")
    endif()
    set(SPECTRA_${upper_name}_PATH "${extract_dir}" PARENT_SCOPE)
  else()
    set(SPECTRA_${upper_name}_PATH "${file_path}" PARENT_SCOPE)
  endif()
endfunction()
