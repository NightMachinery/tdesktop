#pragma once

#include <QtCore/QtTypes>
#include <optional>
#include <vector>

namespace Data {
class ChatFilter;
class Session;
} // namespace Data

namespace Purple {

[[nodiscard]] std::optional<std::vector<quint64>> CaptureMusicSourceRoster(
	Data::Session &data,
	const Data::ChatFilter &filter);

} // namespace Purple
