#pragma once

#include "export.hpp"

#include <QString>
#include <QStringList>

namespace spectra {

/* difflib.SequenceMatcher(None, a, b).ratio(), including autojunk (elements
 * of b that are "popular" when len(b) >= 200). Compares Unicode code points
 * like Python str. */
SPECTRA_VISION_API double SequenceRatio(const QString &a, const QString &b);

/* difflib.get_close_matches(word, possibilities, n, cutoff) */
SPECTRA_VISION_API QStringList GetCloseMatches(const QString &word, const QStringList &possibilities, int n = 3,
					       double cutoff = 0.6);

} // namespace spectra
