#pragma once

#include "base/basic_types.h"
#include "data/data_types.h"

#include <memory>

struct TextForMimeData;
class HistoryItem;
namespace Ui { class PopupMenu; }
namespace HistoryView { class ListWidget; }

namespace Purple {

[[nodiscard]] inline bool CopyRestrictionsDisabled() {
	return true;
}
struct MarkdownCopyOperation {};
using MarkdownCopyToken = std::shared_ptr<const MarkdownCopyOperation>;
MarkdownCopyToken BeginMarkdownCopy();
[[nodiscard]] bool IsCurrentMarkdownCopy(const MarkdownCopyToken &token);
[[nodiscard]] QString CopyTextAsMarkdown(const TextForMimeData &text);
void AddCopyAsMarkdownAction(
	not_null<Ui::PopupMenu*> menu,
	Fn<TextForMimeData()> text);
void AddSelectionCopyAsMarkdownAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryView::ListWidget*> list,
	bool wholeMessages,
	bool overSelection);
void AddMarkdownDocumentCopyAction(not_null<Ui::PopupMenu*> menu,
	not_null<HistoryView::ListWidget*> list, HistoryItem *item,
	bool permitted, bool asGroup);
void AddMessageCopyAsMarkdownAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryView::ListWidget*> list,
	FullMsgId itemId,
	bool asGroup);

}
