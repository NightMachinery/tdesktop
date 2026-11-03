/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_recovery.h"

#include "purple/purple_ogg_repair.h"

#include <algorithm>

namespace Purple::RecordingRecovery {

VoiceResult RecoverVoice(
		const std::filesystem::path &path,
		std::shared_ptr<const RecordingCrypt::Key> key,
		const RecordingCrypt::RandomFill &random,
		bool wasSending) {
	using State = RecordingRegistry::State;
	auto bytes = RecordingCrypt::ReadWhole(path, key);
	if (!bytes) {
		return {};
	}
	const auto finish = FinaliseOgg(*bytes);
	if (finish.status == OggFinishStatus::Unrecoverable) {
		return {};
	}
	const auto changed = (finish.status == OggFinishStatus::Finalised);
	if (changed && !RecordingCrypt::WriteWhole(
			path,
			key,
			random,
			bytes->data(),
			bytes->size())) {
		return {};
	}
	return {
		.state = wasSending ? State::Unsent : State::Recovered,
		.durationMs = std::max(finish.scan.durationMs, std::int64_t(0)),
		.changed = changed,
	};
}

std::int64_t VoiceDurationMs(const std::vector<std::uint8_t> &bytes) {
	const auto scan = ScanOgg(bytes.data(), bytes.size());
	return std::max(scan.durationMs, std::int64_t(0));
}

} // namespace Purple::RecordingRecovery
