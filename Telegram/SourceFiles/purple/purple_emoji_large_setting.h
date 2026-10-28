#pragma once

#include <QtCore/QtGlobal>
#include <rpl/producer.h>

namespace Settings::Builder { class SectionBuilder; }

namespace Purple {

[[nodiscard]] qint64 EmojiLargeCacheCap();
[[nodiscard]] rpl::producer<qint64> EmojiLargeCacheCapValue();
void SetEmojiLargeCacheCap(qint64 bytes);
void BuildEmojiLargeCacheSetting(::Settings::Builder::SectionBuilder &builder);

}
