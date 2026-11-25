#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTextBoundaryFinder>

#include <algorithm>
#include <utility>
#include <vector>

namespace Purple::ModalMx {

constexpr auto kRecentLimit = 7;
constexpr auto kInputHistoryLimit = 100;

[[nodiscard]] inline bool StockModeShortcut(
		int resolvedKey,
		Qt::KeyboardModifiers modifiers,
		bool enabled,
		bool composing) {
	return enabled && !composing
		&& modifiers == Qt::AltModifier && resolvedKey == Qt::Key_X;
}

struct History {
	QStringList recent;
	QHash<QString, int> counts;
	QStringList inputs;

	void record(const QString &commandId, const QString &query) {
		if (!commandId.isEmpty()) {
			recent.removeAll(commandId);
			recent.prepend(commandId);
			while (recent.size() > kRecentLimit) {
				recent.removeLast();
			}
			counts[commandId] = counts.value(commandId, 0) + 1;
		}
		const auto trimmed = query.trimmed();
		if (!trimmed.isEmpty()) {
			inputs.removeAll(trimmed);
			inputs.prepend(trimmed);
			while (inputs.size() > kInputHistoryLimit) {
				inputs.removeLast();
			}
		}
	}

	[[nodiscard]] QByteArray serialize() const {
		auto root = QJsonObject();
		auto rec = QJsonArray();
		for (const auto &id : recent) {
			rec.append(id);
		}
		auto cnt = QJsonObject();
		for (auto it = counts.begin(); it != counts.end(); ++it) {
			cnt.insert(it.key(), it.value());
		}
		auto inp = QJsonArray();
		for (const auto &line : inputs) {
			inp.append(line);
		}
		root.insert(QStringLiteral("recent"), rec);
		root.insert(QStringLiteral("counts"), cnt);
		root.insert(QStringLiteral("inputs"), inp);
		return QJsonDocument(root).toJson(QJsonDocument::Compact);
	}

	[[nodiscard]] static History Deserialize(const QByteArray &bytes) {
		auto result = History();
		const auto doc = QJsonDocument::fromJson(bytes);
		if (!doc.isObject()) {
			return result;
		}
		const auto root = doc.object();
		for (const auto &v : root.value(QStringLiteral("recent")).toArray()) {
			if (const auto s = v.toString(); !s.isEmpty()
					&& result.recent.size() < kRecentLimit) {
				result.recent.push_back(s);
			}
		}
		const auto cnt = root.value(QStringLiteral("counts")).toObject();
		for (auto it = cnt.begin(); it != cnt.end(); ++it) {
			if (const auto n = it.value().toInt(); n > 0) {
				result.counts.insert(it.key(), n);
			}
		}
		for (const auto &v : root.value(QStringLiteral("inputs")).toArray()) {
			if (const auto s = v.toString(); !s.isEmpty()
					&& result.inputs.size() < kInputHistoryLimit) {
				result.inputs.push_back(s);
			}
		}
		return result;
	}
};

struct HistoryStep {
	int index = -1;
	QString text;
};

[[nodiscard]] inline HistoryStep StepInputHistory(
		const QStringList &inputs,
		int currentIndex,
		int delta,
		const QString &draft) {
	const auto next = std::clamp(
		currentIndex + delta,
		-1,
		int(inputs.size()) - 1);
	return {
		.index = next,
		.text = (next < 0) ? draft : inputs[next],
	};
}

[[nodiscard]] inline QString WordInitials(const QString &label) {
	auto out = QString();
	auto atWordStart = true;
	for (const auto ch : label) {
		if (ch.isLetterOrNumber()) {
			if (atWordStart) {
				out.append(ch.toLower());
				atWordStart = false;
			}
		} else {
			atWordStart = true;
		}
	}
	return out;
}

struct OrderlessToken {
	QString lowered;
	QRegularExpression regex;
	bool hasRegex = false;
};

[[nodiscard]] inline std::vector<OrderlessToken> ParseTokens(
		const QString &query) {
	auto result = std::vector<OrderlessToken>();
	const auto parts = query.simplified().split(u' ', Qt::SkipEmptyParts);
	result.reserve(parts.size());
	for (const auto &part : parts) {
		auto token = OrderlessToken{
			.lowered = part.toCaseFolded(),
		};
		auto re = QRegularExpression(
			part,
			QRegularExpression::CaseInsensitiveOption);
		if (re.isValid()) {
			token.regex = std::move(re);
			token.hasRegex = true;
		}
		result.push_back(std::move(token));
	}
	return result;
}

[[nodiscard]] inline bool MatchToken(
		const OrderlessToken &token,
		const QString &haystackFolded,
		const QString &labelInitials) {
	if (haystackFolded.contains(token.lowered)) {
		return true;
	}
	if (labelInitials.contains(token.lowered)) {
		return true;
	}
	return token.hasRegex && token.regex.match(haystackFolded).hasMatch();
}

struct HighlightSpan {
	int start = 0;
	int length = 0;
};

[[nodiscard]] inline std::vector<HighlightSpan> TextMatchSpans(
		const QString &text,
		const std::vector<OrderlessToken> &tokens,
		bool matchInitials = true) {
	if (text.isEmpty() || tokens.empty()) {
		return {};
	}
	auto marks = std::vector<bool>(text.size(), false);
	const auto lowered = text.toLower();
	for (const auto &token : tokens) {
		if (token.lowered.isEmpty()) {
			continue;
		}
		if (const auto pos = lowered.indexOf(token.lowered); pos >= 0) {
			const auto till = std::min(
				int(text.size()),
				int(pos + token.lowered.size()));
			for (auto i = int(pos); i < till; ++i) {
				marks[i] = true;
			}
			continue;
		}
		if (token.hasRegex) {
			const auto m = token.regex.match(text);
			if (m.hasMatch() && m.capturedLength() > 0) {
				const auto from = std::max(0, int(m.capturedStart()));
				const auto till = std::min(
					int(text.size()),
					int(m.capturedEnd()));
				for (auto i = from; i < till; ++i) {
					marks[i] = true;
				}
				continue;
			}
		}
		if (!matchInitials) {
			continue;
		}
		const auto initials = WordInitials(text);
		if (const auto ipos = initials.indexOf(token.lowered); ipos >= 0) {
			auto wordIdx = 0;
			auto atStart = true;
			for (auto i = 0; i < text.size(); ++i) {
				if (text[i].isLetterOrNumber()) {
					if (atStart) {
						if (wordIdx >= ipos
								&& wordIdx < ipos + token.lowered.size()) {
							marks[i] = true;
						}
						++wordIdx;
						atStart = false;
					}
				} else {
					atStart = true;
				}
			}
		}
	}
	auto spans = std::vector<HighlightSpan>();
	for (auto i = 0; i < int(marks.size());) {
		if (!marks[i]) {
			++i;
			continue;
		}
		auto j = i + 1;
		while (j < int(marks.size()) && marks[j]) {
			++j;
		}
		spans.push_back({ i, j - i });
		i = j;
	}
	return spans;
}

[[nodiscard]] inline std::vector<HighlightSpan> LabelMatchSpans(
		const QString &label,
		const std::vector<OrderlessToken> &tokens) {
	return TextMatchSpans(label, tokens, true);
}

[[nodiscard]] inline std::vector<HighlightSpan> SubtitleMatchSpans(
		const QString &subtitle,
		const std::vector<OrderlessToken> &tokens) {
	return TextMatchSpans(subtitle, tokens, false);
}

struct Candidate {
	int payloadIndex = 0;
	QString id;
	QString label;
	QString category;
	QString keysText;
	bool available = true;
	QString aliases;
};

// Returns -1 if the candidate does not match all tokens; otherwise:
//   0 = empty query or a match that needs alias words
//   3 = every token matches the title (label or its word initials)
//   2 = at least one token matches the title, remaining tokens match metadata
//   1 = matched only via group/category, :command-id, or key chord text
[[nodiscard]] inline int CandidateMatchTier(
		const Candidate &it,
		const std::vector<OrderlessToken> &tokens) {
	if (tokens.empty()) {
		return 0;
	}
	const auto labelFolded = it.label.toCaseFolded();
	const auto initials = WordInitials(it.label);
	const auto fullHay = (it.label + u' ' + it.id + u' '
		+ it.category + u' ' + it.keysText).toCaseFolded();
	const auto aliases = it.aliases.toCaseFolded();
	auto titleMatches = 0;
	auto needsAlias = false;
	for (const auto &t : tokens) {
		if (MatchToken(t, labelFolded, initials)) {
			++titleMatches;
		} else if (!MatchToken(t, fullHay, initials)) {
			if (aliases.isEmpty() || !MatchToken(t, aliases, QString())) {
				return -1;
			}
			needsAlias = true;
		}
	}
	if (needsAlias) {
		return 0;
	}
	if (titleMatches == int(tokens.size())) {
		return 3;
	}
	return (titleMatches > 0) ? 2 : 1;
}

[[nodiscard]] inline std::vector<int> RankAndFilter(
		const std::vector<Candidate> &items,
		const History &history,
		const QString &query) {
	auto recentRank = QHash<QString, int>();
	for (auto i = 0; i < history.recent.size() && i < kRecentLimit; ++i) {
		recentRank.insert(history.recent[i], i);
	}
	const auto tokens = ParseTokens(query);
	struct Scored {
		int index = 0;
		int tier = 0;
	};
	auto scored = std::vector<Scored>();
	scored.reserve(items.size());
	for (auto i = 0; i < int(items.size()); ++i) {
		const auto tier = CandidateMatchTier(items[i], tokens);
		if (tier >= 0) {
			scored.push_back({ i, tier });
		}
	}
	std::stable_sort(scored.begin(), scored.end(), [&](const Scored &sa, const Scored &sb) {
		const auto &ca = items[sa.index];
		const auto &cb = items[sb.index];
		if (ca.available != cb.available) {
			return ca.available;
		}
		if (sa.tier != sb.tier) {
			return sa.tier > sb.tier;
		}
		const auto ra = recentRank.value(ca.id, kRecentLimit);
		const auto rb = recentRank.value(cb.id, kRecentLimit);
		if (ra != rb) {
			return ra < rb;
		}
		if (ra == kRecentLimit) {
			const auto fa = history.counts.value(ca.id, 0);
			const auto fb = history.counts.value(cb.id, 0);
			if (fa != fb) {
				return fa > fb;
			}
		}
		if (ca.label.size() != cb.label.size()) {
			return ca.label.size() < cb.label.size();
		}
		return QString::compare(ca.label, cb.label, Qt::CaseInsensitive) < 0;
	});
	auto matched = std::vector<int>();
	matched.reserve(scored.size());
	for (const auto &s : scored) {
		matched.push_back(s.index);
	}
	return matched;
}

[[nodiscard]] inline QString FirstGrapheme(const QString &raw) {
	const auto trimmed = raw.trimmed();
	if (trimmed.isEmpty()) {
		return QString();
	}
	auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, trimmed);
	finder.toStart();
	const auto end = finder.toNextBoundary();
	return (end > 0) ? trimmed.left(end) : trimmed;
}

} // namespace Purple::ModalMx
