cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_SOURCE_DIR}/cmake/spectra/SpectraDeps.cmake")

if(NOT OS_WINDOWS OR NOT CMAKE_VS_PLATFORM_NAME STREQUAL x64)
  message(STATUS "spectra-speech: only Windows x64 is supported for now")
  target_disable(spectra-speech)
  return()
endif()

# whisper.cpp (MIT): speech recognition, with ggml built in
spectra_fetch(
  whisper-cpp
  URL https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.4.tar.gz
  SHA256 57e280cee375ab02425b806ad5146b99f6eb9357e3c2b31357c8a6af2e2e44ae
  EXTRACT
)
set(whisper_root "${SPECTRA_WHISPER_CPP_PATH}/whisper.cpp-1.9.4")

# Vulkan runs Whisper on the GPU (any vendor); without the Vulkan SDK at build
# time, only the CPU backend is built. The SDK's headers are used rather than
# obs-deps' older ones, which ggml's Vulkan backend doesn't build with.
set(GGML_VULKAN OFF)
if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/glslc.exe")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" vulkan_sdk)
  set(Vulkan_INCLUDE_DIR "${vulkan_sdk}/Include")
  set(Vulkan_LIBRARY "${vulkan_sdk}/Lib/vulkan-1.lib")
  set(Vulkan_GLSLC_EXECUTABLE "${vulkan_sdk}/Bin/glslc.exe")
  set(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE "${vulkan_sdk}/Bin/glslangValidator.exe")
  set(GGML_VULKAN ON)
  message(STATUS "spectra-speech: building the Vulkan (GPU) backend with ${vulkan_sdk}")
else()
  message(WARNING "spectra-speech: Vulkan SDK not found, speech recognition will only run on the CPU")
endif()

# OBS's preset sets the platform to "x64,version=<SDK>", which ggml doesn't
# recognise as x86 and then builds only its slow generic CPU code
set(CMAKE_GENERATOR_PLATFORM x64)

# Plain variables, scoped to this directory: whisper.cpp's option() calls
# keep them with CMP0077, which its old cmake_minimum_required would turn off.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(WHISPER_BUILD_TESTS OFF)
set(WHISPER_BUILD_EXAMPLES OFF)
set(WHISPER_BUILD_SERVER OFF)
set(WHISPER_ALL_WARNINGS OFF)
set(WHISPER_CURL OFF)
set(WHISPER_SDL2 OFF)
set(GGML_BUILD_TESTS OFF)
set(GGML_BUILD_EXAMPLES OFF)
# A fixed x64 baseline (AVX2/FMA/F16C) rather than this machine's CPU, so
# builds run on other computers too
set(GGML_NATIVE OFF)
set(GGML_AVX ON)
set(GGML_AVX2 ON)
set(GGML_FMA ON)
set(GGML_F16C ON)
# ggml's own thread pool instead of OpenMP, which would need vcomp140.dll
set(GGML_OPENMP OFF)
add_subdirectory("${whisper_root}" whisper EXCLUDE_FROM_ALL)

foreach(target IN ITEMS whisper ggml ggml-base ggml-cpu ggml-vulkan)
  if(TARGET ${target})
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF FOLDER spectra/whisper)
    target_compile_options(${target} PRIVATE /W0)
  endif()
endforeach()

find_package(Qt6 REQUIRED Core)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swresample)

add_library(spectra-speech SHARED)
add_library(OBS::spectra-speech ALIAS spectra-speech)

target_sources(
  spectra-speech
  PRIVATE
    include/spectra-speech/audio.hpp
    include/spectra-speech/export.hpp
    include/spectra-speech/models.hpp
    include/spectra-speech/speech.hpp
    src/audio.cpp
    src/models.cpp
    src/speech.cpp
)

target_include_directories(spectra-speech PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(spectra-speech PRIVATE SPECTRA_SPEECH_BUILD)
# PRIVATE: the frontend, which links this, is C++17 (its u8string use breaks in C++20)
target_compile_features(spectra-speech PRIVATE cxx_std_20)
target_link_libraries(
  spectra-speech
  PUBLIC Qt::Core
  PRIVATE
    whisper
    OBS::libobs
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swresample
    winhttp
)

set_target_properties_obs(spectra-speech PROPERTIES FOLDER spectra)

# Command-line transcription, to try models and settings on recordings:
# spectra-transcribe <model.bin> <media> [--track N] [--start S] [--duration S] [--language xx] [--vad vad.bin] [--cpu]
add_executable(spectra-transcribe tools/spectra-transcribe.cpp)
target_link_libraries(spectra-transcribe PRIVATE spectra-speech)
set_target_properties(spectra-transcribe PROPERTIES FOLDER spectra)
set_target_properties_obs(spectra-transcribe)
