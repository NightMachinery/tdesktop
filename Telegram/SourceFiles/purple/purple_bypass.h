/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Purple {
namespace internal {

inline int &BypassDepth() {
	static thread_local auto value = 0;
	return value;
}

} // namespace internal

class Bypass final {
public:
	Bypass() {
		++internal::BypassDepth();
	}
	~Bypass() {
		--internal::BypassDepth();
	}
	Bypass(const Bypass&) = delete;
	Bypass &operator=(const Bypass&) = delete;
};

[[nodiscard]] inline bool IsBypassed() {
	return (internal::BypassDepth() != 0);
}

} // namespace Purple
