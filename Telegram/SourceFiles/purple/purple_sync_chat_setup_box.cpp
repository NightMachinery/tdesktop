#include "purple/purple_sync_chat_setup_box.h"

#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include <QtCore/QPointer>
#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_chat_peer_request.h"
#include "purple/purple_sync_chat_service.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Purple {
namespace {

PeerData *Loaded(Main::Session &session, PeerToken chat) {
	if (chat == PeerToken{}) {
		return session.user();
	}
	const auto id = chat.kind == PeerTokenKind::User
		? peerFromUser(UserId(chat.id)) : chat.kind == PeerTokenKind::Chat
		? peerFromChat(ChatId(chat.id)) : peerFromChannel(ChannelId(chat.id));
	return session.data().peerLoaded(id);
}

QString Title(Main::Session &session, PeerToken chat) {
	if (chat == PeerToken{}) {
		return tr::lng_purple_chat_self(tr::now);
	}
	const auto peer = Loaded(session, chat);
	return peer ? peer->name()
		: PeerTokenText(chat, session.userId().bare).value_or(QString());
}

QString Refusal(SyncChatRefusal refusal) {
	switch (refusal) {
	case SyncChatRefusal::None: return {};
	case SyncChatRefusal::Secret: return tr::lng_purple_chat_refuse_secret(tr::now);
	case SyncChatRefusal::TopicRouting: return tr::lng_purple_chat_refuse_topic(tr::now);
	case SyncChatRefusal::Public: return tr::lng_purple_chat_refuse_public(tr::now);
	case SyncChatRefusal::Protected: return tr::lng_purple_chat_refuse_protected(tr::now);
	case SyncChatRefusal::Discussion: return tr::lng_purple_chat_refuse_discussion(tr::now);
	case SyncChatRefusal::AutoDelete: return tr::lng_purple_chat_refuse_ttl(tr::now);
	case SyncChatRefusal::Paid: return tr::lng_purple_chat_refuse_paid(tr::now);
	case SyncChatRefusal::CannotSendFiles: return tr::lng_purple_chat_refuse_files(tr::now);
	case SyncChatRefusal::SendAsOther: return tr::lng_purple_chat_refuse_sender(tr::now);
	case SyncChatRefusal::Migrated: return tr::lng_purple_chat_refuse_migrated(tr::now);
	case SyncChatRefusal::SlowMode: return tr::lng_purple_chat_refuse_slow(tr::now);
	case SyncChatRefusal::BroadcastNeedsCreatorAndSoleAdmin:
		return tr::lng_purple_chat_refuse_admin(tr::now);
	case SyncChatRefusal::UnknownKind:
	case SyncChatRefusal::BroadcastOwnershipUnselected:
		return tr::lng_purple_chat_refuse_unknown(tr::now);
	}
	return tr::lng_purple_chat_refuse_unknown(tr::now);
}

QString SettingsDisclosure(const QString &chat) {
	return tr::lng_purple_chat_settings_reader_warning(tr::now)
		+ u"\n"_q + tr::lng_purple_chat_settings_disclosure(tr::now).arg(chat);
}

QString Readers(const SyncChatReaderFacts &readers, const QString &chat) {
	const auto text = readers.populationUnknown
		? tr::lng_purple_chat_readers_unknown(tr::now)
		: readers.privateRecipient ? tr::lng_purple_chat_readers_private(tr::now)
		: readers.botOwner ? tr::lng_purple_chat_readers_bot(tr::now)
		: tr::lng_purple_chat_readers_count(tr::now).arg(
			QString::number(readers.otherMembers.value_or(0)),
			QString::number(readers.otherAdmins.value_or(0)));
	return text + u"\n"_q + SettingsDisclosure(chat)
		+ u"\n"_q + tr::lng_purple_chat_cloud_disclosure(tr::now);
}

QString StatusText(Main::Session &session,
		const DesktopSyncChatStatus &status) {
	auto text = QString();
	switch (status.phase) {
	case DesktopSyncChatPhase::Unset:
		text = tr::lng_purple_chat_unset(tr::now); break;
	case DesktopSyncChatPhase::Searching:
		text = tr::lng_purple_chat_searching(tr::now); break;
	case DesktopSyncChatPhase::Use:
		text = tr::lng_purple_chat_use(tr::now).arg(Title(session,
			status.resolution.readChat.value_or(PeerToken{}))); break;
	case DesktopSyncChatPhase::Paused:
		text = tr::lng_purple_chat_paused(tr::now); break;
	case DesktopSyncChatPhase::Lost:
		text = tr::lng_purple_chat_lost(tr::now); break;
	}
	if (!status.reason.isEmpty()) {
		text += u"\n"_q + (status.reason == u"resolving"_q
			? tr::lng_purple_chat_searching(tr::now)
			: status.reason == u"newer-mark"_q
			? tr::lng_purple_chat_update_required(tr::now)
			: status.reason == u"unknown-readers"_q
			? tr::lng_purple_chat_readers_unknown(tr::now)
			: status.reason == u"follow-required"_q || status.reason == u"fill-required"_q
			? tr::lng_purple_chat_change_disabled(tr::now)
			: tr::lng_purple_chat_check_failed(tr::now));
	}
	if (status.postingHeld) {
		text += u"\n"_q + tr::lng_purple_chat_posting_held(tr::now);
	}
	if (status.newReaders) {
		text += u"\n\n"_q + tr::lng_purple_chat_new_readers(tr::now)
			+ u"\n"_q + tr::lng_purple_chat_reader_blind_spot(tr::now);
		const auto peer = Loaded(session,
			status.resolution.readChat.value_or(PeerToken{}));
		const auto channel = peer ? peer->asChannel() : nullptr;
		text += u"\n"_q + (channel && channel->hiddenPreHistory()
			? tr::lng_purple_chat_future_readers(tr::now)
			: tr::lng_purple_chat_prior_readers(tr::now));
	}
	return text;
}

struct Choice {
	DesktopSyncChatSetupSelection selection;
	QString title;
};

struct SetupState {
	base::weak_ptr<Main::Session> session;
	Fn<bool()> current;
	std::optional<AccountSettingsOpenTarget> target;
	std::optional<DesktopSyncChatSetupSnapshot> opening;
	std::shared_ptr<Ui::RadiobuttonGroup> group;
	std::vector<Choice> choices;
	std::vector<PeerToken> pending;
	std::unique_ptr<DesktopSyncChatPeerRequest> request;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *accept = nullptr;
	size_t next = 0;
	bool busy = false;
	bool qualifiedHeld = false;
};

bool Current(const SetupState &state) {
	const auto session = state.session.get();
	const auto fresh = session ? SyncChatSetupSnapshotOf(session) : std::nullopt;
	return session && state.current && state.current() && state.target
		&& ValidateAccountSettingsOpenTarget(session, *state.target)
		&& state.opening && fresh && fresh->run == state.opening->run
		&& fresh->scope == state.opening->scope;
}

void LoadNext(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!Current(*state) || state->next >= state->pending.size()) {
		return;
	}
	const auto chat = state->pending[state->next++];
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	state->request = std::make_unique<DesktopSyncChatPeerRequest>(
		state->session.get(), state->opening->scope.origin, chat,
		[weak, state] { return weak && Current(*state); },
		crl::guard(box, [=](std::optional<DesktopSyncChatPeer> peer) {
			if (!Current(*state)) {
				return;
			}
			const auto title = Title(*state->session.get(), chat);
			const auto eligible = peer && DesktopSyncChatSetupEligibility(
				*state->opening, *peer) == DesktopSyncChatSetupRefusal::None;
			const auto index = int(state->choices.size());
			state->choices.push_back({ { chat == PeerToken{}
				? DesktopSyncChatSetupChoice::Self
				: DesktopSyncChatSetupChoice::Existing, peer, false }, title });
			const auto predicate = peer ? DesktopSyncChatEligibility(*peer)
				: std::nullopt;
			const auto label = eligible ? title : title + u": "_q
				+ (predicate && !*predicate ? Refusal(predicate->refusal)
					: tr::lng_purple_chat_refuse_unknown(tr::now));
			const auto row = box->addRow(object_ptr<Ui::Radiobutton>(box,
				state->group, index, label, st::defaultCheckbox));
			row->setDisabled(!eligible);
			crl::on_main(box, [=] { LoadNext(box, state); });
		}));
}

void Accept(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!Current(*state) || state->busy || state->qualifiedHeld) {
		state->status->setText(tr::lng_purple_chat_stale(tr::now));
		return;
	}
	const auto index = state->group->current();
	if (index < 0 || index >= int(state->choices.size())) {
		return;
	}
	state->busy = true;
	state->accept->setDisabled(true);
	const auto chosen = state->choices[index];
	const auto confirm = [=](std::optional<DesktopSyncChatPeer> peer) {
		state->busy = false;
		state->accept->setDisabled(false);
		if (!Current(*state)) {
			state->status->setText(tr::lng_purple_chat_stale(tr::now));
			return;
		}
		auto selection = chosen.selection;
		selection.peer = peer;
		const auto snapshot = *SyncChatSetupSnapshotOf(state->session.get());
		const auto result = AcceptDesktopSyncChatSetup(
			*state->opening, snapshot, selection);
		if (result != DesktopSyncChatSetupRefusal::None
			&& result != DesktopSyncChatSetupRefusal::ConsentRequired) {
			state->status->setText(tr::lng_purple_chat_refuse_unknown(tr::now));
			return;
		}
		const auto disclosure = peer
			? Readers(DesktopSyncChatEligibility(*peer)->readers, chosen.title)
			: tr::lng_purple_chat_create_disclosure(tr::now)
				+ u"\n"_q + SettingsDisclosure(chosen.title);
		box->uiShow()->showBox(Ui::MakeConfirmBox({
			.text = chosen.title + u"\n\n"_q + disclosure,
			.confirmed = crl::guard(box, [=](Fn<void()> close) {
				close();
				if (!Current(*state) || state->busy || state->qualifiedHeld) {
					state->status->setText(tr::lng_purple_chat_stale(tr::now));
					return;
				}
				state->busy = true;
				state->accept->setDisabled(true);
				const auto finish = crl::guard(box,
					[=](std::optional<DesktopSyncChatPeer> acceptedPeer) {
						state->busy = false;
						const auto fresh = state->session
							? SyncChatSetupSnapshotOf(state->session.get()) : std::nullopt;
						const auto same = !peer || (acceptedPeer
							&& SameDesktopSyncChatDisclosure(*peer, *acceptedPeer));
						auto accepted = selection;
						accepted.peer = acceptedPeer;
						accepted.readerConsent = true;
						if (!Current(*state) || !fresh || !same
							|| AcceptDesktopSyncChatSetup(*state->opening,
								*fresh, accepted) != DesktopSyncChatSetupRefusal::None) {
							state->accept->setDisabled(!Current(*state));
							state->status->setText(tr::lng_purple_chat_stale(tr::now));
							return;
						}
						state->qualifiedHeld = true;
						state->status->setText(tr::lng_purple_chat_qualification_held(tr::now));
					});
				if (selection.choice == DesktopSyncChatSetupChoice::Create) {
					finish(std::optional<DesktopSyncChatPeer>());
				} else {
					const auto weak = QPointer<Ui::GenericBox>(box.get());
					state->request = std::make_unique<DesktopSyncChatPeerRequest>(
						state->session.get(), state->opening->scope.origin,
						selection.peer->chat,
						[weak, state] { return weak && Current(*state); }, finish);
				}
			}),
			.confirmText = tr::lng_continue(),
		}));
	};
	if (chosen.selection.choice == DesktopSyncChatSetupChoice::Create) {
		confirm({});
	} else {
		const auto weak = QPointer<Ui::GenericBox>(box.get());
		state->request = std::make_unique<DesktopSyncChatPeerRequest>(
			state->session.get(), state->opening->scope.origin,
			chosen.selection.peer->chat,
			[weak, state] { return weak && Current(*state); },
			crl::guard(box, confirm));
	}
}

}

void SyncChatSetupBox(not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session, Fn<bool()> current) {
	box->setTitle(tr::lng_purple_chat_title());
	box->setWidth(st::boxWideWidth);
	const auto state = box->lifetime().make_state<SetupState>();
	state->session = session;
	state->current = std::move(current);
	state->target = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
	state->opening = SyncChatSetupSnapshotOf(session);
	state->status = box->addRow(object_ptr<Ui::FlatLabel>(box,
		StatusText(*session, SyncChatStatusOf(session)), st::boxLabel));
	const auto refresh = [=] {
		if (!state->qualifiedHeld) {
			state->status->setText(Current(*state)
				? StatusText(*session, SyncChatStatusOf(session))
				: tr::lng_purple_chat_stale(tr::now));
		}
		if (state->accept) {
			state->accept->setDisabled(!Current(*state) || state->qualifiedHeld
				|| state->busy);
		}
	};
	SyncChatStatusChanges() | rpl::on_next([=](Main::Session *changed) {
		if (changed == state->session.get()) {
			refresh();
		}
	}, box->lifetime());
	SettingsChanges() | rpl::on_next(refresh, box->lifetime());
	StateChanges() | rpl::on_next(refresh, box->lifetime());
	if (state->opening) {
		const auto &snapshot = *state->opening;
		if (snapshot.status.newReaders && snapshot.cache) {
			box->addRow(object_ptr<Ui::FlatLabel>(box,
				tr::lng_purple_chat_original_readers(tr::now).arg(
					snapshot.cache->readers.otherMembers
						? QString::number(*snapshot.cache->readers.otherMembers)
						: tr::lng_purple_chat_unknown_count(tr::now),
					snapshot.cache->readers.otherAdmins
						? QString::number(*snapshot.cache->readers.otherAdmins)
						: tr::lng_purple_chat_unknown_count(tr::now)), st::boxLabel));
		}
		if (snapshot.status.resolution.readChat) {
			box->addRow(object_ptr<Ui::FlatLabel>(box,
				Readers(snapshot.status.resolution.readers,
					Title(*session, *snapshot.status.resolution.readChat)), st::boxLabel));
		}
		for (const auto &mark : snapshot.input.marks) {
			if (!mark.eligibility) {
				box->addRow(object_ptr<Ui::FlatLabel>(box,
					tr::lng_purple_chat_refused_mark(tr::now).arg(
						Title(*session, mark.location.chat), Refusal(mark.eligibility.refusal)),
					st::boxLabel));
			}
		}
	}
	if (Current(*state) && DesktopSyncChatFreshSetup(*state->opening)) {
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_chat_fresh_choices(tr::now), st::boxLabel));
		state->group = std::make_shared<Ui::RadiobuttonGroup>();
		state->choices.push_back({ { DesktopSyncChatSetupChoice::Create },
			tr::lng_purple_chat_create(tr::now) });
		box->addRow(object_ptr<Ui::Radiobutton>(box, state->group, 0,
			state->choices.front().title, st::defaultCheckbox));
		state->group->setValue(0);
		state->pending.push_back({});
		for (const auto folder : {static_cast<Data::Folder*>(nullptr),
			session->data().folder(1).get()}) {
			for (const auto row : *session->data().chatsList(folder)->indexed()) {
				const auto history = row->key().history();
				const auto token = history ? DesktopSyncChatToken(
					peerToMTP(history->peer->id), session->userId().bare) : std::nullopt;
				if (token && std::find(state->pending.begin(), state->pending.end(),
					*token) == state->pending.end() && state->pending.size() < 51) {
					state->pending.push_back(*token);
				}
			}
		}
		state->accept = box->addButton(tr::lng_continue(), [=] { Accept(box, state); });
		LoadNext(box, state);
	} else {
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_chat_change_disabled(tr::now), st::boxLabel));
	}
	box->addButton(tr::lng_purple_chat_check_again(), [=] {
		const auto live = state->session.get();
		if (!live || !state->current || !state->current() || !state->target
			|| !ValidateAccountSettingsOpenTarget(live, *state->target)) {
			state->status->setText(tr::lng_purple_chat_stale(tr::now));
			return;
		}
		EnsureSyncChatResolved(live, true);
		box->closeBox();
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

}
