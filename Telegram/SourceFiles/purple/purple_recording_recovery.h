/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_recording_crypt.h"
#include "purple/purple_recording_registry.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Purple::RecordingRecovery {

struct Result {
	RecordingRegistry::State state = RecordingRegistry::State::Unrecoverable;
	std::int64_t durationMs = 0;
	bool changed = false;
	bool partial = false;
};

[[nodiscard]] Result RecoverVoice(
	const std::filesystem::path &path,
	std::shared_ptr<const RecordingCrypt::Key> key,
	const RecordingCrypt::RandomFill &random,
	bool wasSending);

[[nodiscard]] std::int64_t VoiceDurationMs(
	const std::vector<std::uint8_t> &bytes);

} // namespace Purple::RecordingRecovery
