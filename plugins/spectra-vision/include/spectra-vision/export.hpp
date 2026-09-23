#pragma once

#if defined(_WIN32)
#if defined(SPECTRA_VISION_BUILD)
#define SPECTRA_VISION_API __declspec(dllexport)
#else
#define SPECTRA_VISION_API __declspec(dllimport)
#endif
#else
#define SPECTRA_VISION_API __attribute__((visibility("default")))
#endif
