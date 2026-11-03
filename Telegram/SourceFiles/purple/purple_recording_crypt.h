/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Purple::RecordingCrypt {

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kIvBytes = 16;
inline constexpr std::size_t kCheckBytes = 8;
inline constexpr std::size_t kHeaderBytes = 32;
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint64_t kMaxReadBytes = 512ULL * 1024 * 1024;

struct Key {
	std::array<std::uint8_t, kKeyBytes> bytes = {};
	std::array<std::uint8_t, kCheckBytes> check = {};
};

using Iv = std::array<std::uint8_t, kIvBytes>;
using RandomFill = std::function<bool(std::uint8_t *data, std::size_t size)>;

[[nodiscard]] bool Apply(
	const Key &key,
	const Iv &iv,
	std::uint64_t offset,
	std::uint8_t *data,
	std::size_t size);

enum class OpenStatus {
	Ok,
	Missing,
	NotOurs,
	UnknownVersion,
	WrongKey,
};

class File final {
public:
	[[nodiscard]] static std::unique_ptr<File> Create(
		const std::filesystem::path &path,
		std::shared_ptr<const Key> key,
		const RandomFill &random);
	[[nodiscard]] static std::unique_ptr<File> Open(
		const std::filesystem::path &path,
		std::shared_ptr<const Key> key,
		OpenStatus *status = nullptr);

	[[nodiscard]] bool writeAt(
		std::uint64_t offset,
		const std::uint8_t *data,
		std::size_t size);
	[[nodiscard]] bool readAt(
		std::uint64_t offset,
		std::uint8_t *data,
		std::size_t size);
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> readAll(
		std::uint64_t maxBytes = kMaxReadBytes);
	[[nodiscard]] std::uint64_t size() const {
		return _size;
	}
	[[nodiscard]] const std::filesystem::path &path() const {
		return _path;
	}

private:
	File(
		std::filesystem::path path,
		std::shared_ptr<const Key> key,
		Iv iv,
		std::fstream stream,
		std::uint64_t size);

	std::filesystem::path _path;
	std::shared_ptr<const Key> _key;
	Iv _iv = {};
	std::fstream _stream;
	std::uint64_t _size = 0;
	std::vector<std::uint8_t> _buffer;

};

[[nodiscard]] bool WriteWhole(
	const std::filesystem::path &path,
	std::shared_ptr<const Key> key,
	const RandomFill &random,
	const std::uint8_t *data,
	std::size_t size);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadWhole(
	const std::filesystem::path &path,
	std::shared_ptr<const Key> key,
	OpenStatus *status = nullptr,
	std::uint64_t maxBytes = kMaxReadBytes);

} // namespace Purple::RecordingCrypt
