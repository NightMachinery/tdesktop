#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Purple::KeyScroll {

class Motion {
public:
	using Time = std::int64_t;

	void step(Time now, int position, int maximum, int delta) {
		_origin = position;
		_destination = int(std::clamp<std::int64_t>(
			std::int64_t(position) + delta, 0, maximum));
		_started = now;
		_phase = (_origin == _destination) ? Phase::Idle : Phase::Step;
	}

	void startHold(Time now, int direction) {
		if (_phase == Phase::Hold && _direction == direction) {
			return;
		}
		_phase = Phase::Hold;
		_direction = direction;
		_started = _lastFrame = now;
		_remainder = 0.;
	}

	void release(Time now) {
		if (_phase == Phase::Hold) {
			const auto from = std::max(_lastFrame, now - kMaxFrameMs);
			_remainder += _direction * (distance(now - _started) - distance(from - _started));
			_speedAtRelease = kSpeed * progress(now - _started, kRampMs);
			_phase = Phase::Brake;
			_started = _lastFrame = now;
		}
	}

	void cancel() {
		_phase = Phase::Idle;
	}

	[[nodiscard]] bool active() const {
		return _phase != Phase::Idle;
	}

	[[nodiscard]] bool continuous() const {
		return _phase == Phase::Hold || _phase == Phase::Brake;
	}

	[[nodiscard]] int advance(Time now, int position, int maximum) {
		if (_phase == Phase::Idle) {
			return position;
		} else if (_phase == Phase::Step) {
			const auto left = 1. - progress(now - _started, kStepMs);
			const auto next = int(std::lround(
				_destination + (_origin - _destination) * left * left * left));
			if (!left || next == _destination || next > maximum) {
				cancel();
			}
			return std::clamp(next, 0, maximum);
		}
		const auto from = std::max(_lastFrame, now - kMaxFrameMs);
		_lastFrame = now;
		const auto delta = distance(now - _started) - distance(from - _started);
		const auto total = _direction * delta + _remainder;
		const auto pixels = int(std::lround(total));
		_remainder = total - pixels;
		const auto next = int(std::clamp<std::int64_t>(
			std::int64_t(position) + pixels, 0, maximum));
		if ((_direction < 0 && next == 0) || (_direction > 0 && next == maximum)
			|| (_phase == Phase::Brake && now - _started >= kBrakeMs)) {
			cancel();
		}
		return next;
	}

private:
	enum class Phase { Idle, Step, Hold, Brake };

	static constexpr auto kStepMs = Time(90);
	static constexpr auto kRampMs = Time(100);
	static constexpr auto kBrakeMs = Time(80);
	static constexpr auto kMaxFrameMs = Time(40);
	static constexpr auto kSpeed = 1.4;

	[[nodiscard]] static double progress(Time elapsed, Time duration) {
		return std::clamp(double(elapsed) / double(duration), 0., 1.);
	}

	[[nodiscard]] double distance(Time elapsed) const {
		if (_phase == Phase::Brake) {
			const auto t = double(std::clamp(elapsed, Time(0), kBrakeMs));
			return _speedAtRelease * t * (1. - t / (2. * kBrakeMs));
		}
		const auto t = double(std::max(elapsed, Time(0)));
		const auto ramp = std::min(t, double(kRampMs));
		return kSpeed * (ramp * ramp / (2. * kRampMs) + t - ramp);
	}

	Phase _phase = Phase::Idle;
	Time _started = 0;
	Time _lastFrame = 0;
	int _origin = 0;
	int _destination = 0;
	int _direction = 0;
	double _speedAtRelease = 0.;
	double _remainder = 0.;
};

} // namespace Purple::KeyScroll
