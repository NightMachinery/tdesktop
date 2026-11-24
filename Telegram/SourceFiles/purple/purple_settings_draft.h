#pragma once

#include <QtCore/QString>

#include <optional>
#include <variant>
#include <vector>

namespace Purple::SettingsDraft {

template <typename Frame>
class History final {
public:
	[[nodiscard]] const Frame *current() const {
		return _frames.empty() || !_frames.back() ? nullptr : &*_frames.back();
	}
	[[nodiscard]] bool canUndo() const {
		return !_frames.empty();
	}
	void push(std::optional<Frame> frame) {
		if (frame || current()) {
			_frames.push_back(std::move(frame));
		}
	}
	void undo() {
		if (canUndo()) {
			_frames.pop_back();
		}
	}
	void clear() {
		_frames.clear();
	}

private:
	std::vector<std::optional<Frame>> _frames;
};

[[nodiscard]] inline bool ShouldRecapture(bool stale, const auto *current) {
	return stale && !current;
}

using FormValue = std::variant<QString, int>;

[[nodiscard]] inline bool NeedsDiscardConfirmation(const FormValue &baseline, const FormValue &current) {
	return baseline != current;
}

[[nodiscard]] inline bool CanRestoreForm(bool sessionCurrent, bool openingCurrent) {
	return sessionCurrent && openingCurrent;
}

} // namespace Purple::SettingsDraft
