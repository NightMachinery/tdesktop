#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>

namespace Purple::Tests {

inline std::atomic<bool> EmojiShutdownGapReached = false;
inline std::atomic<bool> EmojiShutdownReleaseGap = false;
inline std::atomic<bool> EmojiShutdownNotified = false;

class EmojiShutdownCondition final {
public:
	template <typename Lock, typename Predicate>
	void wait(Lock &lock, Predicate predicate) {
		while (!predicate()) {
			EmojiShutdownGapReached.store(true);
			while (!EmojiShutdownReleaseGap.load()) std::this_thread::yield();
			_condition.wait(lock);
		}
	}

	void notify_one() {
		_condition.notify_one();
		EmojiShutdownNotified.store(true);
	}

private:
	std::condition_variable _condition;

};

}
