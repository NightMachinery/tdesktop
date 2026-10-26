#include "purple/purple_emoji_export_queue.h"

#include <algorithm>
#include <set>

namespace Purple {

EmojiExportQueue::EmojiExportQueue(int budget, qint64 cooldownMs)
: _budget(std::max(budget, 1))
, _cooldownMs(std::max(cooldownMs, qint64(1))) {
}

void EmojiExportQueue::begin(
		const std::vector<quint64> &ids,
		const std::vector<quint64> &extra) {
	_order.clear();
	_cursor = 0;
	_used = 0;
	const auto reserved = std::set<quint64>(extra.begin(), extra.end());
	const auto admitted = std::set<quint64>(ids.begin(), ids.end());
	for (auto i = size_t(0); i != ids.size(); ++i) {
		if (reserved.contains(ids[i])) _order.push_back(i);
	}
	for (auto i = size_t(0); i != ids.size(); ++i) {
		if (!reserved.contains(ids[i])) _order.push_back(i);
	}
	for (auto i = _retry.begin(); i != _retry.end();) {
		if (!admitted.contains(i->first)) i = _retry.erase(i);
		else ++i;
	}
}

void EmojiExportQueue::clear() {
	_order.clear();
	_retry.clear();
	_cursor = 0;
	_used = 0;
}

std::optional<size_t> EmojiExportQueue::current() const {
	return _cursor < _order.size() ? std::optional(_order[_cursor]) : std::nullopt;
}

void EmojiExportQueue::next() {
	if (_cursor < _order.size()) ++_cursor;
}

void EmojiExportQueue::continueSlice() {
	_used = 0;
}

bool EmojiExportQueue::cooling(quint64 id, qint64 now) const {
	const auto i = _retry.find(id);
	return i != _retry.end() && now < i->second;
}

EmojiResolveAdmission EmojiExportQueue::resolve(quint64 id, qint64 now) {
	if (cooling(id, now)) return EmojiResolveAdmission::Cooldown;
	if (_used == _budget) return EmojiResolveAdmission::Continue;
	++_used;
	return EmojiResolveAdmission::Ready;
}

void EmojiExportQueue::failed(quint64 id, qint64 now) {
	_retry[id] = now + _cooldownMs;
}

void EmojiExportQueue::succeeded(quint64 id) {
	_retry.erase(id);
}

std::optional<qint64> EmojiExportQueue::retryAt() const {
	auto result = std::optional<qint64>();
	for (const auto &[id, at] : _retry) {
		if (!result || at < *result) result = at;
	}
	return result;
}

}
