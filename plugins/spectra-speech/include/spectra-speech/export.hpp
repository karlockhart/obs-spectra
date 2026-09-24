#pragma once

#if defined(_WIN32)
#if defined(SPECTRA_SPEECH_BUILD)
#define SPECTRA_SPEECH_API __declspec(dllexport)
#else
#define SPECTRA_SPEECH_API __declspec(dllimport)
#endif
#else
#define SPECTRA_SPEECH_API __attribute__((visibility("default")))
#endif
