#pragma once

#include "base/weak_ptr.h"
#include "data/data_msg_id.h"

#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QString>

class HistoryInner;
class HistoryItem;

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Main {
class Session;
} // namespace Main

namespace Purple {

class MessageHints final {
public:
	enum class Action { Select, Copy };

	enum class Input { Pending, Finished, Unknown };

	MessageHints();
	~MessageHints();

	[[nodiscard]] bool start(not_null<HistoryInner*> view, Action action);
	[[nodiscard]] bool active() const;
	[[nodiscard]] Input type(int key, HistoryInner *view);
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

	[[nodiscard]] QRect clip() const;
	[[nodiscard]] std::vector<Entry> snapshot() const;
	[[nodiscard]] bool eligible(
		not_null<HistoryView::Element*> view,
		std::vector<FullMsgId> &groupIds) const;
	void watch();
	static void apply(
		not_null<HistoryInner*> view,
		not_null<HistoryItem*> item,
		Action action);

	QPointer<HistoryInner> _view;
	QPointer<Overlay> _overlay;
	base::weak_ptr<Main::Session> _session;
	Action _action = Action::Select;
	std::vector<Entry> _entries;
	QString _prefix;
	QRect _globalClip;
	rpl::lifetime _watch;

};

} // namespace Purple
