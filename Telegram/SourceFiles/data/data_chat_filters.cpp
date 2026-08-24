/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_chat_filters.h"

#include "api/api_text_entities.h"
#include "history/history.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "data/data_folder.h"
#include "data/data_histories.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "history/history_unread_things.h"
#include "ui/ui_utility.h"
#include "ui/chat/more_chats_bar.h"
#include "main/main_session.h"
#include "main/main_app_config.h"
#include "purple/purple_gate.h"
#include "apiwrap.h"

namespace Data {
namespace {

constexpr auto kRefreshSuggestedTimeout = 7200 * crl::time(1000);
constexpr auto kLoadExceptionsAfter = 100;
constexpr auto kLoadExceptionsPerRequest = 100;

[[nodiscard]] crl::time RequestUpdatesEach(not_null<Session*> owner) {
	const auto appConfig = &owner->session().appConfig();
	return appConfig->get<int>(u"chatlist_update_period"_q, 3600)
		* crl::time(1000);
}

} // namespace

TextWithEntities ForceCustomEmojiStatic(TextWithEntities text) {
	for (auto &entity : text.entities) {
		if (entity.type() == EntityType::CustomEmoji) {
			entity = EntityInText(
				EntityType::CustomEmoji,
				entity.offset(),
				entity.length(),
				u"force-static:"_q + entity.data());
		}
	}
	return text;
}

ChatFilter::ChatFilter(
	FilterId id,
	ChatFilterTitle title,
	QString iconEmoji,
	std::optional<uint8> colorIndex,
	Flags flags,
	base::flat_set<not_null<History*>> always,
	std::vector<not_null<History*>> pinned,
	base::flat_set<not_null<History*>> never)
: _id(id)
, _title(std::move(title.text))
, _iconEmoji(std::move(iconEmoji))
, _colorIndex(colorIndex)
, _always(std::move(always))
, _pinned(std::move(pinned))
, _never(std::move(never))
, _flags(title.isStatic
	? (flags | Flag::StaticTitle)
	: (flags & ~Flag::StaticTitle)) {
}

ChatFilter ChatFilter::FromTL(
		const MTPDialogFilter &data,
		not_null<Session*> owner) {
	return data.match([&](const MTPDdialogFilter &data) {
		const auto flags = (data.is_contacts() ? Flag::Contacts : Flag(0))
			| (data.is_non_contacts() ? Flag::NonContacts : Flag(0))
			| (data.is_groups() ? Flag::Groups : Flag(0))
			| (data.is_broadcasts() ? Flag::Channels : Flag(0))
			| (data.is_bots() ? Flag::Bots : Flag(0))
			| (data.is_exclude_muted() ? Flag::NoMuted : Flag(0))
			| (data.is_exclude_read() ? Flag::NoRead : Flag(0))
			| (data.is_exclude_archived() ? Flag::NoArchived : Flag(0))
			| (data.is_title_noanimate() ? Flag::StaticTitle : Flag(0));
		auto &&to_histories = ranges::views::transform([&](
				const MTPInputPeer &input) {
			const auto peer = Data::PeerFromInputMTP(owner, input);
			return peer ? owner->history(peer).get() : nullptr;
		}) | ranges::views::filter([](History *history) {
			return history != nullptr;
		}) | ranges::views::transform([](History *history) {
			return not_null<History*>(history);
		});
		auto &&always = ranges::views::concat(
			data.vinclude_peers().v
		) | to_histories;
		auto pinned = ranges::views::all(
			data.vpinned_peers().v
		) | to_histories | ranges::to_vector;
		auto &&never = ranges::views::all(
			data.vexclude_peers().v
		) | to_histories;
		auto &&all = ranges::views::concat(always, pinned);
		auto list = base::flat_set<not_null<History*>>{
			all.begin(),
			all.end()
		};
		return ChatFilter(
			data.vid().v,
			{
				Api::ParseTextWithEntities(&owner->session(), data.vtitle()),
				data.is_title_noanimate(),
			},
			qs(data.vemoticon().value_or_empty()),
			data.vcolor()
				? std::make_optional(data.vcolor()->v)
				: std::nullopt,
			flags,
			std::move(list),
			std::move(pinned),
			{ never.begin(), never.end() });
	}, [](const MTPDdialogFilterDefault &) {
		return ChatFilter();
	}, [&](const MTPDdialogFilterChatlist &data) {
		auto &&to_histories = ranges::views::transform([&](
				const MTPInputPeer &data) {
			const auto peer = data.match([&](const MTPDinputPeerUser &data) {
				const auto user = owner->user(data.vuser_id().v);
				user->setAccessHash(data.vaccess_hash().v);
				return (PeerData*)user;
			}, [&](const MTPDinputPeerChat &data) {
				return (PeerData*)owner->chat(data.vchat_id().v);
			}, [&](const MTPDinputPeerChannel &data) {
				const auto channel = owner->channel(data.vchannel_id().v);
				channel->setAccessHash(data.vaccess_hash().v);
				return (PeerData*)channel;
			}, [&](const MTPDinputPeerSelf &data) {
				return (PeerData*)owner->session().user();
			}, [&](const auto &data) {
				return (PeerData*)nullptr;
			});
			return peer ? owner->history(peer).get() : nullptr;
		}) | ranges::views::filter([](History *history) {
			return history != nullptr;
		}) | ranges::views::transform([](History *history) {
			return not_null<History*>(history);
		});
		auto &&always = ranges::views::concat(
			data.vinclude_peers().v
		) | to_histories;
		auto pinned = ranges::views::all(
			data.vpinned_peers().v
		) | to_histories | ranges::to_vector;
		auto &&all = ranges::views::concat(always, pinned);
		auto list = base::flat_set<not_null<History*>>{
			all.begin(),
			all.end()
		};
		return ChatFilter(
			data.vid().v,
			{
				Api::ParseTextWithEntities(&owner->session(), data.vtitle()),
				data.is_title_noanimate(),
			},
			qs(data.vemoticon().value_or_empty()),
			data.vcolor()
				? std::make_optional(data.vcolor()->v)
				: std::nullopt,
			(Flag::Chatlist
				| (data.is_has_my_invites() ? Flag::HasMyLinks : Flag())
				| (data.is_title_noanimate() ? Flag::StaticTitle : Flag(0))),
			std::move(list),
			std::move(pinned),
			{});
	});
}

ChatFilter ChatFilter::withId(FilterId id) const {
	auto result = *this;
	result._id = id;
	return result;
}

ChatFilter ChatFilter::withTitle(ChatFilterTitle title) const {
	auto result = *this;
	result._title = std::move(title.text);
	if (title.isStatic) {
		result._flags |= Flag::StaticTitle;
	} else {
		result._flags &= ~Flag::StaticTitle;
	}
	return result;
}

ChatFilter ChatFilter::withColorIndex(std::optional<uint8> c) const {
	auto result = *this;
	result._colorIndex = c;
	return result;
}

ChatFilter ChatFilter::withChatlist(bool chatlist, bool hasMyLinks) const {
	auto result = *this;
	result._flags &= Flag::RulesMask;
	if (chatlist) {
		result._flags |= Flag::Chatlist;
		if (hasMyLinks) {
			result._flags |= Flag::HasMyLinks;
		} else {
			result._flags &= ~Flag::HasMyLinks;
		}
	}
	return result;
}

ChatFilter ChatFilter::withoutAlways(not_null<History*> history) const {
	auto result = *this;
	if (CanRemoveFromChatFilter(result, history)) {
		result._always.remove(history);
	}
	return result;
}

MTPDialogFilter ChatFilter::tl(FilterId replaceId) const {
	auto always = _always;
	auto pinned = QVector<MTPInputPeer>();
	pinned.reserve(_pinned.size());
	for (const auto &history : _pinned) {
		pinned.push_back(history->peer->input());
		always.remove(history);
	}
	auto include = QVector<MTPInputPeer>();
	include.reserve(always.size());
	for (const auto &history : always) {
		include.push_back(history->peer->input());
	}
	auto title = MTP_textWithEntities(
		MTP_string(_title.text),
		Api::EntitiesToMTP(
			nullptr,
			_title.entities,
			Api::ConvertOption::SkipLocal));
	if (_flags & Flag::Chatlist) {
		using TLFlag = MTPDdialogFilterChatlist::Flag;
		const auto flags = TLFlag::f_emoticon
			| (_colorIndex ? TLFlag::f_color : TLFlag(0))
			| (staticTitle() ? TLFlag::f_title_noanimate : TLFlag(0));
		return MTP_dialogFilterChatlist(
			MTP_flags(flags),
			MTP_int(replaceId ? replaceId : _id),
			std::move(title),
			MTP_string(_iconEmoji),
			MTP_int(_colorIndex.value_or(0)),
			MTP_vector<MTPInputPeer>(pinned),
			MTP_vector<MTPInputPeer>(include));
	}
	using TLFlag = MTPDdialogFilter::Flag;
	const auto flags = TLFlag::f_emoticon
		| (_colorIndex ? TLFlag::f_color : TLFlag(0))
		| (staticTitle() ? TLFlag::f_title_noanimate : TLFlag(0))
		| ((_flags & Flag::Contacts) ? TLFlag::f_contacts : TLFlag(0))
		| ((_flags & Flag::NonContacts) ? TLFlag::f_non_contacts : TLFlag(0))
		| ((_flags & Flag::Groups) ? TLFlag::f_groups : TLFlag(0))
		| ((_flags & Flag::Channels) ? TLFlag::f_broadcasts : TLFlag(0))
		| ((_flags & Flag::Bots) ? TLFlag::f_bots : TLFlag(0))
		| ((_flags & Flag::NoMuted) ? TLFlag::f_exclude_muted : TLFlag(0))
		| ((_flags & Flag::NoRead) ? TLFlag::f_exclude_read : TLFlag(0))
		| ((_flags & Flag::NoArchived)
			? TLFlag::f_exclude_archived
			: TLFlag(0));
	auto never = QVector<MTPInputPeer>();
	never.reserve(_never.size());
	for (const auto &history : _never) {
		never.push_back(history->peer->input());
	}
	return MTP_dialogFilter(
		MTP_flags(flags),
		MTP_int(replaceId ? replaceId : _id),
		std::move(title),
		MTP_string(_iconEmoji),
		MTP_int(_colorIndex.value_or(0)),
		MTP_vector<MTPInputPeer>(pinned),
		MTP_vector<MTPInputPeer>(include),
		MTP_vector<MTPInputPeer>(never));
}

FilterId ChatFilter::id() const {
	return _id;
}

const TextWithEntities &ChatFilter::titleText() const {
	return _title;
}

ChatFilterTitle ChatFilter::title() const {
	return { _title, !!(_flags & Flag::StaticTitle) };
}

QString ChatFilter::iconEmoji() const {
	return _iconEmoji;
}

std::optional<uint8> ChatFilter::colorIndex() const {
	return _colorIndex;
}

ChatFilter::Flags ChatFilter::flags() const {
	return _flags;
}

bool ChatFilter::staticTitle() const {
	return _flags & Flag::StaticTitle;
}

bool ChatFilter::chatlist() const {
	return _flags & Flag::Chatlist;
}

bool ChatFilter::hasMyLinks() const {
	return _flags & Flag::HasMyLinks;
}

const base::flat_set<not_null<History*>> &ChatFilter::always() const {
	return _always;
}

const std::vector<not_null<History*>> &ChatFilter::pinned() const {
	return _pinned;
}

const base::flat_set<not_null<History*>> &ChatFilter::never() const {
	return _never;
}

bool ChatFilter::contains(
		not_null<History*> history,
		bool ignoreFakeUnread,
		bool ignorePresetMute) const {
	const auto flag = [&] {
		const auto peer = history->peer;
		if (const auto user = peer->asUser()) {
			return user->isBot()
				? Flag::Bots
				: user->isContact()
				? Flag::Contacts
				: Flag::NonContacts;
		} else if (peer->isChat()) {
			return Flag::Groups;
		} else if (const auto channel = peer->asChannel()) {
			if (channel->isBroadcast()) {
				return Flag::Channels;
			} else {
				return Flag::Groups;
			}
		} else {
			Unexpected("Peer type in ChatFilter::contains.");
		}
	}();
	if (_never.contains(history)) {
		return false;
	}
	const auto channel = history->peer->asChannel();
	if (channel && channel->isCommunity()) {
		// A community never matches a filter by chat type (it is neither a
		// group nor a channel); it can only be included explicitly by id.
		return _always.contains(history);
	}
	const auto state = (_flags & (Flag::NoMuted | Flag::NoRead))
		? history->chatListBadgesState()
		: Dialogs::BadgesState();
	const auto muted = ignorePresetMute
		? history->owner().notifySettings().purpleMutedWithoutPreset(
			history->peer)
		: history->muted();
	return false
		|| ((_flags & flag)
			&& (!(_flags & Flag::NoMuted)
				|| !muted
				|| (state.mention
					&& history->folderKnown()
					&& !history->folder()))
			&& (!(_flags & Flag::NoRead)
				|| state.unread
				|| state.mention
				|| (!ignoreFakeUnread && history->fakeUnreadWhileOpened()))
			&& (!(_flags & Flag::NoArchived)
				|| (history->folderKnown() && !history->folder())))
		|| _always.contains(history);
}

ChatFilters::ChatFilters(not_null<Session*> owner)
: _owner(owner)
, _moreChatsTimer([=] { checkLoadMoreChatsLists(); }) {
	_list.emplace_back();
	crl::on_main(&owner->session(), [=] { load(); });

	// Purple: the shown view depends on both halves - which folders exist and
	// which the preset names - so it is rebuilt when either moves. This runs
	// before Session's own peer walk, because ChatFilters is constructed in
	// Session's member list and therefore subscribes first.
	rpl::merge(
		changed(),
		Purple::ActiveChanges()
	) | rpl::on_next([=] {
		purpleRefreshShown();
	}, _purpleLifetime);

	// Once up front as well: a preset that was already active when the app
	// started never fires a change, and defaultId() has to know about the view
	// before the first window asks it what to open on.
	purpleRefreshShown();
}

ChatFilter ChatFilters::purpleViewFilter(int index) const {
	// Named after the preset, or after the view, because that is the honest
	// label: the tab is what this preset shows, and the user picked the name.
	// Nothing else about it is a folder - no rules, no members - which is why
	// every icon lookup falls through to the All chats one and every edit path
	// refuses it.
	const auto title = index
		? Purple::ExtraViews()[index - 1].name
		: Purple::ViewName();
	return ChatFilter(
		PurpleViewFilterId(index),
		{ TextWithEntities{ title } },
		QString(),
		std::nullopt,
		ChatFilter::Flags(),
		{},
		{},
		{});
}

int ChatFilters::purpleViewCount() const {
	return _purpleViewCount;
}

void ChatFilters::purpleRefreshShown() {
	if (!Purple::Filtering()) {
		_purpleShown.clear();
		_purpleViewCount = 0;
		return;
	}
	auto result = std::vector<ChatFilter>();

	// The preset's main view stands where All chats stands, and All chats itself
	// is dropped. That is what makes the whole design work: the main list keeps
	// every chat, so nothing has to be torn out of it, and the one place the
	// user would notice the difference is the one place a preset is meant to
	// change. Left on its own the view takes has() below the threshold and the
	// folder UI disappears by itself, which is what "folders = []" asks for.
	//
	// Then the extra views, in file order, before any folder: they are the
	// preset's own tabs, and a preset's tabs belong next to each other rather
	// than scattered through the account's folders.
	const auto extra = int(Purple::ExtraViews().size());
	_purpleViewCount = std::min(extra + 1, kPurpleViewLimit);
	if (extra + 1 > kPurpleViewLimit) {
		// Silently drawing the first fifteen would read as "the rest are empty",
		// which is the one thing it does not mean.
		LOG(("Purple: preset '%1' defines %2 extra views; showing %3."
			).arg(Purple::ActiveResolved().preset
			).arg(extra
			).arg(kPurpleViewLimit - 1));
	}
	for (auto i = 0; i != _purpleViewCount; ++i) {
		result.push_back(purpleViewFilter(i));
	}

	const auto &shown = Purple::ShownFolders();
	auto missing = QStringList();
	for (const auto &wanted : shown) {
		if (Purple::IsAllFolders(wanted)) {
			// Every folder the selection does not name elsewhere, in the
			// account's own order, at this position. The parser cannot expand
			// it - it has never heard of a Telegram folder - so it arrives here
			// as a marker and is expanded in place, which is what keeps its
			// position in the strip meaningful.
			for (const auto &filter : _list) {
				if (!filter.id()) {
					continue;
				}
				const auto named = ranges::any_of(shown, [&](
						const Purple::PresetFolder &entry) {
					return !Purple::IsAllFolders(entry)
						&& !filter.title().text.text.compare(
							entry.name,
							Qt::CaseInsensitive);
				});
				if (!named
					&& !ranges::contains(result, filter.id(), &ChatFilter::id)) {
					result.push_back(filter);
				}
			}
			continue;
		} else if (!wanted.show.value_or(true)) {
			// Named, but deliberately not on the strip. Worth being able to say
			// separately from leaving it out: a folder can be silenced or fed
			// into the main view without its tab being there.
			continue;
		}
		const auto i = ranges::find_if(_list, [&](const ChatFilter &filter) {
			return filter.id()
				&& !filter.title().text.text.compare(
					wanted.name,
					Qt::CaseInsensitive);
		});
		if (i == end(_list)) {
			missing.push_back(wanted.name);
		} else if (!ranges::contains(result, i->id(), &ChatFilter::id)) {
			result.push_back(*i);
		}
	}
	if (!missing.isEmpty()) {
		// Same reasoning as the hidden-chat count: a folder name that matches
		// nothing looks identical to a folder the preset meant to hide.
		LOG(("Purple: preset names folders that do not exist: %1."
			).arg(missing.join(u", "_q)));
	}
	LOG(("Purple: folder strip showing %1 of %2, including %3 preset view(s)."
		).arg(result.size()).arg(_list.size()).arg(_purpleViewCount));
	_purpleShown = std::move(result);
}

const std::vector<ChatFilter> &ChatFilters::purpleShownList() const {
	return Purple::Filtering() ? _purpleShown : _list;
}

ChatFilters::~ChatFilters() = default;

not_null<Dialogs::MainList*> ChatFilters::chatsList(FilterId filterId) {
	auto &pointer = _chatsLists[filterId];
	if (!pointer) {
		auto limit = rpl::single(rpl::empty_value()) | rpl::then(
			_owner->session().appConfig().refreshed()
		) | rpl::map([=] {
			return _owner->pinnedChatsLimit(filterId);
		});
		pointer = std::make_unique<Dialogs::MainList>(
			&_owner->session(),
			filterId,
			// Purple: the main view's pinned list is a copy of the main list's,
			// so it has to be allowed to hold exactly as much - the folder
			// limit is a different number for a different thing. An extra view
			// keeps its own pins in a local file nothing bounds, and uses the
			// same allowance so that a tab cannot become all pins.
			IsPurpleView(filterId)
				? _owner->maxPinnedChatsLimitValue((Data::Folder*)nullptr)
				: _owner->maxPinnedChatsLimitValue(filterId));
	}
	return pointer.get();
}

Dialogs::MainList *ChatFilters::chatsListLoaded(FilterId filterId) {
	const auto i = _chatsLists.find(filterId);
	return (i != end(_chatsLists)) ? i->second.get() : nullptr;
}

void ChatFilters::clear() {
	_chatsLists.clear();
	_list.clear();
}

void ChatFilters::setPreloaded(
		const QVector<MTPDialogFilter> &result,
		bool tagsEnabled) {
	_loadRequestId = -1;
	_tagsEnabled = tagsEnabled;
	received(result);
	crl::on_main(&_owner->session(), [=] {
		if (_loadRequestId == -1) {
			_loadRequestId = 0;
		}
	});
}

void ChatFilters::load() {
	load(false);
}

void ChatFilters::reload() {
	_reloading = true;
	load();
}

void ChatFilters::load(bool force) {
	if (_loadRequestId && !force) {
		return;
	}
	auto &api = _owner->session().api();
	api.request(_loadRequestId).cancel();
	_loadRequestId = api.request(MTPmessages_GetDialogFilters(
	)).done([=](const MTPmessages_DialogFilters &result) {
		_tagsEnabled = result.data().is_tags_enabled();
		received(result.data().vfilters().v);
		_loadRequestId = 0;
	}).fail([=] {
		_loadRequestId = 0;
		if (_reloading) {
			_reloading = false;
			_listChanged.fire({});
		}
	}).send();
}

bool ChatFilters::tagsEnabled() const {
	return _tagsEnabled.current();
}

rpl::producer<bool> ChatFilters::tagsEnabledValue() const {
	return _tagsEnabled.value();
}

rpl::producer<bool> ChatFilters::tagsEnabledChanges() const {
	return _tagsEnabled.changes();
}

void ChatFilters::requestToggleTags(bool value, Fn<void()> fail) {
	if (_toggleTagsRequestId) {
		return;
	}
	_toggleTagsRequestId = _owner->session().api().request(
		MTPmessages_ToggleDialogFilterTags(MTP_bool(value))
	).done([=](const MTPBool &result) {
		_tagsEnabled = value;
		_toggleTagsRequestId = 0;
	}).fail([=](const MTP::Error &error) {
		const auto message = error.type();
		_toggleTagsRequestId = 0;
		LOG(("API Error: Toggle Tags - %1").arg(message));
		fail();
	}).send();
}

void ChatFilters::received(const QVector<MTPDialogFilter> &list) {
	auto position = 0;
	auto changed = false;
	for (const auto &filter : list) {
		auto parsed = ChatFilter::FromTL(filter, _owner);
		const auto b = begin(_list) + position;
		const auto e = end(_list);
		const auto i = ranges::find(b, e, parsed.id(), &ChatFilter::id);
		if (i == e) {
			applyInsert(std::move(parsed), position);
			changed = true;
		} else if (i == b) {
			if (applyChange(*b, std::move(parsed))) {
				changed = true;
			}
		} else {
			std::swap(*i, *b);
			applyChange(*b, std::move(parsed));
			changed = true;
		}
		++position;
	}
	while (position < _list.size()) {
		applyRemove(position);
		changed = true;
	}
	if (!ranges::contains(begin(_list), end(_list), 0, &ChatFilter::id)) {
		_list.insert(begin(_list), ChatFilter());
	}
	if (changed || !_loaded || _reloading) {
		_loaded = true;
		_reloading = false;
		_listChanged.fire({});
	}
}

void ChatFilters::apply(const MTPUpdate &update) {
	update.match([&](const MTPDupdateDialogFilter &data) {
		if (const auto filter = data.vfilter()) {
			set(ChatFilter::FromTL(*filter, _owner));
		} else {
			remove(data.vid().v);
		}
	}, [&](const MTPDupdateDialogFilters &data) {
		load(true);
	}, [&](const MTPDupdateDialogFilterOrder &data) {
		if (applyOrder(data.vorder().v)) {
			_listChanged.fire({});
		} else {
			load(true);
		}
	}, [](auto&&) {
		Unexpected("Update in ChatFilters::apply.");
	});
}

ChatFilterLink ChatFilters::add(
		FilterId id,
		const MTPExportedChatlistInvite &update) {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	if (i == end(_list) || !i->chatlist()) {
		LOG(("Api Error: "
			"Attempt to add chatlist link to a non-chatlist filter: %1"
			).arg(id));
		return {};
	}
	auto &links = _chatlistLinks[id];
	const auto &data = update.data();
	const auto url = qs(data.vurl());
	const auto title = qs(data.vtitle());
	auto chats = data.vpeers().v | ranges::views::transform([&](
			const MTPPeer &peer) {
		return _owner->history(peerFromMTP(peer));
	}) | ranges::to_vector;
	const auto j = ranges::find(links, url, &ChatFilterLink::url);
	if (j != end(links)) {
		if (j->title != title || j->chats != chats) {
			j->title = title;
			j->chats = std::move(chats);
			_chatlistLinksUpdated.fire_copy(id);
		}
		return *j;
	}
	links.push_back({
		.id = id,
		.url = url,
		.title = title,
		.chats = std::move(chats),
	});
	_chatlistLinksUpdated.fire_copy(id);
	return links.back();
}

void ChatFilters::edit(
		FilterId id,
		const QString &url,
		const QString &title) {
	auto &links = _chatlistLinks[id];
	const auto i = ranges::find(links, url, &ChatFilterLink::url);
	if (i != end(links)) {
		i->title = title;
		_chatlistLinksUpdated.fire_copy(id);

		_owner->session().api().request(MTPchatlists_EditExportedInvite(
			MTP_flags(MTPchatlists_EditExportedInvite::Flag::f_title),
			MTP_inputChatlistDialogFilter(MTP_int(id)),
			MTP_string(url),
			MTP_string(title),
			MTPVector<MTPInputPeer>() // peers
		)).done([=](const MTPExportedChatlistInvite &result) {
			//const auto &data = result.data();
			//const auto link = _owner->chatsFilters().add(id, result);
			//done(link);
		}).fail([=](const MTP::Error &error) {
			//done({ .id = id });
		}).send();
	}
}

void ChatFilters::destroy(FilterId id, const QString &url) {
	auto &links = _chatlistLinks[id];
	const auto i = ranges::find(links, url, &ChatFilterLink::url);
	if (i != end(links)) {
		links.erase(i);
		_chatlistLinksUpdated.fire_copy(id);

		const auto api = &_owner->session().api();
		api->request(_linksRequestId).cancel();
		_linksRequestId = api->request(MTPchatlists_DeleteExportedInvite(
			MTP_inputChatlistDialogFilter(MTP_int(id)),
			MTP_string(url)
		)).send();
	}
}

rpl::producer<std::vector<ChatFilterLink>> ChatFilters::chatlistLinks(
		FilterId id) const {
	return _chatlistLinksUpdated.events_starting_with_copy(
		id
	) | rpl::filter(rpl::mappers::_1 == id) | rpl::map([=] {
		const auto i = _chatlistLinks.find(id);
		return (i != end(_chatlistLinks))
			? i->second
			: std::vector<ChatFilterLink>();
	});
}

void ChatFilters::reloadChatlistLinks(FilterId id) {
	const auto api = &_owner->session().api();
	api->request(_linksRequestId).cancel();
	_linksRequestId = api->request(MTPchatlists_GetExportedInvites(
		MTP_inputChatlistDialogFilter(MTP_int(id))
	)).done([=](const MTPchatlists_ExportedInvites &result) {
		const auto &data = result.data();
		_owner->processUsers(data.vusers());
		_owner->processChats(data.vchats());
		_chatlistLinks[id].clear();
		for (const auto &link : data.vinvites().v) {
			add(id, link);
		}
		_chatlistLinksUpdated.fire_copy(id);
	}).send();
}

void ChatFilters::set(ChatFilter filter) {
	if (!filter.id()) {
		return;
	}
	const auto i = ranges::find(_list, filter.id(), &ChatFilter::id);
	if (i == end(_list)) {
		applyInsert(std::move(filter), _list.size());
		_listChanged.fire({});
	} else if (applyChange(*i, std::move(filter))) {
		_listChanged.fire({});
	}
}

void ChatFilters::applyInsert(ChatFilter filter, int position) {
	Expects(position >= 0 && position <= _list.size());

	_list.insert(
		begin(_list) + position,
		ChatFilter(filter.id(), {}, {}, {}, {}, {}, {}, {}));
	applyChange(*(begin(_list) + position), std::move(filter));
}

void ChatFilters::remove(FilterId id) {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	if (i == end(_list)) {
		return;
	}
	applyRemove(i - begin(_list));
	_listChanged.fire({});
}

void ChatFilters::moveAllToFront() {
	const auto i = ranges::find(_list, FilterId(), &ChatFilter::id);
	if (!_list.empty() && i == begin(_list)) {
		return;
	} else if (i != end(_list)) {
		_list.erase(i);
	}
	_list.insert(begin(_list), ChatFilter());
}

void ChatFilters::applyRemove(int position) {
	Expects(position >= 0 && position < _list.size());

	// Remove the filter from the list before applyChange() tears down its
	// chats. Unpinning a chat re-caches its siblings' pinned indices, which
	// runs Session::refreshChatListEntry(); while the filter is still listed
	// but emptied that re-enters PinnedList::setPinned() on the same pinned
	// list mid-iteration and crashes (see also PinnedList::setPinned).
	const auto i = begin(_list) + position;
	auto filter = std::move(*i);
	_list.erase(i);
	applyChange(filter, ChatFilter(filter.id(), {}, {}, {}, {}, {}, {}, {}));
}

bool ChatFilters::applyChange(ChatFilter &filter, ChatFilter &&updated) {
	Expects(filter.id() == updated.id());

	using Flag = ChatFilter::Flag;

	const auto id = filter.id();
	const auto exceptionsChanged = filter.always() != updated.always();
	const auto rulesMask = Flag() | Flag::RulesMask;
	const auto rulesChanged = exceptionsChanged
		|| ((filter.flags() & rulesMask) != (updated.flags() & rulesMask))
		|| (filter.never() != updated.never());
	const auto pinnedChanged = (filter.pinned() != updated.pinned());
	const auto chatlistChanged = (filter.chatlist() != updated.chatlist())
		|| (filter.hasMyLinks() != updated.hasMyLinks());
	const auto listUpdated = rulesChanged
		|| pinnedChanged
		|| (filter.titleText() != updated.titleText())
		|| (filter.staticTitle() != updated.staticTitle())
		|| (filter.iconEmoji() != updated.iconEmoji());
	const auto colorChanged = filter.colorIndex() != updated.colorIndex();
	const auto colorExistenceChanged = (!filter.colorIndex())
		!= (!updated.colorIndex());
	if (!listUpdated && !chatlistChanged && !colorChanged) {
		return false;
	}
	const auto wasFilter = std::move(filter);
	filter = std::move(updated);
	auto entryToRefreshHeight = (Dialogs::Entry*)(nullptr);
	if (rulesChanged) {
		const auto filterList = _owner->chatsFilters().chatsList(id);
		const auto areTagsEnabled = tagsEnabled();
		const auto tagsExistence = [&](not_null<Dialogs::Row*> row) {
			return (!areTagsEnabled || entryToRefreshHeight)
				? false
				: row->entry()->hasChatsFilterTags(0);
		};
		const auto feedHistory = [&](not_null<History*> history) {
			const auto now = filter.contains(history);
			const auto was = wasFilter.contains(history);
			if (now != was) {
				if (now) {
					history->addToChatList(id, filterList);
				} else {
					history->removeFromChatList(id, filterList);
				}
			}
		};
		const auto feedList = [&](not_null<const Dialogs::MainList*> list) {
			for (const auto &entry : *list->indexed()) {
				if (const auto history = entry->history()) {
					const auto wasTags = tagsExistence(entry);
					feedHistory(history);
					if (wasTags != tagsExistence(entry)) {
						entryToRefreshHeight = entry->entry();
					}
				}
			}
		};
		feedList(_owner->chatsList());
		if (const auto folder = _owner->folderLoaded(Data::Folder::kId)) {
			feedList(folder->chatsList());
		}
		if (exceptionsChanged && !filter.always().empty()) {
			_exceptionsToLoad.push_back(id);
			Ui::PostponeCall(&_owner->session(), [=] {
				_owner->session().api().requestMoreDialogsIfNeeded();
			});
		}
	}
	if (pinnedChanged) {
		const auto filterList = _owner->chatsFilters().chatsList(id);
		filterList->pinned()->applyList(filter.pinned());
	}
	if (chatlistChanged) {
		_isChatlistChanged.fire_copy(id);
	}
	if (colorChanged) {
		_tagColorChanged.fire_copy(TagColorChanged{
			.filterId = id,
			.colorExistenceChanged = colorExistenceChanged,
		});
	}
	if (entryToRefreshHeight) {
		// Trigger a full refresh of height for the main list.
		entryToRefreshHeight->updateChatListEntryHeight();
	}
	return listUpdated;
}

bool ChatFilters::applyOrder(const QVector<MTPint> &order) {
	if (order.size() != _list.size()) {
		return false;
	} else if (_list.empty()) {
		return true;
	}
	auto indices = ranges::views::all(
		_list
	) | ranges::views::transform(
		&ChatFilter::id
	) | ranges::to_vector;
	auto b = indices.begin(), e = indices.end();
	for (const auto &id : order) {
		const auto i = ranges::find(b, e, id.v);
		if (i == e) {
			return false;
		} else if (i != b) {
			std::swap(*i, *b);
		}
		++b;
	}
	auto changed = false;
	auto begin = _list.begin(), end = _list.end();
	for (const auto &id : order) {
		const auto i = ranges::find(begin, end, id.v, &ChatFilter::id);
		Assert(i != end);
		if (i != begin) {
			changed = true;
			std::swap(*i, *begin);
		}
		++begin;
	}
	if (changed) {
		_listChanged.fire({});
	}
	return true;
}

const ChatFilter &ChatFilters::applyUpdatedPinned(
		FilterId id,
		const std::vector<Dialogs::Key> &dialogs) {
	const auto i = ranges::find(_list, id, &ChatFilter::id);
	Assert(i != end(_list));

	const auto limit = _owner->pinnedChatsLimit(id);
	auto always = i->always();
	auto pinned = std::vector<not_null<History*>>();
	pinned.reserve(dialogs.size());
	for (const auto &row : dialogs) {
		if (const auto history = row.history()) {
			if (always.contains(history)) {
				pinned.push_back(history);
			} else if (always.size() < limit) {
				always.insert(history);
				pinned.push_back(history);
			}
		}
	}
	set(ChatFilter(
		id,
		i->title(),
		i->iconEmoji(),
		i->colorIndex(),
		i->flags(),
		std::move(always),
		std::move(pinned),
		i->never()));
	return *i;
}

void ChatFilters::saveOrder(
		const std::vector<FilterId> &order,
		mtpRequestId after) {
	// Purple: this replaces the whole server-side order with exactly what it
	// is handed. While a preset restricts the folder strip the UI only knows
	// about a subset, so saving would drop every folder the preset is hiding -
	// from the account, not just from the view. Refuse rather than reorder
	// blind; this is the one choke point, so a display surface that was missed
	// still cannot do damage here.
	if (Purple::FoldersRestricted()) {
		LOG(("Purple: not saving folder order while a preset restricts "
			"folders. Switch to the normal preset to reorder them."));
		return;
	}
	if (after) {
		_saveOrderAfterId = after;
	}
	const auto api = &_owner->session().api();
	api->request(_saveOrderRequestId).cancel();

	auto ids = QVector<MTPint>();
	ids.reserve(order.size());
	for (const auto id : order) {
		ids.push_back(MTP_int(id));
	}
	const auto wrapped = MTP_vector<MTPint>(ids);

	apply(MTP_updateDialogFilterOrder(wrapped));
	_saveOrderRequestId = api->request(MTPmessages_UpdateDialogFiltersOrder(
		wrapped
	)).afterRequest(_saveOrderAfterId).send();
}

bool ChatFilters::archiveNeeded() const {
	for (const auto &filter : _list) {
		if (!(filter.flags() & ChatFilter::Flag::NoArchived)) {
			return true;
		}
	}
	return false;
}

const std::vector<ChatFilter> &ChatFilters::list() const {
	return _list;
}

FilterId ChatFilters::defaultId() const {
	return lookupId(0);
}

FilterId ChatFilters::lookupId(int index) const {
	// Purple: bounded against whichever list the answer comes from. A preset's
	// extra views make the shown list longer than the real one, so asserting on
	// _list alone would fire on a tab that is genuinely on screen.
	Expects(index >= 0
		&& index < int(Purple::Filtering() && !_purpleShown.empty()
			? _purpleShown.size()
			: _list.size()));

	// Purple: everyone who asks this means "the Nth tab I can see" - the
	// window that is opening, the folder shortcuts, closing a folder, Escape
	// going home. Under a preset that is the shown list, which carries the
	// preset's view where All chats was; answering 0 would send all of them to
	// the complete chat list the preset exists to replace.
	//
	// Clamped because a preset naming folders can make the shown list shorter
	// than the real one, and the callers bound their index against the real
	// one.
	if (Purple::Filtering() && !_purpleShown.empty()) {
		const auto last = int(_purpleShown.size()) - 1;
		return _purpleShown[std::min(index, last)].id();
	}
	if (_owner->session().user()->isPremium() || !_list.front().id()) {
		return _list[index].id();
	}
	const auto i = ranges::find(_list, FilterId(0), &ChatFilter::id);
	return !index
		? FilterId()
		: (index <= int(i - begin(_list)))
		? _list[index - 1].id()
		: _list[index].id();
}

bool ChatFilters::loaded() const {
	return _loaded;
}

bool ChatFilters::has() const {
	return _list.size() > 1;
}

rpl::producer<> ChatFilters::changed() const {
	return _listChanged.events();
}

rpl::producer<FilterId> ChatFilters::isChatlistChanged() const {
	return _isChatlistChanged.events();
}

rpl::producer<TagColorChanged> ChatFilters::tagColorChanged() const {
	return _tagColorChanged.events();
}

bool ChatFilters::loadNextExceptions(bool chatsListLoaded) {
	if (_exceptionsLoadRequestId) {
		return true;
	} else if (!chatsListLoaded
		&& (_owner->chatsList()->fullSize().current()
			< kLoadExceptionsAfter)) {
		return false;
	}
	auto inputs = QVector<MTPInputDialogPeer>();
	const auto collectExceptions = [&](FilterId id) {
		auto result = QVector<MTPInputDialogPeer>();
		const auto i = ranges::find(_list, id, &ChatFilter::id);
		if (i != end(_list)) {
			result.reserve(i->always().size());
			for (const auto &history : i->always()) {
				if (!history->folderKnown()) {
					inputs.push_back(
						MTP_inputDialogPeer(history->peer->input()));
				}
			}
		}
		return result;
	};
	while (!_exceptionsToLoad.empty()) {
		const auto id = _exceptionsToLoad.front();
		const auto exceptions = collectExceptions(id);
		if (inputs.size() + exceptions.size() > kLoadExceptionsPerRequest) {
			Assert(!inputs.isEmpty());
			break;
		}
		_exceptionsToLoad.pop_front();
		inputs.append(exceptions);
	}
	if (inputs.isEmpty()) {
		return false;
	}
	const auto api = &_owner->session().api();
	_exceptionsLoadRequestId = api->request(MTPmessages_GetPeerDialogs(
		MTP_vector(inputs)
	)).done([=](const MTPmessages_PeerDialogs &result) {
		_exceptionsLoadRequestId = 0;
		_owner->session().data().histories().applyPeerDialogs(result);
		_owner->session().api().requestMoreDialogsIfNeeded();
	}).fail([=] {
		_exceptionsLoadRequestId = 0;
		_owner->session().api().requestMoreDialogsIfNeeded();
	}).send();
	return true;
}

void ChatFilters::refreshHistory(not_null<History*> history) {
	if (history->inChatList() && !list().empty()) {
		_owner->refreshChatListEntry(history);
	}
}

void ChatFilters::requestSuggested() {
	if (_suggestedRequestId) {
		return;
	}
	if (_suggestedLastReceived > 0
		&& crl::now() - _suggestedLastReceived < kRefreshSuggestedTimeout) {
		return;
	}
	const auto api = &_owner->session().api();
	_suggestedRequestId = api->request(MTPmessages_GetSuggestedDialogFilters(
	)).done([=](const MTPVector<MTPDialogFilterSuggested> &data) {
		_suggestedRequestId = 0;
		_suggestedLastReceived = crl::now();

		_suggested = ranges::views::all(
			data.v
		) | ranges::views::transform([&](const MTPDialogFilterSuggested &f) {
			return f.match([&](const MTPDdialogFilterSuggested &data) {
				return SuggestedFilter{
					Data::ChatFilter::FromTL(data.vfilter(), _owner),
					qs(data.vdescription())
				};
			});
		}) | ranges::to_vector;

		_suggestedUpdated.fire({});
	}).fail([=] {
		_suggestedRequestId = 0;
		_suggestedLastReceived = crl::now() + kRefreshSuggestedTimeout / 2;

		_suggestedUpdated.fire({});
	}).send();
}

bool ChatFilters::suggestedLoaded() const {
	return (_suggestedLastReceived > 0);
}

const std::vector<SuggestedFilter> &ChatFilters::suggestedFilters() const {
	return _suggested;
}

rpl::producer<> ChatFilters::suggestedUpdated() const {
	return _suggestedUpdated.events();
}

rpl::producer<Ui::MoreChatsBarContent> ChatFilters::moreChatsContent(
		FilterId id) {
	if (!id) {
		return rpl::single(Ui::MoreChatsBarContent{ .count = 0 });
	}
	return [=](auto consumer) {
		auto result = rpl::lifetime();

		auto &entry = _moreChatsData[id];
		auto watching = entry.watching.lock();
		if (!watching) {
			watching = std::make_shared<bool>(true);
			entry.watching = watching;
		}
		result.add([watching] {});

		_moreChatsUpdated.events_starting_with_copy(
			id
		) | rpl::on_next([=] {
			consumer.put_next(Ui::MoreChatsBarContent{
				.count = int(moreChats(id).size()),
			});
		}, result);
		loadMoreChatsList(id);

		return result;
	};
}

const std::vector<not_null<PeerData*>> &ChatFilters::moreChats(
		FilterId id) const {
	static const auto kEmpty = std::vector<not_null<PeerData*>>();
	if (!id) {
		return kEmpty;
	}
	const auto i = _moreChatsData.find(id);
	return (i != end(_moreChatsData)) ? i->second.missing : kEmpty;
}

void ChatFilters::moreChatsHide(FilterId id, bool localOnly) {
	if (!localOnly) {
		const auto api = &_owner->session().api();
		api->request(MTPchatlists_HideChatlistUpdates(
			MTP_inputChatlistDialogFilter(MTP_int(id))
		)).send();
	}

	const auto i = _moreChatsData.find(id);
	if (i != end(_moreChatsData)) {
		if (const auto requestId = base::take(i->second.requestId)) {
			_owner->session().api().request(requestId).cancel();
		}
		i->second.missing = {};
		i->second.lastUpdate = crl::now();
		_moreChatsUpdated.fire_copy(id);
	}
}

void ChatFilters::loadMoreChatsList(FilterId id) {
	Expects(id != 0);

	const auto i = ranges::find(_list, id, &ChatFilter::id);
	if (i == end(_list) || !i->chatlist()) {
		return;
	}

	auto &entry = _moreChatsData[id];
	const auto now = crl::now();
	if (!entry.watching.lock() || entry.requestId) {
		return;
	}
	const auto last = entry.lastUpdate;
	const auto next = last ? (last + RequestUpdatesEach(_owner)) : 0;
	if (next > now) {
		if (!_moreChatsTimer.isActive()) {
			_moreChatsTimer.callOnce(next - now);
		}
		return;
	}
	auto &api = _owner->session().api();
	entry.requestId = api.request(MTPchatlists_GetChatlistUpdates(
		MTP_inputChatlistDialogFilter(MTP_int(id))
	)).done([=](const MTPchatlists_ChatlistUpdates &result) {
		const auto &data = result.data();
		_owner->processUsers(data.vusers());
		_owner->processChats(data.vchats());
		auto list = ranges::views::all(
			data.vmissing_peers().v
		) | ranges::views::transform([&](const MTPPeer &peer) {
			return _owner->peer(peerFromMTP(peer));
		}) | ranges::to_vector;

		auto &entry = _moreChatsData[id];
		entry.requestId = 0;
		entry.lastUpdate = crl::now();
		if (!_moreChatsTimer.isActive()) {
			_moreChatsTimer.callOnce(RequestUpdatesEach(_owner));
		}
		if (entry.missing != list) {
			entry.missing = std::move(list);
			_moreChatsUpdated.fire_copy(id);
		}
	}).fail([=] {
		auto &entry = _moreChatsData[id];
		entry.requestId = 0;
		entry.lastUpdate = crl::now();
	}).send();
}

void ChatFilters::checkLoadMoreChatsLists() {
	for (const auto &[id, entry] : _moreChatsData) {
		loadMoreChatsList(id);
	}
}

bool CanRemoveFromChatFilter(
		const ChatFilter &filter,
		not_null<History*> history) {
	using Flag = ChatFilter::Flag;
	const auto flagsWithoutNoReadNoArchivedNoMuted = filter.flags()
		& ~(Flag::NoRead | Flag::NoArchived | Flag::NoMuted);
	return (filter.always().size() > 1 || flagsWithoutNoReadNoArchivedNoMuted)
		&& filter.contains(history);
}

} // namespace Data
