/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_recording_crypt.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Purple {

class RecordingMirror final {
public:
	struct Opened {
		std::string id;
		std::unique_ptr<RecordingCrypt::File> file;
	};
	struct Closed {
		std::string id;
		std::uint64_t size = 0;
		bool failed = false;
	};
	using Open = std::function<std::optional<Opened>()>;
	using Failed = std::function<void(const std::string &id)>;
	using Restarted = std::function<void(Closed closed)>;

	RecordingMirror(Open open, Failed failed, Restarted restarted);

	void write(
		const void *owner,
		std::int64_t offset,
		const std::uint8_t *data,
		std::size_t size);
	[[nodiscard]] std::vector<std::string> requestCloseAll();
	[[nodiscard]] std::optional<Closed> close(const void *owner);
	[[nodiscard]] std::vector<Closed> closeAll();
	[[nodiscard]] std::optional<std::string> activeId(
		const void *owner) const;

private:
	struct Active {
		std::string id;
		std::unique_ptr<RecordingCrypt::File> file;
		bool failed = false;
		bool closing = false;
	};

	[[nodiscard]] static Closed Finish(Active &active);

	const Open _open;
	const Failed _failed;
	const Restarted _restarted;
	mutable std::mutex _mutex;
	std::map<const void*, Active> _active;

};

} // namespace Purple
