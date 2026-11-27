#pragma once

#include "base/basic_types.h"
#include "base/unique_qptr.h"
#include "data/data_msg_id.h"
#include "purple/purple_window_hint_geometry.h"
#include "purple/purple_hint_role.h"
#include "ui/click_handler.h"

#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QString>

#include <vector>

class QWidget;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Purple {

struct WindowHintTarget {
	enum class Kind { Control, Message, Chat, Folder };
	using Menu = base::unique_qptr<Ui::PopupMenu>;
	using MenuBuilder = Fn<Menu()>;

	QPointer<QWidget> widget;
	QRect bounds;
	QRect bubble;
	QString name;
	bool mainList = false;
	bool reply = false;
	Kind kind = Kind::Control;
	HintRole role = HintRole::General;
	std::vector<FullMsgId> messages;
	Fn<bool()> current;
	Fn<void()> activate;
	MenuBuilder nativeMenu;
	Fn<void()> fullMenu;
	bool canReact = false;
};

class WindowHintSource {
public:
	virtual ~WindowHintSource() = default;
	virtual void collectWindowHints(
		const QRect &clip,
		std::vector<WindowHintTarget> &out) = 0;
};

[[nodiscard]] std::vector<WindowHintTarget> CollectWindowHintTargets(QWidget *root);
[[nodiscard]] HintRole WindowHintHandlerRole(const ClickHandlerPtr &handler);
void ClickWindowHint(QWidget *widget, QPoint localPoint);
void RegisterFolderHintMenu(QWidget *button, WindowHintTarget::MenuBuilder builder);
void RegisterMessageBarHint(QWidget *widget, Fn<bool()> shown);

} // namespace Purple
