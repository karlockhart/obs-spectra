#pragma once

#include "export.hpp"

#include <QByteArray>
#include <QString>

#include <functional>
#include <vector>

namespace spectra::speech {

/* A downloadable model file, pinned by revision and SHA-256 */
struct ModelInfo {
	QString id;   /* config value, e.g. "large-v3-turbo-q5_0" */
	QString file; /* e.g. "ggml-large-v3-turbo-q5_0.bin" */
	QString title;
	qint64 size = 0;
	QByteArray sha256; /* hex */
	QString url;
	bool englishOnly = false;
};

/* Whisper models, best trade-offs first */
SPECTRA_SPEECH_API const std::vector<ModelInfo> &WhisperModels();
SPECTRA_SPEECH_API const ModelInfo *FindWhisperModel(const QString &id);
SPECTRA_SPEECH_API QString DefaultWhisperModel();

/* The Silero voice activity model used to skip silence */
SPECTRA_SPEECH_API const ModelInfo &VadModel();

/* Where models are kept: <OBS config>/plugin_config/spectra-speech/models.
 * Set once at startup by whoever knows the config directory. */
SPECTRA_SPEECH_API void SetModelDirectory(const QString &dir);
SPECTRA_SPEECH_API QString ModelDirectory();
SPECTRA_SPEECH_API QString ModelPath(const ModelInfo &model);
SPECTRA_SPEECH_API bool ModelInstalled(const ModelInfo &model);

/* received and total bytes; return false to cancel */
using DownloadProgress = std::function<bool(qint64 received, qint64 total)>;

/* Downloads the model into ModelDirectory(), checking its size and SHA-256
 * before moving it into place. Blocks; call it off the UI thread. */
SPECTRA_SPEECH_API bool DownloadModel(const ModelInfo &model, const DownloadProgress &progress = {},
				      QString *error = nullptr);

} // namespace spectra::speech
