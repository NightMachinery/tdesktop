/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_crypt.h"

#include <openssl/evp.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>

namespace Purple::RecordingCrypt {
namespace {

constexpr auto kMagic = std::array<std::uint8_t, 4>{ 'P', 'R', 'C', '1' };
constexpr auto kChunk = std::size_t(64 * 1024);

[[nodiscard]] std::array<std::uint8_t, kHeaderBytes> Header(
		const Key &key,
		const Iv &iv) {
	auto result = std::array<std::uint8_t, kHeaderBytes>{};
	std::copy(kMagic.begin(), kMagic.end(), result.begin());
	result[4] = std::uint8_t(kVersion & 0xFF);
	result[5] = std::uint8_t(kVersion >> 8);
	std::copy(iv.begin(), iv.end(), result.begin() + 8);
	std::copy(key.check.begin(), key.check.end(), result.begin() + 24);
	return result;
}

[[nodiscard]] Iv CounterAt(const Iv &iv, std::uint64_t block) {
	auto result = iv;
	auto carry = block;
	for (auto i = int(kIvBytes) - 1; i >= 0 && carry; --i) {
		const auto sum = std::uint64_t(result[i]) + (carry & 0xFF);
		result[i] = std::uint8_t(sum & 0xFF);
		carry = (carry >> 8) + (sum >> 8);
	}
	return result;
}

[[nodiscard]] bool SyncToDisk(const std::filesystem::path &path) {
#ifdef _WIN32
	const auto handle = CreateFileW(
		path.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	const auto result = (FlushFileBuffers(handle) != 0);
	CloseHandle(handle);
	return result;
#else
	const auto descriptor = ::open(path.c_str(), O_RDONLY);
	if (descriptor < 0) {
		return false;
	}
	const auto result = (::fsync(descriptor) == 0);
	::close(descriptor);
	return result;
#endif
}

} // namespace

bool Apply(
		const Key &key,
		const Iv &iv,
		std::uint64_t offset,
		std::uint8_t *data,
		std::size_t size) {
	if (!size) {
		return true;
	}
	const auto context = std::unique_ptr<
		EVP_CIPHER_CTX,
		decltype(&EVP_CIPHER_CTX_free)>(
			EVP_CIPHER_CTX_new(),
			EVP_CIPHER_CTX_free);
	if (!context) {
		return false;
	}
	const auto counter = CounterAt(iv, offset / 16);
	if (EVP_EncryptInit_ex(
			context.get(),
			EVP_aes_256_ctr(),
			nullptr,
			key.bytes.data(),
			counter.data()) != 1) {
		return false;
	}
	auto skip = std::array<std::uint8_t, 16>{};
	auto written = 0;
	const auto lead = int(offset % 16);
	if (lead && EVP_EncryptUpdate(
			context.get(),
			skip.data(),
			&written,
			skip.data(),
			lead) != 1) {
		return false;
	}
	while (size) {
		const auto step = int(std::min(size, kChunk));
		if (EVP_EncryptUpdate(
				context.get(),
				data,
				&written,
				data,
				step) != 1 || written != step) {
			return false;
		}
		data += step;
		size -= step;
	}
	return true;
}

File::File(
	std::filesystem::path path,
	std::shared_ptr<const Key> key,
	Iv iv,
	std::fstream stream,
	std::uint64_t size)
: _path(std::move(path))
, _key(std::move(key))
, _iv(iv)
, _stream(std::move(stream))
, _size(size) {
}

std::unique_ptr<File> File::Create(
		const std::filesystem::path &path,
		std::shared_ptr<const Key> key,
		const RandomFill &random) try {
	auto iv = Iv();
	if (!key || !random || !random(iv.data(), iv.size())) {
		return nullptr;
	}
	auto stream = std::fstream(
		path,
		std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
	if (!stream) {
		return nullptr;
	}
	const auto header = Header(*key, iv);
	stream.write(
		reinterpret_cast<const char*>(header.data()),
		std::streamsize(header.size()));
	stream.flush();
	if (!stream) {
		return nullptr;
	}
	return std::unique_ptr<File>(
		new File(path, std::move(key), iv, std::move(stream), 0));
} catch (...) {
	return nullptr;
}

std::unique_ptr<File> File::Open(
		const std::filesystem::path &path,
		std::shared_ptr<const Key> key,
		OpenStatus *status) try {
	const auto fail = [&](OpenStatus value) {
		if (status) {
			*status = value;
		}
		return std::unique_ptr<File>();
	};
	auto error = std::error_code();
	const auto bytes = std::filesystem::file_size(path, error);
	if (error || !key) {
		return fail(OpenStatus::Missing);
	} else if (bytes < kHeaderBytes) {
		return fail(OpenStatus::NotOurs);
	}
	auto stream = std::fstream(
		path,
		std::ios::binary | std::ios::in | std::ios::out);
	auto header = std::array<std::uint8_t, kHeaderBytes>{};
	if (!stream || !stream.read(
			reinterpret_cast<char*>(header.data()),
			std::streamsize(header.size()))) {
		return fail(OpenStatus::Missing);
	} else if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
		return fail(OpenStatus::NotOurs);
	} else if ((header[4] | (header[5] << 8)) != kVersion) {
		return fail(OpenStatus::UnknownVersion);
	} else if (!std::equal(
			key->check.begin(),
			key->check.end(),
			header.begin() + 24)) {
		return fail(OpenStatus::WrongKey);
	}
	auto iv = Iv();
	std::copy(header.begin() + 8, header.begin() + 24, iv.begin());
	if (status) {
		*status = OpenStatus::Ok;
	}
	return std::unique_ptr<File>(new File(
		path,
		std::move(key),
		iv,
		std::move(stream),
		bytes - kHeaderBytes));
} catch (...) {
	if (status) {
		*status = OpenStatus::Missing;
	}
	return nullptr;
}

bool File::writeAt(
		std::uint64_t offset,
		const std::uint8_t *data,
		std::size_t size) try {
	if (!size) {
		return true;
	} else if (offset > std::numeric_limits<std::uint64_t>::max() - size) {
		return false;
	}
	_buffer.assign(data, data + size);
	if (!Apply(*_key, _iv, offset, _buffer.data(), _buffer.size())) {
		return false;
	}
	_stream.clear();
	_stream.seekp(std::streamoff(kHeaderBytes + offset));
	_stream.write(
		reinterpret_cast<const char*>(_buffer.data()),
		std::streamsize(_buffer.size()));
	_stream.flush();
	if (!_stream) {
		return false;
	}
	_size = std::max(_size, offset + size);
	return true;
} catch (...) {
	return false;
}

bool File::readAt(
		std::uint64_t offset,
		std::uint8_t *data,
		std::size_t size) try {
	if (offset > _size || size > _size - offset) {
		return false;
	} else if (!size) {
		return true;
	}
	_stream.clear();
	_stream.seekg(std::streamoff(kHeaderBytes + offset));
	if (!_stream.read(reinterpret_cast<char*>(data), std::streamsize(size))) {
		return false;
	}
	return Apply(*_key, _iv, offset, data, size);
} catch (...) {
	return false;
}

std::optional<std::vector<std::uint8_t>> File::readAll(
		std::uint64_t maxBytes) try {
	if (_size > maxBytes
		|| _size > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
		return std::nullopt;
	}
	auto result = std::vector<std::uint8_t>(std::size_t(_size));
	if (!readAt(0, result.data(), result.size())) {
		return std::nullopt;
	}
	return result;
} catch (...) {
	return std::nullopt;
}

bool WriteWhole(
		const std::filesystem::path &path,
		std::shared_ptr<const Key> key,
		const RandomFill &random,
		const std::uint8_t *data,
		std::size_t size) try {
	auto temporary = path;
	temporary += ".new";
	{
		const auto file = File::Create(temporary, std::move(key), random);
		if (!file || !file->writeAt(0, data, size)) {
			auto ignored = std::error_code();
			std::filesystem::remove(temporary, ignored);
			return false;
		}
	}
	auto error = std::error_code();
	if (!SyncToDisk(temporary)) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	std::filesystem::rename(temporary, path, error);
	if (error) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
} catch (...) {
	return false;
}

std::optional<std::vector<std::uint8_t>> ReadWhole(
		const std::filesystem::path &path,
		std::shared_ptr<const Key> key,
		OpenStatus *status,
		std::uint64_t maxBytes) {
	const auto file = File::Open(path, std::move(key), status);
	return file ? file->readAll(maxBytes) : std::nullopt;
}

} // namespace Purple::RecordingCrypt
