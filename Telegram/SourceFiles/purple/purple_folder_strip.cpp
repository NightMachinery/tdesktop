#include "purple/purple_folder_strip.h"

#include "data/data_session.h"
#include "main/main_session.h"
#include "purple/purple_gate.h"

namespace Purple {

FilterId HomeFilterId(not_null<Main::Session*> session) {
	return ActiveResolved().normal
		? FilterId()
		: session->data().chatsFilters().defaultId();
}

} // namespace Purple
