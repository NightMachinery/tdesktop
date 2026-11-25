#pragma once

#include <QtCore/QStringList>
#include <algorithm>
#include <optional>
#include <vector>

namespace Purple::ChatPaletteSearch {

struct Folded {
	QString text;
	std::vector<qsizetype> positions;
};

inline Folded Fold(QStringView input, bool positions = false) {
	auto result = Folded();
	for (auto i = qsizetype(0); i != input.size(); ++i) {
		auto ch = input[i];
		const auto category = ch.category();
		if (ch == u'\u200c' || ch == u'\u0640'
			|| category == QChar::Mark_NonSpacing
			|| category == QChar::Mark_Enclosing
			|| category == QChar::Mark_SpacingCombining) {
			continue;
		}
		if (ch == u'ي') {
			ch = u'ی';
		} else if (ch == u'ك') {
			ch = u'ک';
		} else if (const auto digit = ch.digitValue(); digit >= 0) {
			ch = QChar(u'0' + digit);
		}
		const auto folded = QString(ch).toCaseFolded();
		result.text += folded;
		if (positions) {
			result.positions.insert(result.positions.end(), folded.size(), i);
		}
	}
	return result;
}

inline QString Convert(QStringView input, QStringView from, QStringView to) {
	auto mappedLetter = false;
	auto result = QString();
	for (const auto ch : input) {
		const auto index = from.indexOf(ch);
		if (index >= 0) {
			result += to[index];
			mappedLetter |= ch.isLetter();
		} else if (ch.isLetter()) {
			return {};
		} else {
			result += ch;
		}
	}
	return mappedLetter ? Fold(result).text : QString();
}

struct Term {
	QString original;
	QString literal;
	QStringList alternatives;
};

inline std::vector<Term> Parse(const QString &query) {
	constexpr auto persian = QStringView(
		u"ضصثقفغعهخحجچشسیبلاتنمکگظطزرذدپو.ًٌٍَُِّْ][}{|ؤئيإأآة»«:؛كٓژٰ‌ٔء<>؟٬٫﷼٪×،)(ـ۱۲۳۴۵۶۷۸۹۰");
	constexpr auto english = QStringView(
		uR"(qwertyuiop[]asdfghjkl;'zxcvbnm,.QWERTYUIOP{}|ASDFGHJKL:"ZXCVBNM<>?@#$%^&()_1234567890)");
	static_assert(persian.size() == english.size());
	auto result = std::vector<Term>();
	for (const auto &word : query.simplified().split(u' ', Qt::SkipEmptyParts)) {
		auto term = Term{ word.toCaseFolded(), Fold(word).text, {} };
		if (word.size() >= 2 && !word.startsWith(u'@')) {
			for (auto other : { Convert(word, english, persian),
					Convert(word, persian, english) }) {
				if (!other.isEmpty() && other != term.literal
					&& !term.alternatives.contains(other)) {
					term.alternatives.push_back(std::move(other));
				}
			}
		}
		result.push_back(std::move(term));
	}
	return result;
}

inline int NameTier(QStringView name, QStringView term, bool converted) {
	if (term.isEmpty()) {
		return 8;
	} else if (name == term && converted) {
		return 1;
	} else if (name.startsWith(term)) {
		return converted ? 3 : 0;
	}
	for (auto i = name.indexOf(term); i >= 0; i = name.indexOf(term, i + 1)) {
		if (!name[i - 1].isLetterOrNumber()) {
			return converted ? 4 : 1;
		}
	}
	return name.contains(term) ? (converted ? 5 : 2) : 8;
}

inline std::optional<int> Rank(
		const std::vector<Term> &terms,
		const QString &name,
		const QString &address,
		const QString &note) {
	const auto foldedName = Fold(name).text;
	const auto foldedAddress = Fold(address).text;
	const auto foldedNote = Fold(note).text;
	const auto originalName = name.toCaseFolded();
	const auto originalAddress = address.toCaseFolded();
	const auto originalNote = note.toCaseFolded();
	const auto original = originalName + u' ' + originalAddress + u' ' + originalNote;
	auto result = 0;
	for (const auto &term : terms) {
		auto tier = std::min(NameTier(foldedName, term.literal, false),
			NameTier(originalName, term.original, false));
		for (const auto &converted : term.alternatives) {
			tier = std::min(tier, NameTier(foldedName, converted, true));
		}
		if (tier == 8 && (!term.literal.isEmpty() && foldedAddress.contains(term.literal)
			|| originalAddress.contains(term.original))) {
			tier = 6;
		}
		if (tier == 8 && (!term.literal.isEmpty() && foldedNote.contains(term.literal)
			|| original.contains(term.original))) {
			tier = 7;
		}
		if (tier == 8) {
			return std::nullopt;
		}
		result = std::max(result, tier);
	}
	return result;
}

} // namespace Purple::ChatPaletteSearch
