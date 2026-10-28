#include "purple/purple_emoji_large.h"

#include "base/basic_types.h"
#include <algorithm>
#include <limits>

namespace Purple {
namespace {

qint64 Later(qint64 now, qint64 delay) {
	const auto start = std::max(qint64(0), now);
	return start + std::min(delay, std::numeric_limits<qint64>::max() - start);
}

}

std::optional<quint64> EmojiLargeRequestId(const QString &name) {
	if (name.isEmpty() || name.size() > 20 || name.front() == u'0') return std::nullopt;
	for (const auto ch : name) {
		if (ch < u'0' || ch > u'9') return std::nullopt;
	}
	auto ok = false;
	const auto id = name.toULongLong(&ok);
	return ok && id && QString::number(id) == name ? std::optional(id) : std::nullopt;
}

QSize EmojiLargeRenderSize(EmojiExportSource source, QSize native) {
	if (source == EmojiExportSource::Tgs) return QSize(kEmojiLargePixels, kEmojiLargePixels);
	return native.width() > 0 && native.height() > 0
		&& native.width() <= kEmojiLargePixels && native.height() <= kEmojiLargePixels
		? native : QSize();
}

QString EmojiExportSourceName(EmojiExportSource source) {
	switch (source) {
	case EmojiExportSource::Tgs: return u"tgs"_q;
	case EmojiExportSource::Webm: return u"webm"_q;
	case EmojiExportSource::Webp: return u"webp"_q;
	}
	return {};
}

qint64 NormalizeEmojiLargeCap(qint64 bytes) {
	return bytes <= 0 ? kEmojiLargeDefaultBytes
		: std::clamp(bytes, kEmojiLargeMinimumBytes, kEmojiLargeMaximumBytes);
}

EmojiLargeRequestQueue::EmojiLargeRequestQueue(size_t maximum, qint64 spacingMs)
: _maximum(std::clamp(maximum, size_t(1), size_t(64)))
, _spacingMs(std::max(spacingMs, qint64(1))) {
}

EmojiLargeAdmission EmojiLargeRequestQueue::offer(quint64 id, qint64 now) {
	if (!id) return EmojiLargeAdmission::Invalid;
	if (_present.contains(id)) return EmojiLargeAdmission::Duplicate;
	for (auto i = _failed.begin(); i != _failed.end();) {
		if (i->second <= now) i = _failed.erase(i);
		else ++i;
	}
	if (_failed.contains(id)) return EmojiLargeAdmission::Cooldown;
	if (_present.size() >= _maximum) return EmojiLargeAdmission::Full;
	_present.insert(id);
	_pending.push_back(id);
	return EmojiLargeAdmission::Queued;
}

std::optional<quint64> EmojiLargeRequestQueue::take(qint64 now) {
	if (_active || _pending.empty() || now < _nextAt) return std::nullopt;
	const auto id = _pending.front();
	_pending.pop_front();
	_active = id;
	_nextAt = Later(now, _spacingMs);
	return id;
}

void EmojiLargeRequestQueue::complete(quint64 id, bool success, qint64 now) {
	if (_active != id) return;
	_active.reset();
	_present.erase(id);
	if (success) _failed.erase(id);
	else if (id) {
		if (_failed.size() >= 1024) {
			const auto oldest = std::min_element(_failed.begin(), _failed.end(),
				[](const auto &a, const auto &b) { return a.second < b.second; });
			_failed.erase(oldest);
		}
		_failed[id] = Later(now, 60000);
	}
}

void EmojiLargeRequestQueue::clear() {
	_nextAt = 0;
	_pending.clear();
	_active.reset();
	_present.clear();
	_failed.clear();
}

size_t EmojiLargeRequestQueue::size() const {
	return _present.size();
}

bool EmojiLargeRequestQueue::pending() const {
	return !_pending.empty();
}

EmojiSetTitleCache::EmojiSetTitleCache(size_t maximum)
: _maximum(maximum) {
}

void EmojiSetTitleCache::known(quint64 id, EmojiSetTitle title) {
	if (!id || (!_sets.contains(id) && _sets.size() >= _maximum)) return;
	_sets[id] = Entry{ std::move(title), 0 };
}

std::optional<EmojiSetTitle> EmojiSetTitleCache::lookup(quint64 id) const {
	const auto i = _sets.find(id);
	return i == _sets.end() ? std::nullopt : i->second.title;
}

bool EmojiSetTitleCache::canBegin(quint64 id, qint64 now) const {
	if (!id || _active) return false;
	const auto entry = _sets.find(id);
	return entry == _sets.end() ? _sets.size() < _maximum
		: !entry->second.title && now >= entry->second.retryAt;
}

bool EmojiSetTitleCache::begin(quint64 id, qint64 now) {
	if (!canBegin(id, now)) return false;
	_sets.try_emplace(id);
	_active = id;
	return true;
}

void EmojiSetTitleCache::defer(quint64 id) {
	if (_active == id) _active.reset();
}

std::optional<qint64> EmojiSetTitleCache::retryAt(qint64 now) const {
	auto result = std::optional<qint64>();
	for (const auto &[id, entry] : _sets) {
		if (!entry.title && now < entry.retryAt
			&& (!result || entry.retryAt < *result)) result = entry.retryAt;
	}
	return result;
}

void EmojiSetTitleCache::complete(quint64 id, EmojiSetTitle title) {
	if (_active != id) return;
	_active.reset();
	if (!_sets[id].title) known(id, std::move(title));
}

void EmojiSetTitleCache::failed(quint64 id, qint64 now) {
	if (_active != id) return;
	_active.reset();
	_sets[id].retryAt = Later(now, 60000);
}

void EmojiSetTitleCache::clear() {
	_active.reset();
	_sets.clear();
}

std::optional<quint64> EmojiSetTitleCache::active() const {
	return _active;
}

}
