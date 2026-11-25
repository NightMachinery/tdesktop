/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_translation_policy.h"
#include "purple/purple_notices.h"

#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_translation_exclusions.h"
#include "rpl/combine.h"
#include "rpl/event_stream.h"
#include "settings/settings_common.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <algorithm>

#include <QtCore/QString>

namespace Purple {
namespace {

constexpr auto kEnoughForTranslation = 6;
constexpr auto kExcludedChatsPref = "purple-translate-excluded-chats";

rpl::event_stream<Main::Session*> ExcludedChatsChanges;

[[nodiscard]] not_null<PeerData*> CanonicalPeer(not_null<PeerData*> peer) {
	if (const auto migrated = peer->migrateTo()) {
		return migrated;
	}
	return peer;
}

[[nodiscard]] uint64_t MigratedFromPeerId(not_null<PeerData*> canonical) {
	if (const auto from = canonical->migrateFrom()) {
		return from->id.value;
	}
	return 0;
}

[[nodiscard]] std::vector<uint64_t> ReadExcludedChats(
		not_null<Main::Session*> session) {
	return ParseExcludedTranslationChatIds(
		session->local().readPref<QByteArray>(kExcludedChatsPref));
}

void WriteExcludedChats(
		not_null<Main::Session*> session,
		std::vector<uint64_t> ids) {
	session->local().writePref<QByteArray>(
		kExcludedChatsPref,
		SerializeExcludedTranslationChatIds(std::move(ids)));
	ExcludedChatsChanges.fire_copy(session.get());
}

void ClearActiveTranslation(not_null<PeerData*> peer) {
	if (const auto history = peer->owner().historyLoaded(peer)) {
		if (history->translatedTo()) {
			history->translateTo({});
		}
		if (history->translateOfferedFrom()) {
			history->translateOfferFrom({});
		}
	}
}

class ExcludedTranslationChatsController final
	: public ChatsListBoxController {
public:
	ExcludedTranslationChatsController(
		not_null<Main::Session*> session,
		std::vector<not_null<PeerData*>> selected)
	: ChatsListBoxController(session)
	, _session(session)
	, _selected(std::move(selected)) {
	}

	Main::Session &session() const override {
		return *_session;
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

private:
	void prepareViewHook() override {
		delegate()->peerListSetTitle(
			tr::lng_purple_translate_excluded_chats());
		delegate()->peerListAddSelectedPeers(_selected);
	}

	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		if (const auto user = history->peer->asUser();
			user && user->isInaccessible()) {
			return nullptr;
		}
		return std::make_unique<Row>(history);
	}

	const not_null<Main::Session*> _session;
	std::vector<not_null<PeerData*>> _selected;

};

void EditExcludedTranslationChats(
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto original = ReadExcludedChats(session);
	auto originalSet = base::flat_set<uint64_t>(
		original.begin(),
		original.end());
	auto selected = std::vector<not_null<PeerData*>>();
	selected.reserve(original.size());
	for (const auto rawId : original) {
		selected.push_back(CanonicalPeer(session->data().peer(PeerId(rawId))));
	}
	auto init = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), [=] {
			auto checked = base::flat_set<uint64_t>();
			for (const auto peer : box->collectSelectedRows()) {
				checked.insert(CanonicalPeer(peer)->id.value);
			}
			for (const auto rawId : originalSet) {
				const auto peer = CanonicalPeer(
					session->data().peer(PeerId(rawId)));
				if (!checked.contains(peer->id.value)) {
					SetChatTranslationExcluded(peer, false);
				}
			}
			for (const auto rawId : checked) {
				if (!originalSet.contains(rawId)) {
					SetChatTranslationExcluded(
						session->data().peer(PeerId(rawId)),
						true);
				}
			}
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	controller->show(
		Box<PeerListBox>(
			std::make_unique<ExcludedTranslationChatsController>(
				session,
				std::move(selected)),
			std::move(init)),
		Ui::LayerOption::KeepOther);
}

} // namespace

rpl::producer<bool> TranslationPremiumValue(
		not_null<Main::Session*> session) {
	using namespace rpl::mappers;
	return rpl::combine(
		Data::AmPremiumValue(session),
		LocalPremiumValue(),
		_1 || _2);
}

bool TranslationPremiumAvailableForSession(
		not_null<Main::Session*> session) {
	return session->premium() || LocalPremium();
}

void UpdateTranslationOffer(
		not_null<History*> history,
		const std::vector<LanguageId> &recognized,
		int count,
		bool allLoaded) {
	if (ChatTranslationExcluded(history->peer)) {
		if (history->translateOfferedFrom()) {
			history->translateOfferFrom({});
		}
		return;
	}
	auto languages = base::flat_map<LanguageId, int>();
	for (const auto &language : recognized) {
		++languages[language];
	}
	using namespace base;
	constexpr auto p = &flat_multi_map_pair_type<LanguageId, int>::second;
	const auto threshold = (count > kTranslationRecognitionThreshold)
		? (count * kEnoughForTranslation / kTranslationRecognitionThreshold)
		: allLoaded
		? std::min(count, kEnoughForTranslation)
		: kEnoughForTranslation;
	const auto translatable = ranges::accumulate(
		languages,
		0,
		ranges::plus(),
		p);
	const auto was = history->translateOfferedFrom();
	// The threshold scales with all loaded messages and can rise while the
	// foreign-message count stays fixed. Keep an existing offer until those
	// messages fall below the minimum, rather than letting the bar flicker
	// while history loads.
	if (count >= kEnoughForTranslation) {
		if (translatable >= threshold) {
			history->translateOfferFrom(
				ranges::max_element(languages, ranges::less(), p)->first);
		} else if (!(was && translatable >= kEnoughForTranslation)) {
			history->translateOfferFrom({});
		}
	}
	const auto now = history->translateOfferedFrom();
	if (now != was) {
		LOG(("Purple: %1 translation of %2 - %3 of %4 messages recognised, "
			"threshold %5."
			).arg(QString::fromLatin1(now ? "offering" : "withdrawing")
			).arg(history->peer->name()
			).arg(translatable
			).arg(count
			).arg(threshold));
	} else if (!now && count >= kEnoughForTranslation && !languages.empty()) {
		LOG(("Purple: not offering translation of %1 - %2 of %3 messages "
			"recognised, need %4."
			).arg(history->peer->name()
			).arg(translatable
			).arg(count
			).arg(threshold));
	}
}

bool ChatTranslationExcluded(not_null<PeerData*> peer) {
	const auto canonical = CanonicalPeer(peer);
	return IsTranslationChatExcluded(
		ReadExcludedChats(&canonical->session()),
		canonical->id.value,
		MigratedFromPeerId(canonical));
}

bool ChatTranslationDisabled(not_null<PeerData*> peer) {
	using Flag = PeerData::TranslationFlag;
	return ShouldSuppressChatTranslation(
		ChatTranslationExcluded(peer),
		peer->translationFlag() != Flag::Enabled);
}

rpl::producer<> ChatTranslationExcludedChanges(
		not_null<Main::Session*> session) {
	return ExcludedChatsChanges.events(
	) | rpl::filter([=](Main::Session *changed) {
		return (changed == session.get());
	}) | rpl::to_empty;
}

rpl::producer<bool> ChatTranslationExcludedValue(not_null<PeerData*> peer) {
	return rpl::single(rpl::empty)
		| rpl::then(ChatTranslationExcludedChanges(&peer->session()))
		| rpl::map([=] { return ChatTranslationExcluded(peer); })
		| rpl::distinct_until_changed();
}

void SetChatTranslationExcluded(not_null<PeerData*> peer, bool excluded) {
	const auto canonical = CanonicalPeer(peer);
	const auto session = &canonical->session();
	const auto migratedFrom = canonical->migrateFrom();
	auto ids = SetExcludedTranslationChatId(
		ReadExcludedChats(session),
		canonical->id.value,
		migratedFrom ? migratedFrom->id.value : 0,
		excluded);
	WriteExcludedChats(session, std::move(ids));

	canonical->saveTranslationDisabled(excluded);
	if (migratedFrom) {
		migratedFrom->setTranslationDisabled(excluded);
	}
	if (excluded) {
		ClearActiveTranslation(canonical);
		if (migratedFrom) {
			ClearActiveTranslation(migratedFrom);
		}
	}
}

void AddTranslateBarExcludeAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<PeerData*> peer) {
	const auto session = base::make_weak(&peer->session());
	const auto id = peer->id;
	menu->addAction(
		tr::lng_purple_translate_dont_chat(tr::now),
		[=] {
			const auto live = session.get();
			const auto peer = live ? live->data().peerLoaded(id) : nullptr;
			if (!peer) {
				return;
			}
			SetChatTranslationExcluded(peer, true);
			ShowPurpleNotice({
				.session = live,
				.scope = u"Translation"_q,
				.summary = tr::lng_purple_translate_excluded_toast(tr::now),
				.coalesceKey = u"translation:excluded:"_q + QString::number(id.value),
				.primaryAction = PurpleNoticeAction{
					.label = tr::lng_translate_undo(tr::now),
					.run = [=](not_null<Window::Controller*>) {
						if (const auto live = session.get()) {
							if (const auto peer = live->data().peerLoaded(id)) {
								SetChatTranslationExcluded(peer, false);
							}
						}
					},
				},
			});
		},
		&st::menuIconBlock);
}

void AddExcludedTranslationChatsRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	auto label = rpl::single(rpl::empty)
		| rpl::then(ChatTranslationExcludedChanges(session))
		| rpl::map([=] {
			const auto count = int(ReadExcludedChats(session).size());
			return count
				? tr::lng_purple_translate_excluded_count(
					tr::now,
					lt_count,
					count)
				: tr::lng_purple_translate_excluded_none(tr::now);
		});
	const auto button = ::Settings::AddButtonWithLabel(
		container,
		tr::lng_purple_translate_excluded_chats(),
		std::move(label),
		st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		EditExcludedTranslationChats(controller);
	});
}

} // namespace Purple
