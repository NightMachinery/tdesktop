#include "purple/purple_quit_defaults.h"

#include "core/core_settings.h"

namespace Purple {

void ApplyQuitPreferenceDefault(Core::Settings &settings) {
	if (!settings.readPref<bool>("purple-quit-default-applied")) {
		settings.setMacWarnBeforeQuit(false);
		settings.writePref<bool>("purple-quit-default-applied", true);
	}
}

} // namespace Purple
