cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_SOURCE_DIR}/cmake/spectra/SpectraDeps.cmake")

# SQLite 3.53.4 amalgamation (public domain), built with FTS5
spectra_fetch(
  sqlite
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  SHA256 1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
  EXTRACT
)
set(sqlite_root "${SPECTRA_SQLITE_PATH}/sqlite-amalgamation-3530400")

add_library(spectra-sqlite STATIC "${sqlite_root}/sqlite3.c" "${sqlite_root}/sqlite3.h")
target_include_directories(spectra-sqlite PUBLIC "${sqlite_root}")
target_compile_definitions(
  spectra-sqlite
  PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=0
)
set_target_properties(spectra-sqlite PROPERTIES FOLDER spectra COMPILE_WARNING_AS_ERROR OFF POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(spectra-sqlite PRIVATE /W0)
endif()

find_package(Qt6 REQUIRED Core Gui Widgets)

# Lucida's logic (store, sampling loop), shared by the plugin and the tests
add_library(lucida-core STATIC)
target_sources(lucida-core PRIVATE core/recorder.cpp core/recorder.hpp core/store.cpp core/store.hpp)
target_include_directories(lucida-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_link_libraries(lucida-core PUBLIC spectra-vision spectra-sqlite Qt::Core Qt::Gui)
target_compile_features(lucida-core PUBLIC cxx_std_20)
set_target_properties(lucida-core PROPERTIES FOLDER spectra)

# Tests (ports of Lucida's store and recorder tests): lucida-tests <scratch dir>
add_executable(lucida-tests tests/lucida-tests.cpp)
target_link_libraries(lucida-tests PRIVATE lucida-core)
set_target_properties(lucida-tests PROPERTIES FOLDER spectra)
set_target_properties_obs(lucida-tests)

# The plugin: sampling loop inside Spectra and the "Chat Log" dock
add_library(spectra-lucida MODULE)
add_library(OBS::spectra-lucida ALIAS spectra-lucida)
target_sources(
  spectra-lucida
  PRIVATE
    plugin/frame-grabber.cpp
    plugin/frame-grabber.hpp
    plugin/lucida-controller.cpp
    plugin/lucida-controller.hpp
    plugin/lucida-dock.cpp
    plugin/lucida-dock.hpp
    plugin/lucida-plugin.cpp
    plugin/lucida-viewer.cpp
    plugin/lucida-viewer.hpp
)
target_link_libraries(spectra-lucida PRIVATE lucida-core spectra-censor OBS::libobs OBS::frontend-api Qt::Widgets)
set_target_properties(spectra-lucida PROPERTIES AUTOMOC ON)
set_target_properties_obs(spectra-lucida PROPERTIES FOLDER spectra PREFIX "")
