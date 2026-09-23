cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_SOURCE_DIR}/cmake/spectra/SpectraDeps.cmake")

if(NOT TARGET spectra-vision)
  target_disable(spectra-obscura)
  return()
endif()

# Monocypher 4.0.2 (CC0/BSD-2): Ed25519 for Obscura's signed definitions
spectra_fetch(
  monocypher
  URL https://monocypher.org/download/monocypher-4.0.2.tar.gz
  SHA256 38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
  EXTRACT
)
set(monocypher_root "${SPECTRA_MONOCYPHER_PATH}/monocypher-4.0.2/src")
add_library(spectra-monocypher STATIC "${monocypher_root}/monocypher.c" "${monocypher_root}/optional/monocypher-ed25519.c")
target_include_directories(spectra-monocypher PUBLIC "${monocypher_root}" "${monocypher_root}/optional")
set_target_properties(spectra-monocypher PROPERTIES FOLDER spectra COMPILE_WARNING_AS_ERROR OFF LINKER_LANGUAGE C)
if(MSVC)
  target_compile_options(spectra-monocypher PRIVATE /W0)
endif()

find_package(ZLIB REQUIRED)
find_package(Qt6 REQUIRED Core Gui Widgets)

# Obscura's logic: config, learner, signed definitions, output naming
add_library(obscura-core STATIC)
target_sources(
  obscura-core
  PRIVATE
    core/definitions.cpp
    core/definitions.hpp
    core/learner.cpp
    core/learner.hpp
    core/naming.cpp
    core/naming.hpp
    core/obscura-config.cpp
    core/obscura-config.hpp
)
target_include_directories(obscura-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_link_libraries(obscura-core PUBLIC spectra-vision Qt::Core PRIVATE spectra-monocypher ZLIB::ZLIB)
target_compile_features(obscura-core PUBLIC cxx_std_20)
set_target_properties(obscura-core PROPERTIES FOLDER spectra)

# Parity tests against Obscura's Python code: obscura-tests <tests/data> <scratch dir>
add_executable(obscura-tests tests/obscura-tests.cpp)
target_link_libraries(obscura-tests PRIVATE obscura-core)
set_target_properties(obscura-tests PROPERTIES FOLDER spectra)
set_target_properties_obs(obscura-tests)

# The plugin: hotkey/region capture, review window, dock
add_library(spectra-obscura MODULE)
add_library(OBS::spectra-obscura ALIAS spectra-obscura)
target_sources(
  spectra-obscura
  PRIVATE
    plugin/capture-win.cpp
    plugin/capture-win.hpp
    plugin/obscura-controller.cpp
    plugin/obscura-controller.hpp
    plugin/obscura-dock.cpp
    plugin/obscura-dock.hpp
    plugin/obscura-plugin.cpp
    plugin/review-window.cpp
    plugin/review-window.hpp
    plugin/settings-dialog.cpp
    plugin/settings-dialog.hpp
    plugin/snip-overlay.cpp
    plugin/snip-overlay.hpp
)
target_link_libraries(
  spectra-obscura
  PRIVATE obscura-core spectra-censor spectra-grab OBS::libobs OBS::frontend-api Qt::Widgets
)
set_target_properties(spectra-obscura PROPERTIES AUTOMOC ON)
set_target_properties_obs(spectra-obscura PROPERTIES FOLDER spectra PREFIX "")
