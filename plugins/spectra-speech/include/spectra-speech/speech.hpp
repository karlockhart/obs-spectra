#pragma once

#include "export.hpp"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

struct whisper_context;

namespace spectra::speech {

/* Whisper works on 16 kHz mono float samples */
constexpr int kSampleRate = 16000;

struct Options {
	/* "auto" detects the language, otherwise an ISO 639-1 code ("en") */
	QString language = QStringLiteral("auto");
	bool translate = false;
	int threads = 4;
	/* Silero voice activity model (ggml-silero-*.bin): only speech is
	 * transcribed, which avoids Whisper inventing text in silence. Empty to
	 * transcribe everything. */
	QString vadModelPath;
	/* Names, places or jargon to prime the vocabulary with */
	QString prompt;
};

/* One stretch of speech, in seconds from the start of the audio */
struct Segment {
	double start = 0.0;
	double end = 0.0;
	QString text;
	/* Mean token probability, 0..1 */
	float confidence = 0.0f;
	/* Whisper's estimate that there was no speech at all, 0..1 */
	float noSpeech = 0.0f;
};

/* progress 0..1; return false to cancel */
using Progress = std::function<bool(float progress)>;

/* A loaded Whisper model. Transcribe runs one job at a time; create one
 * Model per thread that transcribes. */
class SPECTRA_SPEECH_API Model {
public:
	~Model();
	Model(const Model &) = delete;
	Model &operator=(const Model &) = delete;

	/* useGpu uses Vulkan when a GPU is available, else the CPU */
	static std::unique_ptr<Model> Load(const QString &path, bool useGpu, QString *error = nullptr);

	/* samples: kSampleRate mono float. Returns false on error or cancel. */
	bool Transcribe(const std::vector<float> &samples, const Options &options, std::vector<Segment> &segments,
			const Progress &progress = {}, QString *error = nullptr);

	/* The language the last Transcribe detected or used, e.g. "en" */
	QString LastLanguage() const { return lastLanguage; }

private:
	explicit Model(whisper_context *ctx) : ctx(ctx) {}
	whisper_context *ctx;
	QString lastLanguage;
};

/* ggml backends and CPU features, for logs */
SPECTRA_SPEECH_API QString SystemInfo();

/* Output Whisper invents for noise the voice activity model let through:
 * sound descriptions ("[Music]") always, and stock phrases ("Thank you.",
 * "Thanks for watching!") only when Whisper wasn't sure there was speech,
 * since people do say them. */
SPECTRA_SPEECH_API bool IsLikelyHallucination(const Segment &segment);

} // namespace spectra::speech
