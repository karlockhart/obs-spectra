#include "learner.hpp"

#include <spectra-vision/text.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <set>

namespace obscura {

Features EntryFeatures(const spectra::ChatEntry &entry)
{
	Features f{entry.channel, entry.tags, entry.body, std::array<double, 13>{}};
	for (size_t i = 0; i < 13; i++) {
		(*f.colour)[i] = entry.colour[i];
	}
	return f;
}

/* --- TF-IDF ---------------------------------------------------------------- */

std::vector<std::u32string> TfidfCharWb::Ngrams(const QString &doc, int minN, int maxN)
{
	/* sklearn's _char_wb_ngrams on the lowercased document */
	std::vector<std::u32string> out;
	const QList<uint> text = doc.toLower().toUcs4();
	std::u32string word;
	auto flush = [&] {
		if (word.empty()) {
			return;
		}
		std::u32string w = U" " + word + U" ";
		const int len = (int)w.size();
		for (int n = minN; n <= maxN; n++) {
			int offset = 0;
			out.push_back(w.substr(offset, n));
			while (offset + n < len) {
				offset++;
				out.push_back(w.substr(offset, n));
			}
			if (offset == 0) { /* a short word (len < n) counts once */
				break;
			}
		}
		word.clear();
	};
	for (uint c : text) {
		if (QChar::isSpace(c)) {
			flush();
		} else {
			word.push_back((char32_t)c);
		}
	}
	flush();
	return out;
}

void TfidfCharWb::Fit(const std::vector<QString> &docs)
{
	vocabulary.clear();
	std::vector<int> df;
	for (const QString &doc : docs) {
		std::set<int> seen;
		for (const std::u32string &g : Ngrams(doc)) {
			auto [it, added] = vocabulary.emplace(g, (int)df.size());
			if (added) {
				df.push_back(0);
			}
			seen.insert(it->second);
		}
		for (int i : seen) {
			df[i]++;
		}
	}
	const double n = (double)docs.size();
	idf.resize(df.size());
	for (size_t i = 0; i < df.size(); i++) {
		idf[i] = std::log((1.0 + n) / (1.0 + df[i])) + 1.0;
	}
}

SparseRow TfidfCharWb::Transform(const QString &doc) const
{
	std::map<int, int> counts;
	for (const std::u32string &g : Ngrams(doc)) {
		auto it = vocabulary.find(g);
		if (it != vocabulary.end()) {
			counts[it->second]++;
		}
	}
	SparseRow row;
	double norm = 0.0;
	for (auto [i, tf] : counts) {
		double v = (1.0 + std::log((double)tf)) * idf[i];
		row.emplace_back(i, v);
		norm += v * v;
	}
	norm = std::sqrt(norm);
	if (norm > 0) {
		for (auto &p : row) {
			p.second /= norm;
		}
	}
	return row;
}

/* --- logistic regression --------------------------------------------------- */

namespace {

double Dot(const SparseRow &x, const std::vector<double> &w)
{
	double s = 0.0;
	for (auto [i, v] : x) {
		s += w[i] * v;
	}
	return s;
}

/* log(1 + exp(t)) without overflow */
double Log1pExp(double t)
{
	return t > 0 ? t + std::log1p(std::exp(-t)) : std::log1p(std::exp(t));
}

double Sigmoid(double t)
{
	if (t >= 0) {
		return 1.0 / (1.0 + std::exp(-t));
	}
	double e = std::exp(t);
	return e / (1.0 + e);
}

} // namespace

void LogisticRegression::Fit(const std::vector<SparseRow> &x, const std::vector<int> &y, int dims)
{
	const size_t n = x.size();
	int positives = 0;
	for (int label : y) {
		positives += label;
	}
	/* class_weight="balanced": n / (2 * count(class)) */
	const double wPos = positives ? (double)n / (2.0 * positives) : 0.0;
	const double wNeg = n - positives ? (double)n / (2.0 * (n - positives)) : 0.0;

	const int d = dims + 1; /* last variable is the intercept */
	auto evaluate = [&](const std::vector<double> &p, std::vector<double> &grad) {
		grad.assign(d, 0.0);
		double f = 0.0;
		for (int j = 0; j < dims; j++) {
			f += 0.5 * p[j] * p[j];
			grad[j] = p[j];
		}
		for (size_t i = 0; i < n; i++) {
			const double s = C * (y[i] ? wPos : wNeg);
			const double yi = y[i] ? 1.0 : -1.0;
			const double z = Dot(x[i], p) + p[dims];
			f += s * Log1pExp(-yi * z);
			const double g = -s * yi * Sigmoid(-yi * z);
			for (auto [j, v] : x[i]) {
				grad[j] += g * v;
			}
			grad[dims] += g;
		}
		return f;
	};

	/* L-BFGS with a backtracking Armijo line search */
	const int m = 10;
	std::vector<double> p(d, 0.0), g, pNew(d), gNew;
	double f = evaluate(p, g);
	std::deque<std::vector<double>> S, Y;
	std::deque<double> rho;
	iterations = 0;
	for (; iterations < 2000; iterations++) {
		double gmax = 0.0;
		for (double v : g) {
			gmax = std::max(gmax, std::abs(v));
		}
		if (gmax < 1e-9 * std::max(1.0, std::abs(f))) {
			break;
		}
		/* two-loop recursion */
		std::vector<double> q = g;
		std::vector<double> alpha(S.size());
		for (int k = (int)S.size() - 1; k >= 0; k--) {
			double a = 0.0;
			for (int j = 0; j < d; j++) {
				a += S[k][j] * q[j];
			}
			a *= rho[k];
			alpha[k] = a;
			for (int j = 0; j < d; j++) {
				q[j] -= a * Y[k][j];
			}
		}
		if (!S.empty()) {
			double sy = 0.0, yy = 0.0;
			for (int j = 0; j < d; j++) {
				sy += S.back()[j] * Y.back()[j];
				yy += Y.back()[j] * Y.back()[j];
			}
			const double gamma = sy / yy;
			for (double &v : q) {
				v *= gamma;
			}
		}
		for (size_t k = 0; k < S.size(); k++) {
			double b = 0.0;
			for (int j = 0; j < d; j++) {
				b += Y[k][j] * q[j];
			}
			b *= rho[k];
			for (int j = 0; j < d; j++) {
				q[j] += (alpha[k] - b) * S[k][j];
			}
		}
		/* direction = -q */
		double slope = 0.0;
		for (int j = 0; j < d; j++) {
			slope -= g[j] * q[j];
		}
		if (slope >= 0) { /* not a descent direction: restart from steepest descent */
			q = g;
			slope = 0.0;
			for (double v : g) {
				slope -= v * v;
			}
			S.clear();
			Y.clear();
			rho.clear();
		}
		double step = 1.0, fNew = 0.0;
		for (int tries = 0; tries < 60; tries++) {
			for (int j = 0; j < d; j++) {
				pNew[j] = p[j] - step * q[j];
			}
			fNew = evaluate(pNew, gNew);
			if (fNew <= f + 1e-4 * step * slope) {
				break;
			}
			step *= 0.5;
		}
		std::vector<double> s(d), yv(d);
		double sy = 0.0;
		for (int j = 0; j < d; j++) {
			s[j] = pNew[j] - p[j];
			yv[j] = gNew[j] - g[j];
			sy += s[j] * yv[j];
		}
		const bool converged = std::abs(f - fNew) <= 1e-14 * std::max({1.0, std::abs(f), std::abs(fNew)});
		p = pNew;
		g = gNew;
		f = fNew;
		if (sy > 1e-12) {
			S.push_back(std::move(s));
			Y.push_back(std::move(yv));
			rho.push_back(1.0 / sy);
			if ((int)S.size() > m) {
				S.pop_front();
				Y.pop_front();
				rho.pop_front();
			}
		}
		if (converged) {
			break;
		}
	}
	weights.assign(p.begin(), p.begin() + dims);
	intercept = p[dims];
}

double LogisticRegression::PredictProba(const SparseRow &x) const
{
	return Sigmoid(Dot(x, weights) + intercept);
}

/* --- training.jsonl -------------------------------------------------------- */

namespace {

/* json.dumps string escaping with ensure_ascii=True */
void AppendJsonString(QByteArray &out, const QString &s)
{
	out += '"';
	for (QChar c : s) {
		const ushort u = c.unicode();
		switch (u) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			if (u < 0x20 || u > 0x7e) {
				out += QStringLiteral("\\u%1").arg(u, 4, 16, QChar('0')).toLatin1();
			} else {
				out += (char)u;
			}
		}
	}
	out += '"';
}

/* Python float repr: the shortest round-trip digits, positional when the
 * decimal exponent is in [-4, 16), otherwise d.ddde-XX */
QByteArray PyFloat(double v)
{
	if (std::isnan(v)) {
		return "NaN";
	}
	if (std::isinf(v)) {
		return v > 0 ? "Infinity" : "-Infinity";
	}
	if (v == 0.0) {
		return std::signbit(v) ? "-0.0" : "0.0";
	}
	char buf[64];
	auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
	QByteArray sci(buf, end - buf); /* e.g. -2.5e-04 */
	const bool negative = sci.startsWith('-');
	if (negative) {
		sci.remove(0, 1);
	}
	const qsizetype e = sci.indexOf('e');
	QByteArray digits = sci.left(e);
	digits.replace(".", "");
	const int exponent = sci.mid(e + 1).toInt();
	QByteArray out;
	if (exponent >= -4 && exponent < 16) {
		if (exponent < 0) {
			out = "0." + QByteArray(-exponent - 1, '0') + digits;
		} else if (digits.size() <= exponent + 1) {
			out = digits + QByteArray(exponent + 1 - digits.size(), '0') + ".0";
		} else {
			out = digits.left(exponent + 1) + "." + digits.mid(exponent + 1);
		}
	} else {
		out = digits.left(1);
		if (digits.size() > 1) {
			out += "." + digits.mid(1);
		}
		out += exponent < 0 ? "e-" : "e+";
		out += QByteArray::number(std::abs(exponent)).rightJustified(2, '0');
	}
	return negative ? "-" + out : out;
}

} // namespace

QByteArray TrainingLine(const Features &f, int label, const QString &screen)
{
	QByteArray out = "{\"channel\": ";
	AppendJsonString(out, f.channel);
	out += ", \"tags\": [";
	for (int i = 0; i < f.tags.size(); i++) {
		if (i) {
			out += ", ";
		}
		AppendJsonString(out, f.tags[i]);
	}
	out += "], \"body\": ";
	AppendJsonString(out, f.body);
	out += ", \"colour\": ";
	if (f.colour) {
		out += '[';
		for (size_t i = 0; i < 13; i++) {
			if (i) {
				out += ", ";
			}
			out += PyFloat((*f.colour)[i]);
		}
		out += ']';
	} else {
		out += "null";
	}
	out += ", \"label\": " + QByteArray::number(label) + ", \"screen\": ";
	AppendJsonString(out, screen);
	out += "}\n";
	return out;
}

/* --- Learner --------------------------------------------------------------- */

namespace {
QStringList CleanSeeds(const QStringList &seeds)
{
	QStringList out;
	for (const QString &s : seeds) {
		if (!s.trimmed().isEmpty()) {
			out << s.trimmed().toLower();
		}
	}
	return out;
}
} // namespace

Learner::Learner(const QString &dataDir, const QStringList &seeds_, const std::optional<Definitions> &installed,
		 bool useInstalled_)
	: path(QDir(dataDir).filePath(QStringLiteral("training.jsonl"))),
	  seeds(CleanSeeds(seeds_)),
	  useInstalled(useInstalled_)
{
	std::lock_guard l(lock);
	Load();
	SetInstalled(installed);
}

void Learner::SetInstalled(const std::optional<Definitions> &installed)
{
	std::lock_guard l(lock);
	baseStats.clear();
	baseSeeds.clear();
	installedVersion = 0;
	if (installed) {
		for (const auto &[k, v] : installed->channelStats) {
			baseStats[k] = v;
		}
		baseSeeds = CleanSeeds(installed->seeds);
		installedVersion = installed->defsVersion;
	}
	Fit();
}

void Learner::SetUseInstalled(bool value)
{
	std::lock_guard l(lock);
	if (useInstalled != value) {
		useInstalled = value;
		Fit();
	}
}

void Learner::SetSeeds(const QStringList &value)
{
	std::lock_guard l(lock);
	seeds = CleanSeeds(value);
}

void Learner::Load()
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	QSet<QString> screenIds;
	while (!file.atEnd()) {
		QByteArray line = file.readLine().trimmed();
		if (line.isEmpty()) {
			continue;
		}
		QJsonParseError err;
		QJsonObject o = QJsonDocument::fromJson(line, &err).object();
		if (err.error != QJsonParseError::NoError) {
			continue;
		}
		Sample s;
		s.f.channel = o["channel"].toString();
		for (const QJsonValue &t : o["tags"].toArray()) {
			s.f.tags << t.toString();
		}
		s.f.body = o["body"].toString();
		QJsonArray colour = o["colour"].toArray();
		if (colour.size() == 13) {
			std::array<double, 13> c{};
			for (int i = 0; i < 13; i++) {
				c[i] = colour[i].toDouble();
			}
			s.f.colour = c;
		}
		s.label = o["label"].toInt() ? 1 : 0;
		s.screen = o["screen"].isString() ? o["screen"].toString()
						  : QString::fromUtf8(QJsonDocument(QJsonArray{o["screen"]}).toJson());
		screenIds.insert(s.screen);
		samples.push_back(std::move(s));
	}
	screens = (int)screenIds.size();
}

void Learner::AddScreen(const std::vector<Features> &features, const std::vector<bool> &labels)
{
	if (features.empty()) {
		return;
	}
	std::lock_guard l(lock);
	/* f"{time.time():.6f}", kept unique when screens arrive within a millisecond */
	static long long lastMicros = 0;
	long long micros = QDateTime::currentMSecsSinceEpoch() * 1000;
	micros = std::max(micros, lastMicros + 1);
	lastMicros = micros;
	const QString screen = QStringLiteral("%1.%2").arg(micros / 1000000).arg(micros % 1000000, 6, 10, QChar('0'));
	QFile file(path);
	const bool ok = file.open(QIODevice::Append);
	for (size_t i = 0; i < features.size() && i < labels.size(); i++) {
		const int label = labels[i] ? 1 : 0;
		if (ok) {
			file.write(TrainingLine(features[i], label, screen));
		}
		samples.push_back({features[i], label, screen});
	}
	screens++;
	Fit();
}

void Learner::Reset()
{
	std::lock_guard l(lock);
	QFile::remove(path);
	samples.clear();
	screens = 0;
	Fit();
}

SparseRow Learner::Row(const Features &f, const TfidfCharWb &vec)
{
	SparseRow row = vec.Transform(QStringLiteral("%1 | %1 | %2").arg(f.channel, f.body));
	const int base = vec.Size();
	for (int i = 0; i < 13; i++) {
		const double v = f.colour ? (*f.colour)[i] * 1.5 : 0.0;
		if (v != 0.0) {
			row.emplace_back(base + i, v);
		}
	}
	return row;
}

void Learner::Fit()
{
	stats.clear();
	if (useInstalled) {
		stats = baseStats;
	}
	for (const Sample &s : samples) {
		stats[s.f.channel][s.label]++;
	}
	model.reset();
	bool hasBoth[2] = {false, false};
	for (const Sample &s : samples) {
		hasBoth[s.label] = true;
	}
	if ((int)samples.size() < kMinModelSamples || !hasBoth[0] || !hasBoth[1]) {
		return;
	}
	auto m = std::make_unique<Model>();
	std::vector<QString> docs;
	docs.reserve(samples.size());
	for (const Sample &s : samples) {
		docs.push_back(QStringLiteral("%1 | %1 | %2").arg(s.f.channel, s.f.body));
	}
	m->vec.Fit(docs);
	std::vector<SparseRow> x;
	std::vector<int> y;
	for (const Sample &s : samples) {
		x.push_back(Row(s.f, m->vec));
		y.push_back(s.label);
	}
	m->clf.Fit(x, y, m->vec.Size() + 13);
	model = std::move(m);
}

QString Learner::ChannelKey(const QString &channel) const
{
	if (stats.count(channel)) {
		return channel;
	}
	QStringList keys;
	for (const auto &kv : stats) {
		keys << kv.first;
	}
	QStringList close = spectra::GetCloseMatches(channel, keys, 1, 0.85);
	return close.isEmpty() ? channel : close.first();
}

bool Learner::IsSeeded(const QString &channel) const
{
	std::lock_guard l(lock);
	for (const QString &s : seeds) {
		if (channel.contains(s)) {
			return true;
		}
	}
	if (useInstalled) {
		for (const QString &s : baseSeeds) {
			if (channel.contains(s)) {
				return true;
			}
		}
	}
	return false;
}

std::vector<Prediction> Learner::Predict(const std::vector<Features> &features) const
{
	std::lock_guard l(lock);
	std::vector<Prediction> out;
	for (const Features &f : features) {
		const QString key = ChannelKey(f.channel);
		auto it = stats.find(key);
		const int keepN = it != stats.end() ? it->second[0] : 0;
		const int censorN = it != stats.end() ? it->second[1] : 0;
		const int n = keepN + censorN;
		const bool seeded = IsSeeded(key);
		const double prior = seeded ? 0.95 : 0.15;
		const double pChannel = (censorN + 2.0 * prior) / (n + 2.0);
		if (!model) {
			out.push_back({pChannel, n ? QStringLiteral("channel") : QStringLiteral("seed")});
			continue;
		}
		const double text = model->clf.PredictProba(Row(f, model->vec));
		const double evidence = n + (seeded ? 3.0 : 0.0);
		const double weight = evidence / (evidence + 3.0) * std::abs(pChannel - 0.5) * 2.0;
		out.push_back({weight * pChannel + (1.0 - weight) * text, QStringLiteral("learned")});
	}
	return out;
}

Definitions Learner::ExportDefinitions(int defsVersion) const
{
	std::lock_guard l(lock);
	Definitions d;
	d.defsVersion = defsVersion;
	for (const auto &[k, v] : stats) {
		d.channelStats[k] = v;
	}
	QStringList all = seeds + baseSeeds;
	all.removeDuplicates();
	all.sort();
	d.seeds = all;
	return d;
}

int Learner::Screens() const
{
	std::lock_guard l(lock);
	return screens;
}

int Learner::Samples() const
{
	std::lock_guard l(lock);
	return (int)samples.size();
}

int Learner::InstalledVersion() const
{
	std::lock_guard l(lock);
	return installedVersion;
}

bool Learner::HasModel() const
{
	std::lock_guard l(lock);
	return model != nullptr;
}

QStringList Learner::ChannelsSeen() const
{
	std::lock_guard l(lock);
	QStringList out;
	for (const Sample &s : samples) {
		if (!out.contains(s.f.channel)) {
			out << s.f.channel;
		}
	}
	out.sort();
	return out;
}

} // namespace obscura
