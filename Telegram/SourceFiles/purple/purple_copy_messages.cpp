#include "purple/purple_copy.h"

#include "apiwrap.h"
#include "main/main_account.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_markdown_current.h"
#include "base/weak_ptr.h"
#include "base/unixtime.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_config.h"
#include "purple/purple_markdown_delivery.h"
#include "purple/purple_archive_markdown.h"
#include <QtCore/QCryptographicHash>
#include "base/weak_qptr.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/view/history_view_list_widget.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_text.h"
#include "iv/iv_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "purple/purple_article_markdown.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_menu_icons.h"

#include <QtCore/QStringList>
#include <QtCore/QJsonDocument>

#include <memory>
#include <map>

MessageIdsList HistoryView::ListWidget::purpleSelectedIdsInOrder() const {
	auto ids = getSelectedIds();
	ranges::sort(ids, [&](FullMsgId a, FullMsgId b) {
		const auto first = session().data().message(a);
		const auto second = session().data().message(b);
		if (bool(first) != bool(second)) {
			return bool(first);
		}
		return first ? _delegate->listIsLessInOrder(first, second) : a < b;
	});
	return ids;
}

namespace Purple {
namespace {

struct DisplayedArticleState {
	const HistoryMessageTranslation *identity = nullptr;
	LanguageId historyTarget;
	LanguageId translationTarget;
	std::shared_ptr<const Iv::RichPage> translationPage;
	std::shared_ptr<const Iv::RichPage> displayedPage;
	bool used = false;
	bool requested = false;
	bool failed = false;

	[[nodiscard]] bool translated() const {
		return identity && used && translationTarget == historyTarget;
	}

	friend bool operator==(const DisplayedArticleState &,
		const DisplayedArticleState &) = default;
};

DisplayedArticleState ReadDisplayedArticleState(const HistoryItem *item) {
	const auto translation = item->translation();
	return {
		.identity = translation,
		.historyTarget = item->history()->translatedTo(),
		.translationTarget = translation ? translation->to : LanguageId(),
		.translationPage = translation ? translation->richPage : nullptr,
		.displayedPage = item->translatedRichPage(),
		.used = translation && translation->used,
		.requested = translation && translation->requested,
		.failed = translation && translation->failed,
	};
}

MessageIdsList GroupIds(not_null<HistoryItem*> item) {
	auto ids = MessageIdsList();
	if (const auto group = item->history()->session().data().groups().find(item)) {
		for (const auto member : group->items) { ids.push_back(member->fullId()); }
	}
	return ids;
}

class CopyBatch final : public std::enable_shared_from_this<CopyBatch> {
public:
	CopyBatch(
		not_null<HistoryView::ListWidget*> list,
		MessageIdsList ids,
		bool selection,
		bool group);

	void start();
	void cancel() {
		_cancelled = true;
	}

private:
	struct Entry {
		FullMsgId id;
		const HistoryItem *identity = nullptr;
		History *historyIdentity = nullptr;
		History *migratedFrom = nullptr;
		History *migratedTo = nullptr;
		MessageIdsList groupIds;
		TextWithEntities original;
		DesktopMarkdownBody body;
		TimeId date = 0;
		TimeId editDate = 0;
		std::shared_ptr<const Iv::RichPage> inlinePage;
		std::shared_ptr<const Iv::RichPage> fullPage;
		std::optional<MTPMessage> richOpening;
		std::optional<MTPMessage> richCompletion;
		std::shared_ptr<const Iv::RichPage> verifiedResponse;
		uint64 version = 0;
		DisplayedArticleState display;
		TextForMimeData ordinary;
		QString markdown;
	};

	[[nodiscard]] bool contextValid() const;
	[[nodiscard]] bool entriesValid() const;
	[[nodiscard]] bool displayStatesValid() const;
	void publish();
	void ordinary(size_t index);
	void rendered(size_t index, const std::shared_ptr<const Iv::RichPage> &page);
	void next(size_t index);
	void failed(const QString &error);

	base::weak_qptr<HistoryView::ListWidget> _list;
	base::weak_ptr<Main::Session> _session;
	base::weak_ptr<Window::SessionController> _controller;
	base::weak_ptr<Window::Controller> _window;
	const Main::Account *_domainAccount = nullptr;
	MarkdownCopyToken _copyToken;
	MessageIdsList _ids;
	std::vector<Entry> _entries;
	std::optional<DesktopMarkdownCopyScope> _archiveScope;
	bool _hasGenuine = false;
	std::vector<ArchiveEvent> _resolvedBodies;
	qsizetype _resolvedBytes = 0;
	bool _selection = false;
	bool _group = false;
	bool _cancelled = false;
	rpl::lifetime _lifetime;

};

CopyBatch::CopyBatch(
		not_null<HistoryView::ListWidget*> list,
		MessageIdsList ids,
		bool selection,
		bool group)
: _list(base::make_weak(list))
, _session(base::make_weak(&list->session()))
, _controller(base::make_weak(list->controller()))
, _window(base::make_weak(&list->controller()->window()))
, _domainAccount(&Core::App().activeAccount())
, _copyToken(BeginMarkdownCopy())
, _ids(std::move(ids))
, _selection(selection)
, _group(group) {
}

bool CopyBatch::contextValid() const {
	const auto list = _list.get();
	const auto session = _session.get();
	const auto controller = _controller.get();
	const auto window = _window.get();
	if (_cancelled || !IsCurrentMarkdownCopy(_copyToken) || !list || !session || !controller || !window
		|| &list->session() != session || list->controller() != controller
		|| window->maybeSession() != session
		|| window->sessionController() != controller
		|| Core::App().activeWindow() != window
		|| &Core::App().activeAccount() != _domainAccount
		|| window->locked() || Core::App().passcodeLocked()
		|| Core::App().screenIsLocked() || (_selection && session->frozen())
		|| session->account().loggingOut() || session->account().maybeSession() != session) {
		return false;
	}
	if (_selection && list->purpleSelectedIdsInOrder() != _ids) {
		return false;
	}
	if (_group) {
		const auto item = session->data().message(_ids.front());
		const auto group = item ? session->data().groups().find(item) : nullptr;
		if (!group) {
			return false;
		}
		auto current = MessageIdsList();
		for (const auto item : group->items) current.push_back(item->fullId());
		if (current != _ids) {
			return false;
		}
	}
	return true;
}

bool CopyBatch::displayStatesValid() const {
	const auto session = _session.get();
	if (!session) return false;
	for (const auto &entry : _entries) {
		const auto item = session->data().message(entry.id);
		const auto edited = item ? item->Get<HistoryMessageEdited>() : nullptr;
		if (!item || item != entry.identity || item->originalText() != entry.original
			|| DesktopMarkdownBodyForItem(item) != entry.body
			|| item->date() != entry.date || (edited ? edited->date : 0) != entry.editDate
			|| item->history()->migrateFrom() != entry.migratedFrom
			|| item->history()->migrateToOrMe().get() != entry.migratedTo
			|| GroupIds(item) != entry.groupIds
			|| ReadDisplayedArticleState(item) != entry.display) {
			return false;
		}
	}
	return true;
}

bool CopyBatch::entriesValid() const {
	if (!contextValid() || !displayStatesValid()) {
		return false;
	}
	const auto owner = &_session.get()->data();
	for (const auto &entry : _entries) {
		const auto current = owner->message(entry.id);
		if (!current || current != entry.identity
			|| current->richPage() != entry.inlinePage
			|| current->fullRichPageVersion() != entry.version
			|| current->fullRichPage() != entry.fullPage) {
			return false;
		}
		if (!entry.inlinePage && !entry.fullPage) {
			const auto text = HistoryItemText(current);
			if (text.rich != entry.ordinary.rich || text.expanded != entry.ordinary.expanded) {
				return false;
			}
		}
	}
	return true;
}

void CopyBatch::failed(const QString &error) {
	if (contextValid()) {
		_controller.get()->showToast(error);
	}
	cancel();
}

void CopyBatch::start() {
	if (_ids.empty() || !contextValid()) {
		return;
	}
	_archiveScope = OpenDesktopMarkdownCopy(_session.get());
	const auto owner = &_session.get()->data();
	for (const auto id : _ids) {
		const auto item = owner->message(id);
		if (!item) {
			failed(u"The messages changed. Try copying again."_q);
			return;
		}
		const auto inlinePage = item->richPage();
		const auto fullPage = item->fullRichPage();
		auto entry = Entry();
		entry.id = id;
		entry.identity = item;
		entry.historyIdentity = item->history();
		entry.migratedFrom = item->history()->migrateFrom();
		entry.migratedTo = item->history()->migrateToOrMe();
		entry.groupIds = GroupIds(item);
		entry.original = item->originalText();
		entry.body = DesktopMarkdownBodyForItem(item);
		entry.date = item->date();
		const auto edited = item->Get<HistoryMessageEdited>();
		entry.editDate = edited ? edited->date : 0;
		entry.inlinePage = inlinePage;
		entry.fullPage = fullPage;
		if (inlinePage && (inlinePage->part || (fullPage && fullPage != inlinePage))) {
			entry.richOpening = DesktopRichCopyOpening(_session.get(), id);
			entry.richCompletion = DesktopRichCopyCachedMessage(_session.get(), id);
			if (entry.richCompletion && entry.richCompletion->type() == mtpc_message) {
				if (const auto rich = entry.richCompletion->c_message().vrich_message()) {
					entry.verifiedResponse = Iv::ParseRichPage(_session.get(), *rich);
				}
			}
		}
		entry.version = item->fullRichPageVersion();
		entry.display = ReadDisplayedArticleState(item);
		entry.ordinary = inlinePage || fullPage ? TextForMimeData() : HistoryItemText(item);
		if ((inlinePage || fullPage) && entry.display.translated()
			&& !SelectCompleteArticlePage(inlinePage, fullPage,
				entry.display.translationPage, true)) {
			failed(u"The complete displayed translation is unavailable. Nothing was copied."_q);
			return;
		}
		_entries.push_back(std::move(entry));
	}
	const auto weak = std::weak_ptr<CopyBatch>(shared_from_this());
	DesktopRichResponses() | rpl::on_next([=](const DesktopRichResponse &response) {
		const auto strong = weak.lock();
		if (!strong || !strong->contextValid() || !strong->displayStatesValid()
			|| response.session != strong->_session.get()) { return; }
		for (auto &entry : strong->_entries) {
			if (entry.id != response.id || !entry.richOpening || !entry.inlinePage) { continue; }
			entry.verifiedResponse.reset();
			entry.richCompletion.reset();
			const auto verified = ValidateDesktopRichCopyResponse(*entry.richOpening, response.response);
			if (!verified) { continue; }
			const auto page = Iv::ParseRichPage(response.session, *verified->c_message().vrich_message());
			if (!page || !DesktopRichPageExtends(*entry.inlinePage, *page)) { continue; }
			entry.verifiedResponse = page;
			entry.richCompletion = *verified;
			const auto bytes = tl::count_length(*verified);
			if (bytes > 8 * 1024 * 1024 - strong->_resolvedBytes) { continue; }
			auto wire = mtpBuffer();
			verified->write(wire);
			const auto raw = QByteArray(reinterpret_cast<const char*>(wire.constData()), wire.size() * sizeof(mtpPrime));
			const auto id = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
			if (auto event = DesktopArchiveSnapshot(SessionAccountKey(response.session), *verified,
					ArchiveOrigin::Hydration, id, base::unixtime::now())) {
				AttachDesktopArticleMarkdown(*event, *page);
				const auto retainedBytes = event->observation.payload.size()
					+ (event->content ? QJsonDocument(event->content->rich.toObject()).toJson().size() : 0);
				if (retainedBytes > 8 * 1024 * 1024 - strong->_resolvedBytes) { continue; }
				strong->_resolvedBytes += retainedBytes;
				strong->_resolvedBodies.push_back(*event);
			}
		}
	}, _lifetime);
	owner->itemResizeRequest() | rpl::on_next([=] {
		if (const auto strong = weak.lock()) {
			if (!strong->displayStatesValid()) strong->cancel();
		}
	}, _lifetime);
	_session.get()->changes().realtimeHistoryUpdates(
		Data::HistoryUpdate::Flag::TranslatedTo
	) | rpl::on_next([=](const Data::HistoryUpdate &update) {
		if (const auto strong = weak.lock()) {
			if (ranges::any_of(strong->_entries, [&](const Entry &entry) {
				return entry.historyIdentity == update.history;
			})) strong->cancel();
		}
	}, _lifetime);
	owner->itemRemoved() | rpl::on_next([=](not_null<const HistoryItem*> item) {
		if (const auto strong = weak.lock()) {
			if (ranges::contains(strong->_ids, item->fullId())) strong->cancel();
		}
	}, _lifetime);
	_session.get()->changes().realtimeMessageUpdates(Data::MessageUpdate::Flag::Edited) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (const auto strong = weak.lock()) {
			if (ranges::contains(strong->_ids, update.item->fullId())) strong->cancel();
		}
	}, _lifetime);
	_session.get()->changes().realtimePeerUpdates(Data::PeerUpdate::Flag::Migration)
		| rpl::on_next([=] {
			if (const auto strong = weak.lock()) {
				if (!strong->displayStatesValid()) strong->cancel();
			}
		}, _lifetime);
	Core::App().domain().activeChanges() | rpl::on_next([=] {
		if (const auto strong = weak.lock()) strong->cancel();
	}, _lifetime);
	_window.get()->sessionControllerChanges() | rpl::on_next([=] {
		if (const auto strong = weak.lock()) strong->cancel();
	}, _lifetime);
	owner->sessionDataAboutToBeCleared() | rpl::on_next([=] {
		if (const auto strong = weak.lock()) strong->cancel();
	}, _lifetime);
	next(0);
}

void CopyBatch::next(size_t index) {
	if (!entriesValid()) {
		failed(u"The messages or copy context changed. Try again."_q);
		return;
	}
	if (index == _entries.size()) {
		publish();
		return;
	}
	auto &entry = _entries[index];
	if (!entry.inlinePage && !entry.fullPage) {
		ordinary(index);
		return;
	}
	if (const auto page = SelectCompleteArticlePage(entry.inlinePage,
		entry.fullPage, entry.display.translationPage, entry.display.translated())) {
		rendered(index, page);
		return;
	}
	if (entry.display.translated()) {
		failed(u"The complete displayed translation is unavailable. Nothing was copied."_q);
		return;
	}
	if (!entry.richOpening) {
		failed(u"The original article preview is unavailable. Nothing was copied."_q);
		return;
	}
	const auto session = _session.get();
	const auto item = session->data().message(entry.id);
	const auto self = shared_from_this();
	Core::App().iv().resolveRichMessage(session, item, [=](std::shared_ptr<const Iv::RichPage> page) {
		if (!self->contextValid() || !self->displayStatesValid()) return;
		auto &entry = self->_entries[index];
		const auto session = self->_session.get();
		const auto current = session->data().message(entry.id);
		if (!current || current != entry.identity
			|| current->richPage() != entry.inlinePage
			|| current->fullRichPageVersion() != entry.version
			|| !page || page->part || !entry.verifiedResponse || *entry.verifiedResponse != *page
			|| (current->fullRichPage() != page && current->richPage() != page)) {
			self->failed(u"The complete current article is unavailable. Nothing was copied."_q);
			return;
		}
		entry.fullPage = current->fullRichPage();
		self->rendered(index, page);
	});
}

void CopyBatch::publish() {
	if (!entriesValid()) { return; }
	auto parts = QStringList();
	for (const auto &entry : _entries) { parts.push_back(entry.markdown); }
	auto markdown = parts.join(u"\n\n"_q);
	if (_group && !_hasGenuine && ranges::none_of(_entries, [](const Entry &entry) {
		return entry.inlinePage || entry.fullPage;
	})) {
		const auto item = _session.get()->data().message(_ids.front());
		const auto group = item ? _session.get()->data().groups().find(item) : nullptr;
		if (!group) { return; }
		markdown = CopyTextAsMarkdown(HistoryGroupText(group));
	}
	if (markdown.isEmpty() && !_hasGenuine) { return; }
	const auto deliver = [&] { SetDesktopMarkdownClipboard(markdown); };
	auto published = false;
	if (_archiveScope) {
		const auto &scope = *_archiveScope;
		published = scope.publication->publish(scope.stamp, [&] {
			return scope.current() && entriesValid();
		}, deliver);
	} else if (!markdown.isEmpty()) {
		deliver();
		published = true;
	}
	if (published && contextValid()) {
		for (auto &event : _resolvedBodies) {
			QueueDesktopResolvedRichBody(_session.get(), std::move(event));
		}
		_resolvedBodies.clear();
	}

}

void CopyBatch::ordinary(size_t index) {
	auto &entry = _entries[index];
	const auto item = _session.get()->data().message(entry.id);
	const auto sameDisplay = entry.ordinary.rich == entry.original;
	const auto &body = entry.body;
	if (!_archiveScope || _selection || entry.display.translated()
		|| (!sameDisplay && body.document.isEmpty())) {
		entry.markdown = CopyTextAsMarkdown(entry.ordinary);
		next(index + 1);
		return;
	}
	const auto self = shared_from_this();
	ReadDesktopMarkdownSource(_session.get(), *_archiveScope, body,
		[self, index](DesktopMarkdownCopySource source) {
			if (!source.current || !self->entriesValid()) { self->cancel(); return; }
			auto &entry = self->_entries[index];
			self->_hasGenuine = self->_hasGenuine || source.text.has_value();
			entry.markdown = source.text ? *source.text : CopyTextAsMarkdown(entry.ordinary);
			self->next(index + 1);
		});
}

void CopyBatch::rendered(size_t index, const std::shared_ptr<const Iv::RichPage> &page) {
	if (!entriesValid() || !page || page->part) return;
	const auto session = _session.get();
	auto &entry = _entries[index];
	const auto current = session->data().message(entry.id);
	const auto expected = SelectCompleteArticlePage(current->richPage(),
		current->fullRichPage(), entry.display.translationPage, entry.display.translated());
	if (page != expected) {
		failed(u"The displayed article changed. Nothing was copied."_q);
		return;
	}
	if (!entry.display.translated() && page != entry.inlinePage
		&& (!entry.inlinePage || !AdmitDesktopRichCopyPage(*entry.inlinePage, *page,
			entry.richOpening, entry.richCompletion, entry.verifiedResponse))) {
		failed(u"The cached article does not match the opening message. Nothing was copied."_q);
		return;
	}
	const auto permalink = current->id.bare > 0 && current->history()->peer->isChannel()
		? session->api().exportDirectMessageLink(current, false, true)
		: QString();
	const auto output = RenderArticleMarkdown(*page, permalink);
	if (!output.error.isEmpty()) {
		failed(output.error);
		return;
	}
	entry.markdown = output.text;
	next(index + 1);
}

void BeginCopy(
		not_null<HistoryView::ListWidget*> list,
		MessageIdsList ids,
		bool selection,
		bool group) {
	static auto operations = std::map<HistoryView::ListWidget*, std::weak_ptr<CopyBatch>>();
	const auto key = list.get();
	const auto existing = operations.find(key);
	if (existing == operations.end()) {
		list->lifetime().add([=] { operations.erase(key); });
	} else if (const auto previous = existing->second.lock()) {
		previous->cancel();
	}
	const auto operation = std::make_shared<CopyBatch>(list, std::move(ids), selection, group);
	operations[key] = operation;
	operation->start();
}

} // namespace

void AddSelectionCopyAsMarkdownAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryView::ListWidget*> list,
		bool wholeMessages,
		bool overSelection) {
	if (!overSelection || list->hasCopyRestrictionForSelected()) {
		return;
	}
	if (list->getSelectedText().empty()) {
		const auto hasArticle = wholeMessages && ranges::any_of(list->getSelectedIds(), [&](FullMsgId id) {
			const auto item = list->session().data().message(id);
			return item && (item->richPage() || item->fullRichPage());
		});
		if (!hasArticle) return;
	}
	const auto weak = base::make_weak(list);
	if (!wholeMessages) {
		AddCopyAsMarkdownAction(menu, [=] {
			const auto strong = weak.get();
			return strong && !strong->session().frozen()
				&& !strong->hasCopyRestrictionForSelected()
				? strong->getSelectedText() : TextForMimeData();
		});
		return;
	}
	menu->addAction(tr::lng_purple_copy_as_markdown(tr::now), [=] {
		BeginMarkdownCopy();
		if (const auto strong = weak.get()) {
			const auto ids = strong->purpleSelectedIdsInOrder();
			const auto hasArticle = ranges::any_of(ids, [&](FullMsgId id) {
				const auto item = strong->session().data().message(id);
				return item && (item->richPage() || item->fullRichPage());
			});
			if (hasArticle) {
				BeginCopy(strong, ids, true, false);
			} else if (!strong->session().frozen()) {
				const auto markdown = CopyTextAsMarkdown(strong->getSelectedText());
				if (!markdown.isEmpty()) TextUtilities::SetClipboardText(TextForMimeData::Simple(markdown));
			}
		}
	}, &st::menuIconCopy);
}

void AddMarkdownDocumentCopyAction(not_null<Ui::PopupMenu*> menu,
		not_null<HistoryView::ListWidget*> list, HistoryItem *item,
		bool permitted, bool asGroup) {
	if (!permitted || !item || item->richPage() || !item->media()) { return; }
	const auto body = DesktopMarkdownBodyForItem(item);
	if (!body.document.isEmpty() && IsDesktopMarkdownFile({body.document, u"document"_q,
		{{u"name"_q, body.name}, {u"mime"_q, body.mime}}})) {
		AddMessageCopyAsMarkdownAction(menu, list, item->fullId(), asGroup);
	}
}

void AddMessageCopyAsMarkdownAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryView::ListWidget*> list,
		FullMsgId itemId,
		bool asGroup) {
	if (menu->property("purple_markdown_copy").toBool()) { return; }
	menu->setProperty("purple_markdown_copy", true);
	const auto weak = base::make_weak(list);
	menu->addAction(tr::lng_purple_copy_as_markdown(tr::now), [=] {
		BeginMarkdownCopy();
		const auto strong = weak.get();
		if (!strong) return;
		const auto item = strong->session().data().message(itemId);
		if (!item) return;
		auto ids = MessageIdsList{ itemId };
		auto grouped = false;
		if (asGroup) {
			if (const auto group = strong->session().data().groups().find(item)) {
				ids.clear();
				for (const auto part : group->items) ids.push_back(part->fullId());
				grouped = true;
			}
		}
		BeginCopy(strong, std::move(ids), false, grouped);
	}, &st::menuIconCopy);
}

} // namespace Purple
