/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_mp4_repair.h"
#include "purple/purple_recording_crypt.h"
#include "purple/purple_recording_recovery.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

struct AVFormatContext;
struct AVIOContext;
struct AVPacket;
struct AVRational;

namespace Purple::RoundMedia {

using Bytes = std::vector<std::uint8_t>;

struct ParameterSets {
	Bytes sps;
	Bytes pps;
};

[[nodiscard]] std::optional<ParameterSets> ParameterSetsFromExtradata(
	const std::uint8_t *data,
	std::size_t size);
[[nodiscard]] std::optional<ParameterSets> ParameterSetsFromSample(
	const std::uint8_t *data,
	std::size_t size);
[[nodiscard]] Bytes MakeAvcc(const ParameterSets &sets);

[[nodiscard]] std::optional<RoundConfig> ConfigOf(
	const AVFormatContext *input);

class CopyMuxer final {
public:
	[[nodiscard]] static std::unique_ptr<CopyMuxer> Start(
		std::unique_ptr<RecordingCrypt::File> file,
		const AVFormatContext *input,
		int journalSeconds);
	~CopyMuxer();

	[[nodiscard]] bool write(const AVPacket *packet);
	[[nodiscard]] bool finish();
	[[nodiscard]] std::uint64_t size() const;
	[[nodiscard]] bool failed() const {
		return _failed;
	}

	[[nodiscard]] int writeBytes(const std::uint8_t *data, int size);
	[[nodiscard]] std::int64_t seek(std::int64_t offset, int whence);

private:
	CopyMuxer() = default;

	void release();

	std::unique_ptr<RecordingCrypt::File> _file;
	AVFormatContext *_output = nullptr;
	AVIOContext *_io = nullptr;
	std::vector<AVRational> _inputBase;
	std::vector<std::int64_t> _lastDts;
	std::int64_t _position = 0;
	bool _headerWritten = false;
	bool _failed = false;

};

struct Remuxed {
	Bytes bytes;
	std::int64_t durationMs = 0;
	int videoPackets = 0;
	bool partial = false;
};

[[nodiscard]] std::optional<Remuxed> Remux(const Bytes &bytes);
[[nodiscard]] std::optional<Remuxed> Join(const std::vector<Bytes> &parts);
[[nodiscard]] std::optional<Remuxed> Rebuild(
	const Bytes &media,
	const RoundRecovery &recovery);
[[nodiscard]] AacFrameMeasure MakeAacMeasure();

[[nodiscard]] RecordingRecovery::Result Recover(
	const std::filesystem::path &media,
	const std::filesystem::path &journal,
	std::shared_ptr<const RecordingCrypt::Key> key,
	const RecordingCrypt::RandomFill &random,
	bool wasSending);

} // namespace Purple::RoundMedia
