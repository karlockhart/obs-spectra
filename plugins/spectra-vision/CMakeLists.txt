cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_SOURCE_DIR}/cmake/spectra/SpectraDeps.cmake")

if(NOT OS_WINDOWS OR NOT CMAKE_VS_PLATFORM_NAME STREQUAL x64)
  message(STATUS "spectra-vision: only Windows x64 is supported for now")
  target_disable(spectra-vision)
  return()
endif()

# onnxruntime (MIT) - prebuilt release matching RapidOCR's tested version
spectra_fetch(
  onnxruntime
  URL https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-win-x64-1.30.0.zip
  SHA256 c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949
  EXTRACT
)
set(onnxruntime_root "${SPECTRA_ONNXRUNTIME_PATH}/onnxruntime-win-x64-1.30.0")

add_library(Spectra::onnxruntime SHARED IMPORTED)
set_target_properties(
  Spectra::onnxruntime
  PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_root}/lib/onnxruntime.dll"
    IMPORTED_IMPLIB "${onnxruntime_root}/lib/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_root}/include"
)

# RapidOCR 3.9.2 default models (Apache-2.0): PP-OCRv6 small det/rec
spectra_fetch(
  ocr-det-model
  URL https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx
  SHA256 090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f
)
spectra_fetch(
  ocr-rec-model
  URL https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx
  SHA256 6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884
)

add_library(spectra-vision SHARED)
add_library(OBS::spectra-vision ALIAS spectra-vision)

target_sources(
  spectra-vision
  PRIVATE
    include/spectra-vision/export.hpp
    include/spectra-vision/image.hpp
    include/spectra-vision/ocr.hpp
    src/geometry.cpp
    src/geometry.hpp
    src/image.cpp
    src/ocr.cpp
)

target_include_directories(spectra-vision PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(spectra-vision PRIVATE SPECTRA_VISION_BUILD)
target_compile_features(spectra-vision PUBLIC cxx_std_20)
target_link_libraries(spectra-vision PRIVATE Spectra::onnxruntime)

set_target_properties_obs(spectra-vision PROPERTIES FOLDER spectra)

# onnxruntime.dll next to the executable: the application directory is
# searched before System32, which has an older Windows-provided copy.
add_custom_command(
  TARGET spectra-vision
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different "${onnxruntime_root}/lib/onnxruntime.dll"
    "${onnxruntime_root}/lib/onnxruntime_providers_shared.dll" "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}/"
  COMMENT "Copy onnxruntime to the executable directory"
  VERBATIM
)
install(
  FILES "${onnxruntime_root}/lib/onnxruntime.dll" "${onnxruntime_root}/lib/onnxruntime_providers_shared.dll"
  DESTINATION "${OBS_EXECUTABLE_DESTINATION}"
  COMPONENT Runtime
)

target_add_resource(spectra-vision "${SPECTRA_OCR_DET_MODEL_PATH}" "${OBS_DATA_DESTINATION}/spectra-vision/models")
target_add_resource(spectra-vision "${SPECTRA_OCR_REC_MODEL_PATH}" "${OBS_DATA_DESTINATION}/spectra-vision/models")
target_add_resource(spectra-vision "${onnxruntime_root}/LICENSE" "${OBS_DATA_DESTINATION}/spectra-vision/licenses/onnxruntime")

# Command-line OCR tool, used to compare results with RapidOCR
find_package(Qt6 REQUIRED Gui)
add_executable(spectra-ocr tools/spectra-ocr.cpp)
target_link_libraries(spectra-ocr PRIVATE spectra-vision Qt::Gui)
set_target_properties(spectra-ocr PROPERTIES FOLDER spectra)
set_target_properties_obs(spectra-ocr)
