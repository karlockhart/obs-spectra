cmake_minimum_required(VERSION 3.28...3.30)

# spectra-updater.exe: swaps a downloaded OBS-Spectra version into an install after Spectra exits.
# Win32 and the static C runtime only, so nothing it loads lives in the folder it replaces.
add_executable(spectra-updater WIN32)
set(SPECTRA_UPDATER_ICON "${CMAKE_SOURCE_DIR}/frontend/cmake/windows/obs-studio.ico")
configure_file(spectra-updater.rc.in spectra-updater.rc)
target_sources(
  spectra-updater
  PRIVATE
    spectra-updater.cpp
    spectra-updater.manifest
    updater-common.hpp
    "${CMAKE_CURRENT_BINARY_DIR}/spectra-updater.rc"
)
target_compile_definitions(spectra-updater PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(spectra-updater PRIVATE comctl32 shell32)
set_target_properties(
  spectra-updater
  PROPERTIES FOLDER spectra MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
set_target_properties_obs(spectra-updater)

# Tests of the in-app updater: spectra-update-tests <scratch dir> [--zip <release zip>] [--network <channel>]
find_package(Qt6 REQUIRED Core)
find_package(ZLIB REQUIRED)
add_executable(spectra-update-tests)
target_sources(
  spectra-update-tests
  PRIVATE
    tests/spectra-update-tests.cpp
    ../utility/SpectraReleases.cpp
    ../utility/SpectraReleases.hpp
    ../utility/SpectraUpdateJob.cpp
    ../utility/SpectraUpdateJob.hpp
    ../utility/SpectraZip.cpp
    ../utility/SpectraZip.hpp
    updater-common.hpp
)
target_compile_definitions(spectra-update-tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(spectra-update-tests PRIVATE Qt::Core ZLIB::ZLIB CURL::libcurl)
set_target_properties(spectra-update-tests PROPERTIES AUTOMOC ON FOLDER spectra)
set_target_properties_obs(spectra-update-tests)
