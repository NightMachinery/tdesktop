#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace Purple::SleepTimerRules {

using Time = std::int64_t;
inline constexpr auto kMinute = Time(60000);
inline constexpr auto kMaximum = 12 * 60 * kMinute;

inline constexpr auto kSongOwner = std::uint64_t(1);
inline constexpr auto kVoiceOwner = std::uint64_t(2);
inline constexpr auto kViewerOwner = std::uint64_t(3);
inline constexpr auto kPipOwner = std::uint64_t(4);

enum class Kind { Song, Voice, Video };
enum class Mode { Off, Timed, EndOfItem };

struct Item {
	Kind kind = Kind::Song;
	std::uint64_t session = 0;
	std::uint64_t document = 0;
	std::uint64_t peer = 0;
	std::int64_t message = 0;

	[[nodiscard]] bool operator==(const Item &) const = default;
};

struct Media {
	Item item;
	std::uint64_t owner = 0;
	std::uint64_t stream = 0;
	bool loaded = false;
	bool eligible = false;
	bool playing = false;
};

struct State {
	Mode mode = Mode::Off;
	Time deadline = 0;
	std::optional<Item> bound;
	std::uint64_t stream = 0;
	std::uint64_t owner = 0;
	std::optional<Item> pendingAdvance;
	std::uint64_t generation = 0;
};

enum class Loss { None, Silent, Notice };

struct SeekIntent {
	bool pending = false;
	bool keepPaused = false;
	bool temporaryPause = false;
};

void BeginSeek(SeekIntent &intent, bool userPaused);
void BeginAudioSeek(SeekIntent &intent, bool observedPaused, bool sameItem);
void PauseSeek(SeekIntent &intent);
void ExpireSeek(SeekIntent &intent);
void SetSeekPlaybackIntent(SeekIntent &intent, bool keepPaused);
[[nodiscard]] bool FinishSeek(SeekIntent &intent, bool held, bool actualPaused);
void BeginVideoSeek(SeekIntent &intent, bool paused);
void SetVideoSeekPlaybackIntent(
	SeekIntent &intent, bool pause, bool &restartPaused, bool &pausedBySeek);
void ToggleVideoSeekPlaybackIntent(
	SeekIntent &intent, bool paused, bool active, bool finished,
	bool &restartPaused, bool &pausedBySeek);
void PreserveVideoSeek(
	SeekIntent &intent, bool active, bool &restartPaused, bool &pausedBySeek);

struct PausePorts {
	std::function<void()> clearAudioResumes;
	std::function<void()> clearVideoResumes;
	std::function<void()> pauseVoice;
	std::function<void()> pauseSong;
	std::function<void()> pauseVideos;
};

void PauseForExpiry(const PausePorts &ports);
[[nodiscard]] bool ValidDuration(Time duration);
[[nodiscard]] Time Remaining(const State &state, Time now);
[[nodiscard]] Time CustomPrefill(const State &state, Time now, Time lastCustom);
[[nodiscard]] std::optional<Media> ChooseEnd(const std::vector<Media> &media);
[[nodiscard]] bool StartTimed(State &state, Time now, Time duration, bool loaded);
[[nodiscard]] bool StartEnd(State &state, const std::optional<Media> &media);
[[nodiscard]] bool AddTime(State &state, Time now, Time duration);
[[nodiscard]] bool Expire(State &state, Time now);
[[nodiscard]] bool Finish(State &state, const Item &item, std::uint64_t stream = 0);
[[nodiscard]] bool TakeAdvance(State &state, const Item &item);
void DropAdvance(State &state, Kind kind);
void Cancel(State &state);
[[nodiscard]] Loss Reconcile(State &state, const std::vector<Media> &media);

} // namespace Purple::SleepTimerRules
