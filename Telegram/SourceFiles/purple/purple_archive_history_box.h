#pragma once
#include "data/data_types.h"
#include "purple/purple_archive_rows.h"

namespace Window {
class SessionController;
}

namespace Ui {
class PopupMenu;
}
namespace HistoryView {
class ListWidget;
}

namespace Purple {
[[nodiscard]] QString ArchiveVersionPreview(const ArchiveVersionSnapshot &snapshot);
void ShowArchiveHistory(not_null<Window::SessionController*> controller, ArchiveMessageKey key);
void AddArchiveHistoryAction(not_null<Ui::PopupMenu*> menu,
	not_null<HistoryView::ListWidget*> list, FullMsgId itemId);
}
