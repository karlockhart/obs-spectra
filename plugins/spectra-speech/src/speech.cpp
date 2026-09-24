#include <spectra-speech/speech.hpp>

#include <whisper.h>

#include <obs-module.h>

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace spectra::speech {

namespace {

/* whisper.cpp and ggml are chatty at info level; keep that out of the log */
void LogCallback(enum ggml_log_level level, const char *text, void *)
{
	if (!text || !*text) {
		return;
	}
	QString line = QString::fromUtf8(text).trimmed();
	if (line.isEmpty()) {
		return;
	}
	const int obsLevel = level == GGML_LOG_LEVEL_ERROR  ? LOG_WARNING
			     : level == GGML_LOG_LEVEL_WARN ? LOG_INFO
							    : LOG_DEBUG;
	blog(obsLevel, "[Speech] %s", line.toUtf8().constData());
}

void InstallLogger()
{
	static std::once_flag once;
	std::call_once(once, []() { whisper_log_set(LogCallback, nullptr); });
}

struct Job {
	const Progress *progress = nullptr;
	std::atomic<float> fraction{0.0f};
	std::atomic<bool> cancelled{false};

	bool Report(float value)
	{
		fraction = value;
		if (progress && *progress && !(*progress)(value)) {
			cancelled = true;
		}
		return !cancelled;
	}
};

void ProgressCallback(whisper_context *, whisper_state *, int percent, void *data)
{
	static_cast<Job *>(data)->Report(std::clamp(percent, 0, 100) / 100.0f);
}

bool AbortCallback(void *data)
{
	Job *job = static_cast<Job *>(data);
	/* Asks the caller again so a cancel lands between progress steps */
	return !job->Report(job->fraction);
}

} // namespace

Model::~Model()
{
	whisper_free(ctx);
}

std::unique_ptr<Model> Model::Load(const QString &path, bool useGpu, QString *error)
{
	InstallLogger();

	whisper_context_params params = whisper_context_default_params();
	params.use_gpu = useGpu;
	whisper_context *ctx = whisper_init_from_file_with_params(path.toUtf8().constData(), params);
	if (!ctx) {
		if (error) {
			*error = QStringLiteral("Could not load the speech model %1").arg(path);
		}
		return nullptr;
	}
	return std::unique_ptr<Model>(new Model(ctx));
}

bool Model::Transcribe(const std::vector<float> &samples, const Options &options, std::vector<Segment> &segments,
		       const Progress &progress, QString *error)
{
	segments.clear();
	if (samples.empty()) {
		return true;
	}

	Job job;
	job.progress = &progress;

	const QByteArray language = options.language.isEmpty() ? QByteArray("auto") : options.language.toUtf8();
	const QByteArray vadModel = options.vadModelPath.toUtf8();
	const QByteArray prompt = options.prompt.toUtf8();

	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = std::max(options.threads, 1);
	params.language = language.constData();
	params.translate = options.translate;
	params.print_progress = false;
	params.print_realtime = false;
	params.print_special = false;
	params.print_timestamps = false;
	/* Each window on its own: carrying text over lets one mistake repeat
	 * for minutes in long recordings */
	params.no_context = true;
	params.suppress_blank = true;
	params.suppress_nst = true;
	if (!prompt.isEmpty()) {
		params.initial_prompt = prompt.constData();
	}
	if (!vadModel.isEmpty()) {
		params.vad = true;
		params.vad_model_path = vadModel.constData();
		params.vad_params = whisper_vad_default_params();
	}
	params.progress_callback = ProgressCallback;
	params.progress_callback_user_data = &job;
	params.abort_callback = AbortCallback;
	params.abort_callback_user_data = &job;

	const int result = whisper_full(ctx, params, samples.data(), (int)samples.size());
	if (job.cancelled) {
		if (error) {
			*error = QStringLiteral("Cancelled");
		}
		return false;
	}
	if (result != 0) {
		if (error) {
			*error = QStringLiteral("Speech recognition failed (%1)").arg(result);
		}
		return false;
	}

	const int lang = whisper_full_lang_id(ctx);
	lastLanguage = lang >= 0 ? QString::fromUtf8(whisper_lang_str(lang)) : QString();

	const int count = whisper_full_n_segments(ctx);
	segments.reserve(count);
	for (int i = 0; i < count; i++) {
		Segment segment;
		/* whisper.cpp times are in 10 ms steps */
		segment.start = whisper_full_get_segment_t0(ctx, i) / 100.0;
		segment.end = whisper_full_get_segment_t1(ctx, i) / 100.0;
		segment.text = QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).trimmed();
		const int tokens = whisper_full_n_tokens(ctx, i);
		float sum = 0.0f;
		for (int t = 0; t < tokens; t++) {
			sum += whisper_full_get_token_p(ctx, i, t);
		}
		segment.confidence = tokens > 0 ? sum / tokens : 0.0f;
		segment.noSpeech = whisper_full_get_segment_no_speech_prob(ctx, i);
		if (!segment.text.isEmpty()) {
			segments.push_back(std::move(segment));
		}
	}
	job.Report(1.0f);
	return true;
}

QString SystemInfo()
{
	InstallLogger();
	return QString::fromUtf8(whisper_print_system_info()).trimmed();
}

bool IsLikelyHallucination(const Segment &segment)
{
	const QString &text = segment.text;
	static const QRegularExpression nonSpeech(QStringLiteral(R"(^\s*[\[\(\*♪].*[\]\)\*♪]\s*$)"));
	static const QRegularExpression strip(QStringLiteral(R"([^\p{L}\p{N} ]+)"));
	static const QSet<QString> phrases = {
		QStringLiteral("you"),
		QStringLiteral("thank you"),
		QStringLiteral("thanks for watching"),
		QStringLiteral("thank you for watching"),
		QStringLiteral("thank you so much for watching"),
		QStringLiteral("please subscribe"),
		QStringLiteral("subtitles by the amaraorg community"),
		QStringLiteral("bye"),
	};

	if (text.trimmed().isEmpty() || nonSpeech.match(text).hasMatch()) {
		return true;
	}
	QString plain = text.toLower();
	plain.remove(strip);
	plain = plain.simplified();
	if (plain.isEmpty()) {
		return true;
	}
	const bool unsure = segment.noSpeech > 0.3f || segment.confidence < 0.6f;
	return unsure && phrases.contains(plain);
}

} // namespace spectra::speech
