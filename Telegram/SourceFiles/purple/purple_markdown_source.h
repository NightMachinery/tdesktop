#pragma once

#include "data/data_types.h"

class HistoryItem;
namespace Purple {

inline constexpr auto kDesktopMarkdownSourceLimit = 1024 * 1024;

struct DesktopMarkdownBody {
	QString document;
	QString name;
	QString mime;
	uint64_t size = 0;
	friend bool operator==(const DesktopMarkdownBody &, const DesktopMarkdownBody &) = default;
};
[[nodiscard]] bool IsDesktopMarkdownFile(const DesktopMarkdownBody &document);
void ReadDesktopMarkdownFile(not_null<HistoryItem*> item, Fn<void(std::optional<QString>)> done);

}
