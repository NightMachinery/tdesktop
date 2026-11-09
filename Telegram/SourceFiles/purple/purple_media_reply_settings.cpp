#include "purple/purple_media_reply_settings.h"

#include "base/weak_ptr.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_media_reply_splice.h"
#include "purple/purple_media_reply_adapter.h"
#include "purple/purple_media_reply_presentation.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>

namespace Purple {
namespace {

QString Mode(MediaReplyMode value) {
	switch (value) {
	case MediaReplyMode::Own: return tr::lng_purple_media_reply_own(tr::now);
	case MediaReplyMode::Received: return tr::lng_purple_media_reply_received(tr::now);
	case MediaReplyMode::Both: return tr::lng_purple_media_reply_both(tr::now);
	case MediaReplyMode::Off: return tr::lng_purple_media_reply_off(tr::now);
	}
	return QString();
}

QString StatusText(const MediaReplyStatus &status) {
	auto reason = QString();
	switch (status.reason) {
	case MediaReplyStatusReason::JournalFull: reason = tr::lng_purple_media_reply_journal_full(tr::now); break;
	case MediaReplyStatusReason::JournalUnreadable: reason = tr::lng_purple_media_reply_journal_unreadable(tr::now); break;
	case MediaReplyStatusReason::LockUnavailable: reason = tr::lng_purple_media_reply_lock(tr::now); break;
	case MediaReplyStatusReason::StorageRefused: reason = tr::lng_purple_media_reply_storage(tr::now); break;
	case MediaReplyStatusReason::Permission: reason = tr::lng_purple_media_reply_permission(tr::now); break;
	case MediaReplyStatusReason::Flood: reason = tr::lng_purple_media_reply_flood(tr::now); break;
	case MediaReplyStatusReason::ApprovalRequired: reason = tr::lng_purple_media_reply_approval(tr::now); break;
	case MediaReplyStatusReason::Uncertain: reason = tr::lng_purple_media_reply_uncertain(tr::now); break;
	case MediaReplyStatusReason::Cancelled: reason = tr::lng_purple_media_reply_cancelled(tr::now); break;
	case MediaReplyStatusReason::None: reason = status.disabled ? tr::lng_purple_media_reply_disabled(tr::now)
		: status.inFlight ? tr::lng_purple_media_reply_inflight(tr::now) : tr::lng_purple_media_reply_ready(tr::now); break;
	}
	return (status.disabled && status.reason != MediaReplyStatusReason::None ? tr::lng_purple_media_reply_disabled(tr::now) + '\n' : QString())
		+ reason + '\n' + tr::lng_purple_media_reply_capacity(tr::now) + u": "_q
		+ (status.reserved < 0 ? tr::lng_purple_media_reply_not_read(tr::now) : QString::number(status.reserved))
		+ u" / 4096; "_q + QString::number(status.inFlight) + u" / 8"_q;
}

PeerToken Token(not_null<PeerData*> peer) {
	if (const auto id = peerToUser(peer->id)) return {PeerTokenKind::User, id.bare};
	if (const auto id = peerToChat(peer->id)) return {PeerTokenKind::Chat, id.bare};
	return {PeerTokenKind::Channel, peerToChannel(peer->id).bare};
}

void SettingsBox(not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller, QString row);

void AddButton(not_null<Ui::GenericBox*> box, const QString &label, Fn<void()> clicked) {
	::Settings::AddButtonWithIcon(box->verticalLayout(), rpl::single(label),
		st::settingsButtonNoIcon)->setClickedCallback(std::move(clicked));
}

void EditTextBox(not_null<Ui::GenericBox*> box,
		QString title, QString value, Fn<bool(QString)> save) {
	box->setTitle(rpl::single(title));
	const auto field = box->addRow(object_ptr<Ui::InputField>(box, st::defaultInputField,
		Ui::InputField::Mode::SingleLine, rpl::single(title), value));
	box->setFocusCallback([=] { field->setFocusFast(); });
	box->addButton(tr::lng_settings_save(), [=] {
		if (save(field->getLastText())) box->closeBox();
		else field->showError();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void SettingsBox(not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller, QString row) {
	box->setTitle(tr::lng_purple_media_reply());
	const auto weakController = base::make_weak(controller.get());
	const auto weakSession = base::make_weak(&controller->session());
	const auto weakBox = QPointer<Ui::GenericBox>(box.get());
	const auto target = AccountMediaReplyEditTarget(&controller->session());
	if (!target) {
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_media_reply_unavailable(tr::now), st::boxDividerLabel));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	const auto current = [=] {
		const auto window = weakController.get();
		const auto session = weakSession.get();
		return weakBox && window && session && &window->session() == session;
	};
	const auto save = [=](QString scope, QString key, QJsonValue value) {
		if (!current()) return false;
		const auto result = WriteAccountMediaReply(weakSession.get(), *target, scope, key, value, current);
		if (!result.ok) { box->uiShow()->showToast(result.error); return false; }
		box->closeBox();
		if (const auto window = weakController.get()) window->show(Box(SettingsBox, window, row));
		return true;
	};
	const auto open = [=](QString scope) {
		if (current()) controller->show(Box(SettingsBox, controller, scope));
	};
	const auto &settings = AccountSettings(&controller->session());
	const auto &policy = settings.mediaReply;
	const auto status = MediaReplyExecutionStatus(&controller->session());
	AddButton(box, tr::lng_purple_media_reply_status(tr::now) + u": "_q + StatusText(status), [=] {
		if (!current()) return;
		controller->show(Box([=](not_null<Ui::GenericBox*> notice) {
			notice->setTitle(tr::lng_purple_media_reply_status());
			notice->addRow(object_ptr<Ui::FlatLabel>(notice, StatusText(MediaReplyExecutionStatus(weakSession.get()))
				+ u"\n\n"_q + tr::lng_purple_media_reply_status_info(tr::now), st::boxDividerLabel));
			notice->addButton(tr::lng_close(), [=] { notice->closeBox(); });
		}));
	});
	if (row.isEmpty()) {
		AddButton(box, tr::lng_purple_media_reply_text(tr::now) + u": "_q + policy.text, [=] {
			if (current()) controller->show(Box(EditTextBox,
				tr::lng_purple_media_reply_text(tr::now), policy.text,
				Fn<bool(QString)>([=](QString text) { return save({}, u"text"_q, text); })));
		});
		const auto names = std::array{u"private"_q, u"groups"_q, u"channels"_q};
		const auto labels = std::array{tr::lng_purple_media_reply_private(tr::now),
			tr::lng_purple_media_reply_groups(tr::now), tr::lng_purple_media_reply_channels(tr::now)};
		for (auto i = 0; i != 3; ++i) AddButton(box, labels[i], [=] { open(names[i]); });
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_media_reply_info(tr::now), st::boxDividerLabel));
		auto priority = 0;
		for (const auto &set : policy.sets) {
			AddButton(box, QString::number(++priority) + u". "_q + set.name,
				[=] { open(u"sets."_q + set.name); });
		}
		AddButton(box, tr::lng_purple_media_reply_add(tr::now), [=] {
			if (current()) controller->show(Box(EditTextBox,
				tr::lng_purple_media_reply_name(tr::now), QString(), Fn<bool(QString)>([=](QString name) {
					for (const auto &set : policy.sets) if (set.name == name) return false;
					return save(u"sets."_q + name, u"peers"_q, QJsonArray());
				})));
		});
	} else {
		auto shown = MediaReplyRow();
		auto peers = QJsonArray();
		if (row.startsWith(u"sets."_q)) {
			for (const auto &set : policy.sets) if (set.name == row.mid(5)) {
				shown = set.row;
				for (const auto &peer : set.peers) peers.push_back(PeerTokenText(peer).value());
			}
		} else shown = policy.defaults[row == u"groups"_q ? 1 : row == u"channels"_q ? 2 : 0];
		const auto choose = [=](QString key, std::optional<MediaReplyMode> initial) {
			if (!current()) return;
			controller->show(Box([=](not_null<Ui::GenericBox*> chooser) {
				auto options = std::vector<QString>{Mode(MediaReplyMode::Off), Mode(MediaReplyMode::Own),
					Mode(MediaReplyMode::Received), Mode(MediaReplyMode::Both)};
				if (row.startsWith(u"sets."_q)) options.push_back(tr::lng_purple_media_reply_inherit(tr::now));
				SingleChoiceBox(chooser, {
					.title = tr::lng_purple_media_reply(),
					.options = options,
					.initialSelection = MediaReplyModeSelection(initial, row.startsWith(u"sets."_q)),
					.callback = [=](int index) { save(row, key,
						index == 4 ? QJsonValue() : QJsonValue(MediaReplyModeName(MediaReplyMode(index)))); },
				});
			}));
		};
		AddButton(box, tr::lng_purple_media_reply_voice(tr::now) + u": "_q
			+ (shown.voice || !row.startsWith(u"sets."_q) ? Mode(shown.voice.value_or(MediaReplyMode::Off))
				: tr::lng_purple_media_reply_inherit(tr::now)), [=] {
			choose(u"voice"_q, shown.voice);
		});
		if (row.startsWith(u"sets."_q)) {
			AddButton(box, tr::lng_purple_media_reply_follow(tr::now) + u": "_q
				+ (!shown.roundFollow ? tr::lng_purple_media_reply_inherit(tr::now)
					: *shown.roundFollow ? tr::lng_purple_media_reply_follow(tr::now) : tr::lng_purple_media_reply_independent(tr::now)), [=] {
				if (!current()) return;
				controller->show(Box([=](not_null<Ui::GenericBox*> chooser) {
					SingleChoiceBox(chooser, {.title = tr::lng_purple_media_reply_follow(),
						.options = {tr::lng_purple_media_reply_follow(tr::now), tr::lng_purple_media_reply_independent(tr::now), tr::lng_purple_media_reply_inherit(tr::now)},
						.initialSelection = !shown.roundFollow ? 2 : *shown.roundFollow ? 0 : 1,
						.callback = [=](int index) { save(row, u"round_follow_voice"_q, index == 2 ? QJsonValue() : QJsonValue(index == 0)); }});
				}));
			});
			auto defaults = tr::lng_purple_media_reply_defaults(tr::now);
			const auto labels = std::array{tr::lng_purple_media_reply_private(tr::now), tr::lng_purple_media_reply_groups(tr::now), tr::lng_purple_media_reply_channels(tr::now)};
			const auto kinds = std::array{ChatKind::Private, ChatKind::Group, ChatKind::Channel};
			const auto tokens = std::array{PeerToken{PeerTokenKind::User, 0}, PeerToken{PeerTokenKind::Chat, 0}, PeerToken{PeerTokenKind::Channel, 0}};
			for (auto index = 0; index != 3; ++index) {
				const auto value = ResolveMediaReply(settings, tokens[index], kinds[index]);
				defaults += '\n' + labels[index] + u": "_q + Mode(value.voice) + u" / "_q + Mode(value.round)
					+ u"; "_q + (value.roundFollow ? tr::lng_purple_media_reply_follow(tr::now) : tr::lng_purple_media_reply_independent(tr::now));
			}
			box->addRow(object_ptr<Ui::FlatLabel>(box, defaults, st::boxDividerLabel));
		} else {
			const auto follow = box->addRow(object_ptr<Ui::Checkbox>(box,
				tr::lng_purple_media_reply_follow(tr::now), shown.roundFollow.value_or(true)));
			follow->checkedChanges() | rpl::on_next([=](bool checked) { save(row, u"round_follow_voice"_q, checked); }, follow->lifetime());
		}
		const auto disabled = MediaReplyRoundDisabled(settings, shown, row.startsWith(u"sets."_q));
		const auto selection = MediaReplyModeSelection(shown.round, row.startsWith(u"sets."_q));
		const auto round = ::Settings::AddButtonWithIcon(box->verticalLayout(),
			rpl::single(tr::lng_purple_media_reply_round(tr::now) + u": "_q
				+ (disabled ? tr::lng_purple_media_reply_follow(tr::now) : selection == 4
					? tr::lng_purple_media_reply_inherit(tr::now) : Mode(MediaReplyMode(selection)))), st::settingsButtonNoIcon);
		round->setDisabled(disabled);
		round->setClickedCallback([=] { choose(u"round"_q, shown.round); });
		if (row.startsWith(u"sets."_q)) {
			AddButton(box, tr::lng_purple_media_reply_select(tr::now), [=] {
				if (!current()) return;
				auto picker = std::make_unique<ChooseRecipientBoxController>(weakSession.get(),
					[=](not_null<Data::Thread*> thread) {
						if (!current()) return;
						auto next = peers;
						const auto token = QJsonValue(PeerTokenText(Token(thread->peer())).value());
						if (!next.contains(token)) next.push_back(token);
						save(row, u"peers"_q, next);
					});
				controller->show(Box<PeerListBox>(std::move(picker), [](not_null<PeerListBox*> box) {
					box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
				}));
			});
			for (const auto value : peers) {
				const auto token = ParsePeerToken(value.toString());
				const auto id = token->kind == PeerTokenKind::User ? peerFromUser(UserId(token->id))
					: token->kind == PeerTokenKind::Chat ? peerFromChat(ChatId(token->id))
					: peerFromChannel(ChannelId(token->id));
				const auto peer = controller->session().data().peerLoaded(id);
				auto title = peer ? peer->name() : tr::lng_purple_media_reply_unknown_chat(tr::now);
				if (peer) {
					const auto effective = ResolveMediaReply(AccountSettings(&controller->session()), *token, KindOf(peer));
					title += u": "_q + Mode(effective.voice) + u" / "_q + Mode(effective.round);
				}
				AddButton(box, title, [=] {
					auto next = QJsonArray();
					for (const auto previous : peers) if (previous != value) next.push_back(previous);
					save(row, u"peers"_q, next);
				});
			}
		}
	}
	box->addRow(object_ptr<Ui::FlatLabel>(box,
		tr::lng_purple_media_reply_priority(tr::now), st::boxDividerLabel));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void BuildMediaReplySettings(::Settings::Builder::SectionBuilder &builder) {
	const auto weak = base::make_weak(builder.controller());
	const auto weakSession = base::make_weak(builder.session().get());
	builder.addButton({
		.id = u"advanced/purple_media_reply"_q,
		.title = tr::lng_purple_media_reply(),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			const auto controller = weak.get();
			const auto session = weakSession.get();
			if (controller && session && &controller->session() == session) {
				controller->show(Box(SettingsBox, controller, QString()));
			}
		},
	});
}

} // namespace Purple
