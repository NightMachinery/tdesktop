#pragma once

#include "base/basic_types.h"
#include "data/data_chat_filters.h"

namespace Main {
class Session;
} // namespace Main

namespace Purple {

[[nodiscard]] FilterId HomeFilterId(not_null<Main::Session*> session);

} // namespace Purple
