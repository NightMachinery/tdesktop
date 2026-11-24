#pragma once

#include "data/data_types.h"
#include "data/data_chat_filters.h"

#include <QtCore/QtTypes>
#include <optional>
#include <vector>

namespace Data {
class Session;
} // namespace Data

namespace Purple {

struct MusicSourceRosterResult {
	bool ready = false;
	bool waitingMainDialogs = false;
	bool waitingArchiveDialogs = false;
	int loadedMainDialogs = 0;
	int loadedArchiveDialogs = 0;
	std::vector<quint64> peers;
};

[[nodiscard]] inline bool MusicFilterNeedsDialogLists(Data::ChatFilter::Flags flags) {
	using Flag = Data::ChatFilter::Flag;
	constexpr auto kCategoryFlags = Flag::Contacts
		| Flag::NonContacts
		| Flag::Groups
		| Flag::Channels
		| Flag::Bots;
	return (flags & kCategoryFlags) != Flag(0);
}

[[nodiscard]] MusicSourceRosterResult InspectMusicSourceRoster(
	Data::Session &data,
	const Data::ChatFilter &filter);

[[nodiscard]] std::optional<std::vector<quint64>> CaptureMusicSourceRoster(
	Data::Session &data,
	const Data::ChatFilter &filter);

} // namespace Purple
