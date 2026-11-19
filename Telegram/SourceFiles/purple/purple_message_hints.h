#pragma once

#include "base/weak_ptr.h"
#include "data/data_msg_id.h"

#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QString>

class QContextMenuEvent;
class HistoryInner;
class HistoryItem;

namespace HistoryView {
class Element;
class ListWidget;
} // namespace HistoryView

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Purple {

class MessageHints final {
public:
	enum class Action { Select, Copy, Menu, Link, Reply, Edit };

	enum class Input { Pending, Finished, Composing, Unknown };

	struct Target {
		HistoryInner *main = nullptr;
		HistoryView::ListWidget *section = nullptr;
		bool scheduled = false;

		[[nodiscard]] explicit operator bool() const {
			return main || section;
		}
		[[nodiscard]] Ui::RpWidget *widget() const;
		[[nodiscard]] Main::Session &session() const;
		[[nodiscard]] bool supportsCompose(Action action) const;
	};

	MessageHints();
	~MessageHints();

	[[nodiscard]] bool start(Target view, Action action);
	[[nodiscard]] bool active() const;
	[[nodiscard]] Input type(int key, Target view);
	void clear();

private:
	class Overlay;

	struct Entry {
		FullMsgId id;
		const HistoryView::Element *element = nullptr;
		QRect rect;
		std::vector<FullMsgId> group;
		QString label;

		friend bool operator==(const Entry &, const Entry &) = default;
	};

	[[nodiscard]] Target target() const;
	[[nodiscard]] QRect clip() const;
	[[nodiscard]] std::vector<Entry> snapshot() const;
	[[nodiscard]] bool eligible(
		not_null<HistoryView::Element*> view,
		std::vector<FullMsgId> &groupIds) const;
	[[nodiscard]] static bool canCompose(
		Target view,
		not_null<HistoryItem*> item,
		Action action);
	void watch();
	static Input apply(
		Target view,
		not_null<HistoryItem*> item,
		Action action,
		QPoint point);

	QPointer<Ui::RpWidget> _view;
	QPointer<Overlay> _overlay;
	base::weak_ptr<Main::Session> _session;
	Action _action = Action::Select;
	bool _scheduled = false;
	std::vector<Entry> _entries;
	QString _prefix;
	QRect _globalClip;
	rpl::lifetime _watch;

};

[[nodiscard]] HistoryItem *MessageHintMenuItem(
	QContextMenuEvent *event,
	HistoryItem *focused);

} // namespace Purple
