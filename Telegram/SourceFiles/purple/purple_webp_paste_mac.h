#pragma once

#include "purple/purple_webp_paste.h"

#ifdef __OBJC__
@class NSPasteboard;
#endif

namespace Purple {

[[nodiscard]] std::optional<MarkedWebpPaste> ReadMacMarkedWebpPaste(const QMimeData *data);
#ifdef __OBJC__
[[nodiscard]] std::optional<MarkedWebpPaste> ReadMarkedWebpPasteboard(NSPasteboard *pasteboard);
#endif

}
