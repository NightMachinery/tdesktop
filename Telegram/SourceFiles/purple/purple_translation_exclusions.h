/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Purple {

[[nodiscard]] inline std::vector<uint64_t> NormalizeExcludedTranslationChatIds(
		std::vector<uint64_t> ids) {
	ids.erase(
		std::remove(ids.begin(), ids.end(), uint64_t(0)),
		ids.end());
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

[[nodiscard]] inline std::vector<uint64_t> ParseExcludedTranslationChatIds(
		const QByteArray &stored) {
	auto ids = std::vector<uint64_t>();
	for (const auto &raw : stored.split(',')) {
		const auto trimmed = raw.trimmed();
		if (trimmed.isEmpty()) {
			continue;
		}
		auto ok = false;
		const auto value = trimmed.toULongLong(&ok);
		if (ok && value != 0) {
			ids.push_back(uint64_t(value));
		}
	}
	return NormalizeExcludedTranslationChatIds(std::move(ids));
}

[[nodiscard]] inline QByteArray SerializeExcludedTranslationChatIds(
		std::vector<uint64_t> ids) {
	ids = NormalizeExcludedTranslationChatIds(std::move(ids));
	auto parts = QList<QByteArray>();
	parts.reserve(int(ids.size()));
	for (const auto id : ids) {
		parts.push_back(QByteArray::number(static_cast<qulonglong>(id)));
	}
	return parts.join(',');
}

[[nodiscard]] inline bool IsTranslationChatExcluded(
		const std::vector<uint64_t> &ids,
		uint64_t peerId,
		uint64_t migratedFromId = 0) {
	const auto contains = [&](uint64_t id) {
		return (id != 0)
			&& (std::find(ids.begin(), ids.end(), id) != ids.end());
	};
	return contains(peerId) || contains(migratedFromId);
}

[[nodiscard]] inline std::vector<uint64_t> SetExcludedTranslationChatId(
		std::vector<uint64_t> ids,
		uint64_t peerId,
		uint64_t migratedFromId,
		bool excluded) {
	if (excluded) {
		if (peerId != 0) {
			ids.push_back(peerId);
		}
	} else {
		ids.erase(
			std::remove_if(ids.begin(), ids.end(), [&](uint64_t id) {
				return (peerId != 0 && id == peerId)
					|| (migratedFromId != 0 && id == migratedFromId);
			}),
			ids.end());
	}
	return NormalizeExcludedTranslationChatIds(std::move(ids));
}

[[nodiscard]] inline bool ShouldSuppressChatTranslation(
		bool purpleExcluded,
		bool upstreamDisabled) {
	return purpleExcluded || upstreamDisabled;
}

} // namespace Purple
