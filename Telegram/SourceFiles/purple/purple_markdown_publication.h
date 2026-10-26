#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace Purple {

class DesktopMarkdownPublication final {
public:
	[[nodiscard]] uint64_t stamp() const;
	[[nodiscard]] std::unique_lock<std::recursive_mutex> write();
	void committed();
	void revoke();
	[[nodiscard]] bool publish(uint64_t stamp,
		const std::function<bool()> &current,
		const std::function<void()> &deliver);

private:
	std::recursive_mutex _mutex;
	std::atomic<uint64_t> _stamp = 0;
	std::atomic<bool> _active = true;

};

}
