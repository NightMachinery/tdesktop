#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <optional>

namespace Purple {

struct RecordingCaptureTarget {
	int account = -1;
	std::uint64_t user = 0;
	std::uint64_t peer = 0;
	std::int64_t topic = 0;
	std::int64_t reply = 0;
};

class RecordingTargetMailbox {
public:
	void set(std::optional<RecordingCaptureTarget> target) {
		std::atomic_store(&_target, target ? std::make_shared<const RecordingCaptureTarget>(*target) : nullptr);
	}
	[[nodiscard]] std::shared_ptr<const RecordingCaptureTarget> get() const { return std::atomic_load(&_target); }

private:
	std::shared_ptr<const RecordingCaptureTarget> _target;

};

class RecordingDockModel {
public:
	enum class Phase { Empty, Recording, Paused, Finishing };

	[[nodiscard]] bool active() const { return _phase != Phase::Empty; }
	[[nodiscard]] bool paused() const { return _phase == Phase::Paused; }
	[[nodiscard]] bool finishing() const { return _phase == Phase::Finishing; }
	[[nodiscard]] std::uint64_t generation() const { return _generation; }
	[[nodiscard]] std::uint64_t pauseRevision() const { return _pauseRevision; }
	[[nodiscard]] bool accepts(std::uint64_t generation) const {
		return active() && !finishing() && generation == _generation;
	}
	bool begin() {
		if (active()) return false;
		++_generation;
		_phase = Phase::Recording;
		return true;
	}
	bool pause(std::uint64_t generation, bool paused) {
		if (!accepts(generation)) return false;
		++_pauseRevision;
		_phase = paused ? Phase::Paused : Phase::Recording;
		return true;
	}
	bool finish(std::uint64_t generation) {
		if (!accepts(generation)) return false;
		_phase = Phase::Finishing;
		return true;
	}
	void clear() { _phase = Phase::Empty; ++_generation; }

private:
	Phase _phase = Phase::Empty;
	std::uint64_t _generation = 0;
	std::uint64_t _pauseRevision = 0;

};

} // namespace Purple
