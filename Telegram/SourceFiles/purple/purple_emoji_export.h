#pragma once

#include "purple/purple_control_protocol.h"
#include <QtCore/QString>
#include <rpl/producer.h>

namespace Main {
class Account;
}

namespace Settings::Builder {
class SectionBuilder;
}

namespace Purple {

void RequestEmojiRender(quint64 id, int pixels,
	std::function<bool()> active, ControlReply reply);
void StartEmojiExport();
void StopEmojiExportAtLogout(Main::Account &account);
void BuildEmojiExportSetting(::Settings::Builder::SectionBuilder &builder);
[[nodiscard]] bool EmojiExportEnabled();
void SetEmojiExportEnabled(bool enabled);
[[nodiscard]] rpl::producer<bool> EmojiExportEnabledValue();
[[nodiscard]] QString CurrentEmojiExportDirectory();

}
