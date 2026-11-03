#include "purple/purple_sync_magic_box.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_sync_magic_service.h"
#include "purple/purple_sync_magic_telegram.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QLocale>

namespace Purple {
namespace {

constexpr auto kProblemToastDuration = crl::time(8000);

SyncMagicFrame FrameOf(not_null<Main::Session*> session) {
	const auto chat = SyncMagicChatOf(session);
	return chat
		? SyncMagicFrameOf(chat->last(), chat->binding())
		: SyncMagicFrame();
}

std::optional<MagicChatCandidate> LocalOf(
		not_null<Main::Session*> session,
		PeerToken chat) {
	if (chat.kind != PeerTokenKind::Channel) {
		return std::nullopt;
	}
	const auto channel = session->data().channelLoaded(ChannelId(chat.id));
	if (!channel) {
		return std::nullopt;
	}
	auto result = SyncMagicLocalCandidate(channel);
	if (channel->membersCountKnown()) {
		result.facts.otherMembers = uint64_t(std::max(channel->membersCount() - 1, 0));
	}
	return result;
}

QString PickRefusal(SyncMagicPick pick) {
	switch (pick) {
	case SyncMagicPick::NotAlone: return tr::lng_purple_magic_pick_not_alone(tr::now);
	case SyncMagicPick::Unknown: return tr::lng_purple_magic_pick_unknown(tr::now);
	case SyncMagicPick::Unsupported: return tr::lng_purple_magic_pick_unsupported(tr::now);
	case SyncMagicPick::Allowed: break;
	}
	return QString();
}

void AddChoices(
		not_null<Dialogs::MainList*> list,
		std::vector<not_null<ChannelData*>> &result) {
	for (const auto &row : list->indexed()->all()) {
		const auto history = row->history();
		const auto channel = history ? history->peer->asMegagroup() : nullptr;
		if (channel && channel->amIn()) {
			result.push_back(channel);
		}
	}
}

void ChooseChatBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Fn<bool()> current,
		Fn<void(PeerToken)> chosen) {
	box->setTitle(tr::lng_purple_magic_choose());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_purple_magic_choose_about(tr::now),
		st::boxDividerLabel));
	const auto container = box->verticalLayout();
	const auto add = [=](const QString &name, PeerToken chat) {
		::Settings::AddButtonWithIcon(
			container,
			rpl::single(name),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			if (!current()) {
				box->closeBox();
				return;
			}
			const auto refusal = PickRefusal(SyncMagicPickOf(chat, LocalOf(session, chat)));
			if (!refusal.isEmpty()) {
				box->uiShow()->showToast(refusal);
				return;
			}
			box->closeBox();
			chosen(chat);
		});
	};
	add(tr::lng_purple_chat_self(tr::now), PeerToken{});
	auto channels = std::vector<not_null<ChannelData*>>();
	auto &data = session->data();
	AddChoices(data.chatsList(nullptr), channels);
	if (const auto archive = data.folderLoaded(Data::Folder::kId)) {
		AddChoices(archive->chatsList(), channels);
	}
	for (const auto &channel : channels) {
		add(channel->name(), PeerToken{
			PeerTokenKind::Channel,
			peerToChannel(channel->id).bare });
	}
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void OpenChat(
		not_null<Main::Session*> session,
		PeerToken chat,
		std::shared_ptr<Ui::Show> show) {
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	const auto peer = (chat.kind == PeerTokenKind::Channel)
		? session->data().channelLoaded(ChannelId(chat.id))
		: nullptr;
	if (!controller || &controller->session() != session || !peer) {
		show->showToast(tr::lng_purple_magic_open_failed(tr::now));
		return;
	}
	controller->showPeerHistory(peer);
}

} // namespace

QString SyncMagicProblemText(SyncChatProblem problem) {
	switch (problem) {
	case SyncChatProblem::ChatGone: return tr::lng_purple_magic_problem_chat_gone(tr::now);
	case SyncChatProblem::ChatLeft: return tr::lng_purple_magic_problem_chat_left(tr::now);
	case SyncChatProblem::NotMegagroup: return tr::lng_purple_magic_problem_not_megagroup(tr::now);
	case SyncChatProblem::NotCreator: return tr::lng_purple_magic_problem_not_creator(tr::now);
	case SyncChatProblem::ExtraMember: return tr::lng_purple_magic_problem_extra_member(tr::now);
	case SyncChatProblem::ExtraAdmin: return tr::lng_purple_magic_problem_extra_admin(tr::now);
	case SyncChatProblem::PublicUsername: return tr::lng_purple_magic_problem_public_username(tr::now);
	case SyncChatProblem::Restricted: return tr::lng_purple_magic_problem_restricted(tr::now);
	case SyncChatProblem::CannotPost: return tr::lng_purple_magic_problem_cannot_post(tr::now);
	case SyncChatProblem::ForeignRecord: return tr::lng_purple_magic_problem_foreign_record(tr::now);
	case SyncChatProblem::ForwardedRecord: return tr::lng_purple_magic_problem_forwarded_record(tr::now);
	case SyncChatProblem::ViaBotRecord: return tr::lng_purple_magic_problem_via_bot_record(tr::now);
	case SyncChatProblem::CreateLimit: return tr::lng_purple_magic_problem_create_limit(tr::now);
	case SyncChatProblem::MagicIneligible: return tr::lng_purple_magic_problem_magic_ineligible(tr::now);
	case SyncChatProblem::SpaceMismatch: return tr::lng_purple_magic_problem_space_mismatch(tr::now);
	case SyncChatProblem::RequestRefused: return tr::lng_purple_magic_problem_request_refused(tr::now);
	case SyncChatProblem::None: break;
	}
	return QString();
}

QString SyncMagicStatusText(
		not_null<Main::Session*> session,
		const SyncMagicFrame &frame) {
	switch (frame.line) {
	case SyncMagicFrame::Line::Using: {
		const auto name = frame.savedMessages()
			? tr::lng_purple_chat_self(tr::now)
			: frame.manual
			? [&] {
				const auto local = LocalOf(session, *frame.chat);
				return local ? local->title : tr::lng_purple_magic_magic_chat(tr::now);
			}()
			: tr::lng_purple_magic_magic_chat(tr::now);
		const auto line = tr::lng_purple_magic_using(tr::now, lt_chat, name);
		return frame.manual
			? (line + u' ' + tr::lng_purple_magic_picked(tr::now))
			: line;
	}
	case SyncMagicFrame::Line::Waiting: {
		const auto time = QLocale().toString(
			base::unixtime::parse(TimeId(frame.retryAt)),
			QLocale::ShortFormat);
		return frame.telegram
			? (frame.retryAt > 0
				? tr::lng_purple_magic_waiting(tr::now, lt_time, time)
				: tr::lng_purple_magic_waiting_later(tr::now))
			: (frame.retryAt > 0
				? tr::lng_purple_magic_retrying(tr::now, lt_time, time)
				: tr::lng_purple_magic_retrying_later(tr::now));
	}
	case SyncMagicFrame::Line::Problem:
		return tr::lng_purple_magic_paused(
			tr::now,
			lt_reason,
			SyncMagicProblemText(frame.problem));
	case SyncMagicFrame::Line::Looking:
		break;
	}
	return tr::lng_purple_magic_looking(tr::now);
}

void ShowSyncMagicProblemOnce(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show) {
	const auto chat = SyncMagicChatOf(session);
	const auto frame = FrameOf(session);
	if (!chat || !frame.showProblem || !show) {
		return;
	}
	chat->shown();
	show->showBox(Ui::MakeInformBox(SyncMagicStatusText(session, frame)));
}

void ToastSyncMagicProblem(not_null<Main::Session*> session) {
	const auto chat = SyncMagicChatOf(session);
	const auto frame = FrameOf(session);
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!chat || !frame.showProblem || !controller || &controller->session() != session) {
		return;
	}
	chat->shown();
	const auto weak = base::make_weak(controller);
	controller->uiShow()->showToast({
		.text = TextWithEntities{ SyncMagicStatusText(session, frame) + ' ' }.append(
			Ui::Text::Link(tr::lng_purple_magic_problem_open(tr::now))),
		.filter = [=](const ClickHandlerPtr &, Qt::MouseButton) {
			if (const auto strong = weak.get()) {
				strong->show(Box(SyncMagicChatBox, &strong->session(), [=] {
					return weak.get() != nullptr;
				}));
			}
			return false;
		},
		.duration = kProblemToastDuration,
	});
}

void SyncMagicChatBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Fn<bool()> current) {
	box->setTitle(tr::lng_purple_chat_title());
	box->setWidth(st::boxWideWidth);
	const auto weak = base::make_weak(session);
	const auto live = [=] {
		return weak.get() && current && current() && SyncMagicChatOf(session);
	};
	const auto container = box->verticalLayout();
	const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		QString(),
		st::boxLabel));
	const auto check = ::Settings::AddButtonWithIcon(
		container,
		tr::lng_purple_magic_check(),
		st::settingsButtonNoIcon);
	const auto open = ::Settings::AddButtonWithIcon(
		container,
		tr::lng_purple_magic_open(),
		st::settingsButtonNoIcon);
	const auto listed = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::lng_purple_magic_listed(tr::now),
		false));
	const auto listedAbout = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_purple_magic_listed_about(tr::now),
		st::boxDividerLabel));
	const auto choose = ::Settings::AddButtonWithIcon(
		container,
		tr::lng_purple_magic_choose(),
		st::settingsButtonNoIcon);
	const auto useDefault = ::Settings::AddButtonWithIcon(
		container,
		tr::lng_purple_magic_use_default(),
		st::settingsButtonNoIcon);
	const auto join = ::Settings::AddButtonWithIcon(
		container,
		tr::lng_purple_magic_join(),
		st::settingsButtonNoIcon);

	const auto update = [=] {
		if (!live()) {
			status->setText(tr::lng_purple_chat_select_account(tr::now));
			for (const auto button : { check, open, choose, useDefault, join }) {
				button->setDisabled(true);
			}
			listed->setEnabled(false);
			return;
		}
		const auto frame = FrameOf(session);
		status->setText(SyncMagicStatusText(session, frame));
		check->setDisabled(false);
		choose->setDisabled(false);
		open->setVisible(frame.canOpen());
		listed->setVisible(frame.canList());
		listedAbout->setVisible(frame.canList());
		listed->setEnabled(true);
		listed->setChecked(frame.listed, Ui::Checkbox::NotifyAboutChange::DontNotify);
		useDefault->setVisible(frame.canUseDefault());
		join->setVisible(frame.canJoin());
		ShowSyncMagicProblemOnce(session, box->uiShow());
	};
	const auto edited = [=](SyncMagicResult result) {
		if (result.kind == SyncMagicResult::Kind::Unavailable && live()) {
			box->uiShow()->showToast(tr::lng_purple_magic_not_saved(tr::now));
		}
	};

	check->setClickedCallback([=] {
		if (live()) {
			SyncMagicLookup(session, SyncMagicTrigger::Manual, true);
		}
	});
	open->setClickedCallback([=] {
		const auto frame = FrameOf(session);
		if (live() && frame.canOpen()) {
			OpenChat(session, *frame.chat, box->uiShow());
		}
	});
	listed->checkedChanges() | rpl::on_next([=](bool value) {
		const auto chat = live() ? SyncMagicChatOf(session) : nullptr;
		if (!chat) {
			return;
		}
		chat->list(value, crl::guard(box, [=](bool ok) {
			if (!ok) {
				box->uiShow()->showToast(tr::lng_purple_magic_not_saved(tr::now));
			}
			update();
		}));
	}, box->lifetime());
	choose->setClickedCallback([=] {
		if (!live()) {
			return;
		}
		box->uiShow()->showBox(Box(ChooseChatBox, session, live, [=](PeerToken chat) {
			if (const auto magic = live() ? SyncMagicChatOf(session) : nullptr) {
				magic->choose(chat, crl::guard(box, edited));
			}
		}));
	});
	useDefault->setClickedCallback([=] {
		if (const auto magic = live() ? SyncMagicChatOf(session) : nullptr) {
			magic->useDefault(crl::guard(box, edited));
		}
	});
	join->setClickedCallback([=] {
		if (!live()) {
			return;
		}
		box->uiShow()->showBox(Ui::MakeConfirmBox({
			.text = tr::lng_purple_magic_join_about(tr::now),
			.confirmed = crl::guard(box, [=](Fn<void()> close) {
				close();
				if (!live()) {
					return;
				}
				SyncMagicJoin(session, crl::guard(box, [=](bool ok) {
					if (!ok) {
						box->uiShow()->showToast(tr::lng_purple_magic_join_failed(tr::now));
					}
				}));
			}),
			.confirmText = tr::lng_purple_magic_join(),
		}));
	});

	SyncMagicChanges() | rpl::on_next([=](Main::Session *changed) {
		if (changed == session.get()) {
			update();
		}
	}, box->lifetime());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	update();
	if (live()) {
		SyncMagicLookup(session, SyncMagicTrigger::Screen, false);
	}
}

} // namespace Purple
