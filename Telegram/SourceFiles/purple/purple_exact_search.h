#pragma once

#include <QtCore/QStringList>
#include <algorithm>

namespace Purple {

inline QStringList ExactSearchTerms(const QString &query) {
	return query.toCaseFolded().simplified().split(u' ', Qt::SkipEmptyParts);
}

inline bool MatchExactSearch(const QStringList &terms, const QString &text) {
	const auto folded = text.toCaseFolded();
	return std::all_of(terms.begin(), terms.end(), [&](const QString &term) {
		return folded.contains(term);
	});
}

} // namespace Purple
