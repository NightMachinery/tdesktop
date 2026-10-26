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
#include "purple/purple_sync_chat_setup_service.h"
#include "purple/purple_sync_request_sender.h"
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
	if (const auto pending = PendingSyncRequest(&session.mtp())) {
		text += u"\n"_q + SyncRequestWaitText(*pending);
	} else if (status.reason == u"telegram-transient"_q && status.retryAt > 0) {
		text += u"\n"_q + SyncRequestWaitText(
			{ SyncTransientKind::Timeout, status.retryAt });
	} else if (!status.reason.isEmpty()) {
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
	std::unique_ptr<DesktopSyncChatSetupService> service;
	DesktopSyncChatSetupProgress progress;
	Ui::RoundButton *retry = nullptr;
	Ui::RoundButton *self = nullptr;
	Ui::RoundButton *consent = nullptr;
	Ui::RoundButton *check = nullptr;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *accept = nullptr;
	size_t next = 0;
	uint64_t serviceGeneration = 0;
	bool busy = false;
	bool loading = false;
};

bool Current(const SetupState &state) {
	const auto session = state.session.get();
	const auto fresh = session ? SyncChatSetupSnapshotOf(session) : std::nullopt;
	return session && state.current && state.current() && state.target
		&& ValidateAccountSettingsOpenTarget(session, *state.target)
		&& state.opening && (state.service
			? SyncChatSetupContextCurrent(session, state.opening->scope)
			: fresh && fresh->run == state.opening->run
				&& fresh->scope == state.opening->scope);
}

void LoadNext(not_null<Ui::GenericBox*> box, SetupState *state);

bool RestoreFreshSetup(SetupState *state, uint64_t generation, const QString &message) {
	if (!state->service || state->serviceGeneration != generation
		|| !state->progress.resumeAbsent) {
		return false;
	}
	++state->serviceGeneration;
	state->service.reset();
	state->progress = {};
	state->busy = false;
	state->status->setText(message);
	if (state->accept) { state->accept->setDisabled(!Current(*state)); }
	if (state->check) { state->check->setDisabled(false); }
	if (state->retry) { state->retry->setDisabled(!Current(*state)); }
	if (state->self) { state->self->hide(); }
	if (state->consent) { state->consent->hide(); }
	return true;
}

void SetupProgress(not_null<Ui::GenericBox*> box, SetupState *state,
		DesktopSyncChatSetupProgress progress) {
	state->progress = std::move(progress);
	if (state->progress.resumeAbsent) {
		const auto generation = state->serviceGeneration;
		const auto message = tr::lng_purple_chat_setup_no_journal(tr::now);
		state->status->setText(message);
		crl::on_main(box, [=] {
			if (RestoreFreshSetup(state, generation, message)) { LoadNext(box, state); }
		});
		return;
	}
	state->busy = state->progress.phase == DesktopSyncChatCreatePhase::Running;
	auto text = state->progress.phase == DesktopSyncChatCreatePhase::Complete
		? tr::lng_purple_chat_setup_complete(tr::now)
		: state->progress.phase == DesktopSyncChatCreatePhase::Cancelled
		? tr::lng_purple_chat_setup_cancelled(tr::now)
		: state->progress.phase == DesktopSyncChatCreatePhase::Limit
		? tr::lng_purple_chat_setup_limit(tr::now)
		: state->busy ? tr::lng_purple_chat_setup_running(tr::now)
		: tr::lng_purple_chat_setup_held(tr::now);
	if (!state->progress.reason.isEmpty()) {
		text += u"\n"_q + tr::lng_purple_chat_setup_reason(tr::now).arg(state->progress.reason);
	}
	if (state->progress.reason == u"setup-resume-required"_q && state->progress.journal) {
		text += u"\n"_q + tr::lng_purple_chat_setup_resume(tr::now).arg(
			state->progress.journal->chat && state->session
				? Title(*state->session.get(), *state->progress.journal->chat)
				: tr::lng_purple_chat_create(tr::now));
	}
	if (state->session) {
		if (const auto pending = PendingSyncRequest(&state->session->mtp())) {
			text += u"\n"_q + SyncRequestWaitText(*pending);
		}
	}
	if (state->progress.journal && state->progress.journal->retryAt > 0) {
		text += u"\n"_q + SyncRequestWaitText({ SyncTransientKind::Flood,
			state->progress.journal->retryAt });
	}
	if (state->progress.journal && state->progress.journal->newReaders) {
		text += u"\n"_q + tr::lng_purple_chat_new_readers(tr::now);
	}
	state->status->setText(std::move(text));
	if (state->check) { state->check->setDisabled(state->service != nullptr); }
	if (state->retry) { state->retry->show(); state->retry->setDisabled(state->busy || !Current(*state)
		|| state->progress.phase == DesktopSyncChatCreatePhase::Complete); }
	if (state->self) { state->self->setVisible(state->progress.phase == DesktopSyncChatCreatePhase::Limit); state->self->setDisabled(!Current(*state)
		|| state->progress.phase != DesktopSyncChatCreatePhase::Limit); }
	if (state->consent) { state->consent->setVisible(state->progress.consent.has_value()); state->consent->setDisabled(!Current(*state) || !state->progress.consent); }
	if (state->accept) { state->accept->setDisabled(state->service || !Current(*state)); }
}

void LoadNext(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (state->service || state->loading || !Current(*state) || state->next >= state->pending.size()) {
		return;
	}
	const auto chat = state->pending[state->next++];
	state->loading = true;
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	state->request = std::make_unique<DesktopSyncChatPeerRequest>(
		state->session.get(), state->opening->scope.origin, chat,
		[weak, state] { return weak && Current(*state); },
		crl::guard(box, [=](std::optional<DesktopSyncChatPeer> peer) {
			state->loading = false;
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
	if (!Current(*state) || state->busy || state->service) {
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
				if (!Current(*state) || state->busy || state->service) {
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
						++state->serviceGeneration;
						state->service = std::make_unique<DesktopSyncChatSetupService>(
							state->session.get(), *fresh, accepted,
							[weak = QPointer<Ui::GenericBox>(box.get()), state] {
								return weak && state->session && state->current && state->current()
									&& state->target && ValidateAccountSettingsOpenTarget(
										state->session.get(), *state->target);
							}, crl::guard(box, [=](DesktopSyncChatSetupProgress progress) {
								SetupProgress(box, state, std::move(progress));
							}));
						state->request.reset();
						state->service->start();
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
		if (!state->service) {
			state->status->setText(Current(*state)
				? StatusText(*session, SyncChatStatusOf(session))
				: tr::lng_purple_chat_stale(tr::now));
		} else {
			SetupProgress(box, state, state->progress);
		}
		if (state->accept) {
			state->accept->setDisabled(!Current(*state) || state->service
				|| state->busy);
		}
	};
	SyncRequestChanges() | rpl::on_next([=](MTP::Instance *instance) {
		if (state->session && &state->session->mtp() == instance) {
			refresh();
		}
	}, box->lifetime());
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
	state->retry = box->addRow(object_ptr<Ui::RoundButton>(box,
		tr::lng_purple_chat_setup_retry(), st::defaultBoxButton));
	state->retry->setClickedCallback([=] {
		if (!Current(*state)) { return; }
		if (state->service) { state->service->retry(); return; }
		if (state->loading) {
			state->loading = false;
			--state->next;
		}
		state->request.reset();
		++state->serviceGeneration;
		state->service = std::make_unique<DesktopSyncChatSetupService>(
			state->session.get(), *state->opening, DesktopSyncChatSetupSelection{},
			[weak = QPointer<Ui::GenericBox>(box.get()), state] {
				return weak && state->session && state->current && state->current()
					&& state->target && ValidateAccountSettingsOpenTarget(
						state->session.get(), *state->target);
			}, crl::guard(box, [=](DesktopSyncChatSetupProgress progress) {
				SetupProgress(box, state, std::move(progress));
			}), true);
		state->service->start();
	});
	state->retry->setDisabled(!Current(*state));
	state->self = box->addRow(object_ptr<Ui::RoundButton>(box,
		tr::lng_purple_chat_setup_self(), st::defaultBoxButton));
	state->self->setClickedCallback([=] { if (state->service) { state->service->useSelf(); } });
	state->self->hide();
	state->consent = box->addRow(object_ptr<Ui::RoundButton>(box,
		tr::lng_purple_chat_setup_consent(), st::defaultBoxButton));
	state->consent->setClickedCallback([=] {
		if (!state->service || !state->progress.consent || !Current(*state)) { return; }
		const auto offer = *state->progress.consent;
		box->uiShow()->showBox(Ui::MakeConfirmBox({
			.text = Readers(DesktopSyncChatEligibility(offer.peer)->readers,
				Title(*state->session.get(), offer.peer.chat)),
			.confirmed = crl::guard(box, [=](Fn<void()> close) {
				close();
				if (state->service && Current(*state)) { state->service->reconsent(offer); }
			}),
			.confirmText = tr::lng_continue(),
		}));
	});
	state->consent->hide();
	state->check = box->addButton(tr::lng_purple_chat_check_again(), [=] {
		if (state->service) { return; }
		const auto live = state->session.get();
		if (!live || !state->current || !state->current() || !state->target
			|| !ValidateAccountSettingsOpenTarget(live, *state->target)) {
			state->status->setText(tr::lng_purple_chat_stale(tr::now));
			return;
		}
		EnsureSyncChatResolved(live, true);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] {
		if (state->service) { state->service->cancel(); }
		box->closeBox();
	});
}

}
