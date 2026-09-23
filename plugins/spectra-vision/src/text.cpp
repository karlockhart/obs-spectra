#include <spectra-vision/text.hpp>

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* Port of CPython's difflib.SequenceMatcher (isjunk=None, autojunk=True) */

namespace spectra {

namespace {

struct Matcher {
	std::vector<char32_t> a, b;
	std::unordered_map<char32_t, std::vector<int>> b2j;

	Matcher(const QString &sa, const QString &sb)
	{
		for (char32_t c : sa.toUcs4()) {
			a.push_back(c);
		}
		for (char32_t c : sb.toUcs4()) {
			b.push_back(c);
		}
		for (int i = 0; i < (int)b.size(); i++) {
			b2j[b[i]].push_back(i);
		}
		const int n = (int)b.size();
		if (n >= 200) {
			const size_t ntest = (size_t)(n / 100 + 1);
			std::vector<char32_t> popular;
			for (auto &[elt, idxs] : b2j) {
				if (idxs.size() > ntest) {
					popular.push_back(elt);
				}
			}
			for (char32_t elt : popular) {
				b2j.erase(elt);
			}
		}
	}

	std::tuple<int, int, int> FindLongestMatch(int alo, int ahi, int blo, int bhi) const
	{
		int besti = alo, bestj = blo, bestsize = 0;
		std::unordered_map<int, int> j2len;
		for (int i = alo; i < ahi; i++) {
			std::unordered_map<int, int> newj2len;
			auto it = b2j.find(a[i]);
			if (it != b2j.end()) {
				for (int j : it->second) {
					if (j < blo) {
						continue;
					}
					if (j >= bhi) {
						break;
					}
					auto prev = j2len.find(j - 1);
					int k = (prev != j2len.end() ? prev->second : 0) + 1;
					newj2len[j] = k;
					if (k > bestsize) {
						besti = i - k + 1;
						bestj = j - k + 1;
						bestsize = k;
					}
				}
			}
			j2len.swap(newj2len);
		}

		/* No junk is defined, so only the "extend with equal elements"
		 * passes apply (popular elements are not junk). */
		while (besti > alo && bestj > blo && a[besti - 1] == b[bestj - 1]) {
			besti--;
			bestj--;
			bestsize++;
		}
		while (besti + bestsize < ahi && bestj + bestsize < bhi && a[besti + bestsize] == b[bestj + bestsize]) {
			bestsize++;
		}
		return {besti, bestj, bestsize};
	}

	int MatchingCharacters() const
	{
		std::vector<std::tuple<int, int, int, int>> queue{{0, (int)a.size(), 0, (int)b.size()}};
		int matches = 0;
		while (!queue.empty()) {
			auto [alo, ahi, blo, bhi] = queue.back();
			queue.pop_back();
			auto [i, j, k] = FindLongestMatch(alo, ahi, blo, bhi);
			if (k) {
				matches += k;
				if (alo < i && blo < j) {
					queue.emplace_back(alo, i, blo, j);
				}
				if (i + k < ahi && j + k < bhi) {
					queue.emplace_back(i + k, ahi, j + k, bhi);
				}
			}
		}
		return matches;
	}
};

} // namespace

double SequenceRatio(const QString &a, const QString &b)
{
	Matcher m(a, b);
	const size_t total = m.a.size() + m.b.size();
	if (!total) {
		return 1.0;
	}
	return 2.0 * m.MatchingCharacters() / total;
}

QStringList GetCloseMatches(const QString &word, const QStringList &possibilities, int n, double cutoff)
{
	std::vector<std::pair<double, QString>> scored;
	for (const QString &x : possibilities) {
		double r = SequenceRatio(x, word);
		if (r >= cutoff) {
			scored.emplace_back(r, x);
		}
	}
	/* heapq.nlargest compares (score, string) tuples */
	std::stable_sort(scored.begin(), scored.end(), [](const auto &p, const auto &q) {
		if (p.first != q.first) {
			return p.first > q.first;
		}
		return p.second > q.second;
	});
	QStringList out;
	for (int i = 0; i < (int)scored.size() && i < n; i++) {
		out << scored[i].second;
	}
	return out;
}

} // namespace spectra
