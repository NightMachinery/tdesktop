#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <utility>
#include <vector>

namespace Purple::ModalMx {

constexpr auto kRecentLimit = 7;
constexpr auto kInputHistoryLimit = 100;

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
		root.insert(u"recent"_q, rec);
		root.insert(u"counts"_q, cnt);
		root.insert(u"inputs"_q, inp);
		return QJsonDocument(root).toJson(QJsonDocument::Compact);
	}

	[[nodiscard]] static History Deserialize(const QByteArray &bytes) {
		auto result = History();
		const auto doc = QJsonDocument::fromJson(bytes);
		if (!doc.isObject()) {
			return result;
		}
		const auto root = doc.object();
		for (const auto &v : root.value(u"recent"_q).toArray()) {
			if (const auto s = v.toString(); !s.isEmpty()
					&& result.recent.size() < kRecentLimit) {
				result.recent.push_back(s);
			}
		}
		const auto cnt = root.value(u"counts"_q).toObject();
		for (auto it = cnt.begin(); it != cnt.end(); ++it) {
			if (const auto n = it.value().toInt(); n > 0) {
				result.counts.insert(it.key(), n);
			}
		}
		for (const auto &v : root.value(u"inputs"_q).toArray()) {
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

[[nodiscard]] inline std::vector<HighlightSpan> LabelMatchSpans(
		const QString &label,
		const std::vector<OrderlessToken> &tokens) {
	if (label.isEmpty() || tokens.empty()) {
		return {};
	}
	auto marks = std::vector<bool>(label.size(), false);
	const auto lowered = label.toLower();
	for (const auto &token : tokens) {
		if (token.lowered.isEmpty()) {
			continue;
		}
		if (const auto pos = lowered.indexOf(token.lowered); pos >= 0) {
			const auto till = std::min(
				int(label.size()),
				int(pos + token.lowered.size()));
			for (auto i = int(pos); i < till; ++i) {
				marks[i] = true;
			}
			continue;
		}
		if (token.hasRegex) {
			const auto m = token.regex.match(label);
			if (m.hasMatch() && m.capturedLength() > 0) {
				const auto from = std::max(0, int(m.capturedStart()));
				const auto till = std::min(
					int(label.size()),
					int(m.capturedEnd()));
				for (auto i = from; i < till; ++i) {
					marks[i] = true;
				}
				continue;
			}
		}
		const auto initials = WordInitials(label);
		if (const auto ipos = initials.indexOf(token.lowered); ipos >= 0) {
			auto wordIdx = 0;
			auto atStart = true;
			for (auto i = 0; i < label.size(); ++i) {
				if (label[i].isLetterOrNumber()) {
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

struct Candidate {
	int payloadIndex = 0;
	QString id;
	QString label;
	QString category;
	QString keysText;
	bool available = true;
};

[[nodiscard]] inline std::vector<int> RankAndFilter(
		const std::vector<Candidate> &items,
		const History &history,
		const QString &query) {
	auto recentRank = QHash<QString, int>();
	for (auto i = 0; i < history.recent.size() && i < kRecentLimit; ++i) {
		recentRank.insert(history.recent[i], i);
	}
	const auto amxLess = [&](int a, int b) {
		const auto &ca = items[a];
		const auto &cb = items[b];
		if (ca.available != cb.available) {
			return ca.available;
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
	};

	const auto tokens = ParseTokens(query);
	auto matched = std::vector<int>();
	matched.reserve(items.size());
	for (auto i = 0; i < int(items.size()); ++i) {
		if (tokens.empty()) {
			matched.push_back(i);
			continue;
		}
		const auto &it = items[i];
		const auto hay = (it.label + u' ' + it.id + u' '
			+ it.category + u' ' + it.keysText).toCaseFolded();
		const auto initials = WordInitials(it.label);
		const auto ok = std::all_of(
			tokens.begin(),
			tokens.end(),
			[&](const OrderlessToken &t) {
				return MatchToken(t, hay, initials);
			});
		if (ok) {
			matched.push_back(i);
		}
	}
	if (tokens.empty()) {
		std::stable_sort(matched.begin(), matched.end(), amxLess);
	} else {
		std::stable_sort(matched.begin(), matched.end(), [&](int a, int b) {
			const auto &ca = items[a];
			const auto &cb = items[b];
			if (ca.available != cb.available) {
				return ca.available;
			}
			if (ca.label.size() != cb.label.size()) {
				return ca.label.size() < cb.label.size();
			}
			return amxLess(a, b);
		});
	}
	return matched;
}

} // namespace Purple::ModalMx
