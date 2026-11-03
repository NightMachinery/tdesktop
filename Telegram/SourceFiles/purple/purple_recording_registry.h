/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Purple::RecordingRegistry {

inline constexpr int kVersion = 1;
inline constexpr std::int64_t kListenSendWindowSeconds = 120;

struct Choice {
	int seconds = 0;
	const char *setting = "";
};

inline constexpr auto kJournalChoices = std::array{
	Choice{ 0, "off" },
	Choice{ 1, "1s" },
	Choice{ 2, "2s" },
	Choice{ 5, "5s" },
};

inline constexpr auto kBinKeepChoices = std::array{
	Choice{ 12 * 3600, "12h" },
	Choice{ 24 * 3600, "24h" },
	Choice{ 48 * 3600, "48h" },
	Choice{ 7 * 24 * 3600, "7d" },
	Choice{ 0, "off" },
};

template <std::size_t Size>
[[nodiscard]] constexpr int ChoiceIndex(
		const std::array<Choice, Size> &choices,
		int seconds) {
	for (auto i = std::size_t(); i != Size; ++i) {
		if (choices[i].seconds == seconds) {
			return int(i);
		}
	}
	return -1;
}

enum class Kind {
	Voice,
	Round,
};

enum class State {
	Recording,
	Sending,
	Sent,
	Unsent,
	Discarded,
	Recovered,
	Unrecoverable,
};

enum class Mark {
	None,
	Recovered,
	Partial,
	NotSent,
	Discarded,
	Unrecoverable,
};

struct Entry {
	std::string id;
	int account = -1;
	std::uint64_t user = 0;
	std::uint64_t peer = 0;
	std::int64_t topic = 0;
	std::int64_t reply = 0;
	Kind kind = Kind::Voice;
	std::int64_t started = 0;
	std::int64_t durationMs = 0;
	std::int64_t binned = 0;
	bool viewOnce = false;
	bool partial = false;
	bool fromListen = false;
	std::string media;
	std::string journal;
	State state = State::Recording;

	friend bool operator==(const Entry&, const Entry&) = default;
};

struct Manifest {
	std::vector<Entry> entries;

	[[nodiscard]] Entry *find(const std::string &id);
	[[nodiscard]] const Entry *find(const std::string &id) const;
	bool remove(const std::string &id);
};

enum class ReadStatus {
	Ok,
	Empty,
	UnknownVersion,
	Damaged,
};

[[nodiscard]] std::string Encode(const Manifest &manifest);
[[nodiscard]] Manifest Decode(const std::string &text, ReadStatus *status);

[[nodiscard]] const char *KindName(Kind kind);
[[nodiscard]] const char *StateName(State state);

[[nodiscard]] bool ValidFileName(const std::string &name);

enum class StopKind {
	Cancel,
	Send,
	Listen,
};

[[nodiscard]] std::optional<StopKind> StopKindFromOrdinal(int ordinal);

struct StopDecision {
	bool keepFiles = true;
	State state = State::Recording;
};

[[nodiscard]] StopDecision DecideStop(
	StopKind kind,
	bool keepDiscarded);

[[nodiscard]] bool InBin(State state);
[[nodiscard]] bool NeedsRecovery(State state);
[[nodiscard]] Mark MarkOf(const Entry &entry);

struct SentFacts {
	int account = -1;
	std::uint64_t user = 0;
	std::uint64_t peer = 0;
	Kind kind = Kind::Voice;
	std::int64_t now = 0;
};

[[nodiscard]] std::optional<std::size_t> MatchSent(
	const Manifest &manifest,
	const SentFacts &facts);

[[nodiscard]] std::vector<std::string> Expired(
	const Manifest &manifest,
	const std::function<bool(std::int64_t binned)> &expired);

[[nodiscard]] std::vector<std::string> OfAccount(
	const Manifest &manifest,
	int account);

[[nodiscard]] std::vector<std::string> AdoptOrphans(
	Manifest &manifest,
	const std::vector<std::string> &names,
	std::int64_t now);

[[nodiscard]] std::vector<const Entry*> BinNewestFirst(
	const Manifest &manifest);

} // namespace Purple::RecordingRegistry
