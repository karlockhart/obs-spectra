cmake_minimum_required(VERSION 3.28...3.30)

# OBS-Spectra editions: Full; Lite, without Lucida, Obscura, OCR and speech
# recognition; Ultralight, Lite without the browser (CEF): no browser sources
# or docks. Set before the bootstrap, which skips downloading CEF without it.
set(SPECTRA_EDITION Full CACHE STRING "OBS-Spectra edition: Full, Lite or Ultralight")
set_property(CACHE SPECTRA_EDITION PROPERTY STRINGS Full Lite Ultralight)
if(NOT SPECTRA_EDITION MATCHES "^(Full|Lite|Ultralight)$")
  message(FATAL_ERROR "SPECTRA_EDITION must be Full, Lite or Ultralight, not '${SPECTRA_EDITION}'")
endif()
if(SPECTRA_EDITION STREQUAL "Ultralight")
  set(ENABLE_BROWSER OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(obs-studio VERSION ${OBS_VERSION_CANONICAL})

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/architecture.cmake")
  if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
    return()
  endif()
endif()

include(compilerconfig)
include(defaults)
include(helpers)

option(ENABLE_FRONTEND "Enable building with UI (requires Qt)" ON)
option(ENABLE_SCRIPTING "Enable scripting support" ON)
option(ENABLE_HEVC "Enable HEVC encoders" ON)

add_subdirectory(libobs)
if(OS_WINDOWS)
  add_subdirectory(libobs-d3d11)
  add_subdirectory(libobs-winrt)
endif()
add_subdirectory(libobs-opengl)
if(OS_MACOS)
  add_subdirectory(libobs-metal)
endif()
add_subdirectory(plugins)

add_subdirectory(test/test-input)

add_subdirectory(frontend)

message_configuration()
