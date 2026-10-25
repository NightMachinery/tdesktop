#pragma once
#include "data/data_types.h"

namespace Ui {
class PopupMenu;
}
namespace HistoryView {
class ListWidget;
}

namespace Purple {
void AddArchiveHistoryAction(not_null<Ui::PopupMenu*> menu,
	not_null<HistoryView::ListWidget*> list, FullMsgId itemId);
}
