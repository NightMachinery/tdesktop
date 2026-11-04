/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_mirror.h"

namespace Purple {

RecordingMirror::RecordingMirror(Open open, Failed failed)
: _open(std::move(open))
, _failed(std::move(failed)) {
}

void RecordingMirror::write(
		const void *owner,
		std::int64_t offset,
		const std::uint8_t *data,
		std::size_t size) {
	if (!owner || offset < 0 || !data || !size) {
		return;
	}
	auto lock = std::unique_lock(_mutex);
	auto i = _active.find(owner);
	if (i == _active.end()) {
		if (offset != 0 || !_open) {
			return;
		}
		lock.unlock();
		auto opened = _open();
		lock.lock();
		auto active = Active();
		if (opened && opened->file) {
			active.id = std::move(opened->id);
			active.file = std::move(opened->file);
		} else {
			active.failed = true;
		}
		i = _active.emplace(owner, std::move(active)).first;
	}
	auto &active = i->second;
	if (active.failed) {
		return;
	} else if (!active.file->writeAt(std::uint64_t(offset), data, size)) {
		active.failed = true;
		const auto id = active.id;
		lock.unlock();
		if (_failed) {
			_failed(id);
		}
	}
}

std::vector<std::string> RecordingMirror::activeIds() const {
	const auto lock = std::lock_guard(_mutex);
	auto result = std::vector<std::string>();
	for (const auto &[owner, active] : _active) {
		if (!active.id.empty()) {
			result.push_back(active.id);
		}
	}
	return result;
}

std::vector<std::string> RecordingMirror::requestCloseAll() {
	const auto lock = std::lock_guard(_mutex);
	auto result = std::vector<std::string>();
	for (auto &[owner, active] : _active) {
		active.closing = true;
		if (!active.id.empty()) {
			result.push_back(active.id);
		}
	}
	return result;
}

RecordingMirror::Closed RecordingMirror::Finish(Active &active) {
	auto result = Closed{
		.id = active.id,
		.size = active.file ? active.file->size() : 0,
		.failed = active.failed,
	};
	active.file = nullptr;
	return result;
}

std::optional<RecordingMirror::Closed> RecordingMirror::close(
		const void *owner) {
	const auto lock = std::lock_guard(_mutex);
	const auto i = _active.find(owner);
	if (i == _active.end()) {
		return std::nullopt;
	}
	auto result = Finish(i->second);
	_active.erase(i);
	return result;
}

std::vector<RecordingMirror::Closed> RecordingMirror::closeAll() {
	const auto lock = std::lock_guard(_mutex);
	auto result = std::vector<Closed>();
	for (auto &[owner, active] : _active) {
		result.push_back(Finish(active));
	}
	_active.clear();
	return result;
}

std::optional<std::string> RecordingMirror::activeId(
		const void *owner) const {
	const auto lock = std::lock_guard(_mutex);
	const auto i = _active.find(owner);
	return (i == _active.end() || i->second.id.empty())
		? std::nullopt
		: std::optional(i->second.id);
}

} // namespace Purple
