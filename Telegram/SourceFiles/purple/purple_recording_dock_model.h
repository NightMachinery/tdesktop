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

[[nodiscard]] inline bool RecordingComposeActive(bool floating, bool nativeActive) {
	return nativeActive && !floating;
}

[[nodiscard]] inline bool SameRecordingThread(const RecordingCaptureTarget &a, const RecordingCaptureTarget &b) {
	return a.account == b.account && a.user == b.user && a.peer == b.peer && a.topic == b.topic;
}

[[nodiscard]] inline std::optional<RecordingCaptureTarget> ReplacementRecordingTarget(
		const RecordingCaptureTarget &original, RecordingCaptureTarget chosen) {
	if (original.account != chosen.account || original.user != chosen.user || !chosen.peer) return std::nullopt;
	chosen.reply = 0;
	return chosen;
}

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
	[[nodiscard]] bool transitioning() const { return _transitioning; }
	[[nodiscard]] std::uint64_t generation() const { return _generation; }
	[[nodiscard]] std::uint64_t pauseRevision() const { return _pauseRevision; }
	[[nodiscard]] std::uint64_t targetRevision() const { return _targetRevision; }
	[[nodiscard]] bool accepts(std::uint64_t generation) const {
		return active() && !finishing() && generation == _generation;
	}
	bool begin() {
		if (active()) return false;
		++_generation;
		_phase = Phase::Recording;
		return true;
	}
	bool receiveCompletion(std::uint64_t generation) {
		if (!active() || generation != _generation) return false;
		_phase = Phase::Paused;
		_transitioning = false;
		return true;
	}
	bool retarget(std::uint64_t generation) {
		if (!accepts(generation) || _transitioning) return false;
		++_targetRevision;
		return true;
	}
	bool requestPause(std::uint64_t generation) {
		if (!accepts(generation) || _transitioning) return false;
		_transitioning = true;
		return true;
	}
	bool pause(std::uint64_t generation, bool paused) {
		if (!accepts(generation)) return false;
		_transitioning = false;
		++_pauseRevision;
		_phase = paused ? Phase::Paused : Phase::Recording;
		return true;
	}
	bool finish(std::uint64_t generation) {
		if (!accepts(generation) || _transitioning) return false;
		_phase = Phase::Finishing;
		return true;
	}
	void clear() { _phase = Phase::Empty; _transitioning = false; ++_generation; }

private:
	Phase _phase = Phase::Empty;
	std::uint64_t _generation = 0;
	std::uint64_t _pauseRevision = 0;
	std::uint64_t _targetRevision = 0;
	bool _transitioning = false;

};

} // namespace Purple
