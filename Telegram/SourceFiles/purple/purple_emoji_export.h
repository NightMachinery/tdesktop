#pragma once

#include <QtCore/QString>
#include <rpl/producer.h>

namespace Main {
class Account;
}

namespace Settings::Builder {
class SectionBuilder;
}

namespace Purple {

void StartEmojiExport();
void StopEmojiExportAtLogout(Main::Account &account);
void BuildEmojiExportSetting(::Settings::Builder::SectionBuilder &builder);
[[nodiscard]] bool EmojiExportEnabled();
void SetEmojiExportEnabled(bool enabled);
[[nodiscard]] rpl::producer<bool> EmojiExportEnabledValue();
[[nodiscard]] QString CurrentEmojiExportDirectory();

}
