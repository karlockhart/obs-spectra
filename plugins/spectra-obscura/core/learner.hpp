#pragma once

#include "definitions.hpp"

#include <spectra-vision/chat.hpp>

#include <QString>
#include <QStringList>

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace obscura {

/* What the learner sees of a chat entry (the rows of training.jsonl) */
struct Features {
	QString channel;
	QStringList tags;
	QString body;
	std::optional<std::array<double, 13>> colour;
};
Features EntryFeatures(const spectra::ChatEntry &entry);

struct Prediction {
	double prob = 0.0; /* probability that the entry should be censored */
	QString source;    /* seed | channel | learned */

	bool Censor() const { return prob >= 0.5; }
	double Confidence() const { return std::abs(prob - 0.5) * 2.0; }
};

/* --- sklearn-compatible pieces ------------------------------------------- */

using SparseRow = std::vector<std::pair<int, double>>; /* sorted by index */

/* TfidfVectorizer(analyzer="char_wb", ngram_range=(2, 4), sublinear_tf=True):
 * lowercase, smooth idf, l2-normalised rows */
class TfidfCharWb {
public:
	void Fit(const std::vector<QString> &docs);
	SparseRow Transform(const QString &doc) const;
	int Size() const { return (int)idf.size(); }

	static std::vector<std::u32string> Ngrams(const QString &doc, int minN = 2, int maxN = 4);

private:
	std::unordered_map<std::u32string, int> vocabulary;
	std::vector<double> idf;
};

/* LogisticRegression(C, class_weight="balanced") for binary labels:
 * minimises 0.5 |w|^2 + C sum_i s_i log(1 + exp(-y_i (w.x_i + b))) with
 * L-BFGS, intercept unpenalised (same optimum as sklearn's lbfgs solver) */
class LogisticRegression {
public:
	explicit LogisticRegression(double C = 4.0) : C(C) {}
	void Fit(const std::vector<SparseRow> &x, const std::vector<int> &y, int dims);
	double PredictProba(const SparseRow &x) const;

	std::vector<double> weights;
	double intercept = 0.0;
	int iterations = 0;

private:
	double C;
};

/* --- the learner ---------------------------------------------------------- */

/* Learns which chat entries to censor from review decisions (port of
 * obscura/classifier.py): per-channel statistics smoothed toward a seed prior,
 * blended with a character n-gram text model. Reads and appends Obscura's
 * training.jsonl. Thread-safe. */
class Learner {
public:
	static constexpr int kMinModelSamples = 8;

	Learner(const QString &dataDir, const QStringList &seeds, const std::optional<Definitions> &installed = {},
		bool useInstalled = true);

	void SetInstalled(const std::optional<Definitions> &installed);
	void SetUseInstalled(bool useInstalled);
	void SetSeeds(const QStringList &seeds);
	void AddScreen(const std::vector<Features> &features, const std::vector<bool> &labels);
	void Reset();

	std::vector<Prediction> Predict(const std::vector<Features> &features) const;
	bool IsSeeded(const QString &channel) const;
	Definitions ExportDefinitions(int defsVersion) const;

	int Screens() const;
	int Samples() const;
	int InstalledVersion() const;
	QStringList ChannelsSeen() const;
	bool HasModel() const;

private:
	struct Sample {
		Features f;
		int label;
		QString screen;
	};
	struct Model {
		TfidfCharWb vec;
		LogisticRegression clf;
	};

	QString path;
	QStringList seeds;
	std::vector<Sample> samples;
	int screens = 0;
	bool useInstalled = true;
	std::map<QString, std::array<int, 2>> baseStats;
	QStringList baseSeeds;
	int installedVersion = 0;
	std::map<QString, std::array<int, 2>> stats;
	std::unique_ptr<Model> model;
	mutable std::recursive_mutex lock;

	void Load();
	void Fit();
	QString ChannelKey(const QString &channel) const;
	static SparseRow Row(const Features &f, const TfidfCharWb &vec);
};

/* One line of training.jsonl, in Python json.dumps' default layout */
QByteArray TrainingLine(const Features &f, int label, const QString &screen);

} // namespace obscura
