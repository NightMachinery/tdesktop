#include "purple/purple_markdown_publication.h"

namespace Purple {

uint64_t DesktopMarkdownPublication::stamp() const {
	return _stamp.load();
}

std::unique_lock<std::recursive_mutex> DesktopMarkdownPublication::write() {
	return std::unique_lock(_mutex);
}

void DesktopMarkdownPublication::committed() {
	_stamp.fetch_add(1);
}

void DesktopMarkdownPublication::revoke() {
	_active.store(false);
}

bool DesktopMarkdownPublication::publish(uint64_t opening,
		const std::function<bool()> &current,
		const std::function<void()> &deliver) {
	const auto lock = std::unique_lock(_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !_active.load() || _stamp.load() != opening
		|| !current || !current() || !deliver) {
		return false;
	}
	deliver();
	return true;
}

}
