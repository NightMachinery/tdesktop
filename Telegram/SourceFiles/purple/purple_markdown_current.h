#pragma once

#include "purple/purple_markdown_source.h"
#include "data/data_types.h"
#include "rpl/producer.h"
#include "iv/iv_rich_page.h"

class HistoryItem;
namespace Main { class Session; }

namespace Purple {

void RememberDesktopRichPreview(not_null<Main::Session*> session, const MTPMessage &message);
struct DesktopRichResponse {
	Main::Session *session = nullptr;
	FullMsgId id;
	MTPmessages_Messages response;
};
[[nodiscard]] rpl::producer<DesktopRichResponse> DesktopRichResponses();
void ObserveDesktopRichResponse(not_null<Main::Session*> session,
	FullMsgId id, const MTPmessages_Messages &response);
[[nodiscard]] std::optional<MTPMessage> DesktopRichCopyOpening(
	not_null<Main::Session*> session, FullMsgId id);
[[nodiscard]] std::optional<MTPMessage> DesktopRichCopyCachedMessage(
	not_null<Main::Session*> session, FullMsgId id);
[[nodiscard]] bool AdmitDesktopRichCopyPage(const Iv::RichPage &preview,
	const Iv::RichPage &page, const std::optional<MTPMessage> &opening,
	const std::optional<MTPMessage> &complete,
	const std::shared_ptr<const Iv::RichPage> &decoded);
[[nodiscard]] std::optional<MTPMessage> ValidateDesktopRichCopyResponse(
	const MTPMessage &opening, const MTPmessages_Messages &response);
[[nodiscard]] bool DesktopRichWireExtends(const MTPRichMessage &preview, const MTPRichMessage &full);
[[nodiscard]] DesktopMarkdownBody DesktopMarkdownBodyForItem(not_null<HistoryItem*> item);
[[nodiscard]] bool DesktopRichPageExtends(const Iv::RichPage &preview, const Iv::RichPage &full);

}
