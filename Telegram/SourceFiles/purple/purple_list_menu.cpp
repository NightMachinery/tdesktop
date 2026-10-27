/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_list_menu.h"

#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_preset_box.h"
#include "ui/layers/generic_box.h"
#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/menu/menu_multiline_action.h"
#include "ui/widgets/popup_menu.h"

#include <array>

#include <QtCore/QPointer>
#include "styles/style_media_player.h" // mediaPlayerMenuCheck
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h" // defaultFlatLabel

namespace Purple {
namespace {

// What the "until" submenus offer. Deliberately coarse: these are decisions
// about the rest of an afternoon, and a minute-accurate picker would be a
// worse answer to the question than four good guesses.
constexpr auto kOverrideSpans = std::array{
	std::pair{ "30 minutes", 30 * 60 },
	std::pair{ "2 hours", 2 * 3600 },
	std::pair{ "8 hours", 8 * 3600 },
	std::pair{ "24 hours", 24 * 3600 },
};

[[nodiscard]] QString DisplayTitle(const List &list) {
	return list.title.isEmpty() ? list.name : list.title;
}

// The fork's one input field. Deliberately one field: a list is a name and a
// set of members, the members are ticked in from this same menu, and everything
// a preset does with a list is written in settings.toml beside the preset.
void NewListBox(
		not_null<Ui::GenericBox*> box,
		Fn<void(QString)> create,
		Fn<bool()> originCurrent) {
	const auto editor = CaptureAppWideSettingsEditor(box);
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	box->setTitle(rpl::single(u"New list"_q));

	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Name"_q)));
	box->setFocusCallback([=] { field->setFocusFast(); });

	const auto submit = [=] {
		const auto name = field->getLastText().trimmed();
		if (name.isEmpty() || name.startsWith('*')) {
			// The two the parser refuses. Everything else it will take, and
			// the splice verifies the result before anything is written.
			field->showError();
			return;
		}
		const auto result = WriteSettingsFromEditor(editor, [=](const QString &text) {
			return AddList(text, SettingsFilePath(), name, name);
		}, u"a new list"_q, [=] { return weak && originCurrent(); });
		if (!weak) { return; }
		if (!result.ok) {
			box->uiShow()->showToast(result.error);
			return;
		}
		create(name);
		if (weak) { box->closeBox(); }
	};
	field->submits() | rpl::on_next(submit, field->lifetime());

	box->addButton(rpl::single(u"Create"_q), submit);
	box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
}

} // namespace

bool HasCustomLists() {
	return !ActiveSettings().lists.empty();
}

void FillListsMenu(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	const auto id = IdOf(peer);
	if (!id) {
		return;
	}
	const auto session = &peer->session();
	const auto weakSession = base::make_weak(session);
	const auto peerId = peer->id;

	// Which entry is deciding this chat right now. It is the answer to the
	// question that brings anyone here - "why is this hidden?" - and no entry
	// at all is as real an answer as any, because a preset names what gets
	// through and silence about a chat is a decision too.
	const auto summary = AccountListsSummary(session);
	if (!Filtering(session) && !summary.isEmpty()) {
		auto label = base::make_unique_q<Ui::Menu::MultilineAction>(menu->menu(), menu->st().menu,
			st::defaultFlatLabel, QPoint(menu->st().menu.itemPadding.left(), menu->st().menu.itemPadding.top()),
			tr::marked(summary), &st::menuIconInfo);
		menu->addAction(std::move(label));
		menu->addSeparator();
	}
	if (Filtering(session)) {
		// What the entry wanted, and what actually happened - which are not the
		// same thing for a chat a folder pulled back in. The state has to be
		// the second: a line reading "hidden" over a chat sitting in the list
		// is worse than no line at all.
		const auto verdict = VisibleFor(peer);
		const auto history = peer->owner().historyLoaded(peer);
		const auto hidden = history && history->purpleHiddenFromView();

		// A chat held back by a mode is a different answer from one hidden
		// outright: it says what would bring it back, which is the only useful
		// thing to say to somebody asking why they cannot see it.
		const auto waiting = [&] {
			switch (verdict.show) {
			case ShowMode::Message: return u"hidden until a message"_q;
			case ShowMode::MessageOrReaction:
				return u"hidden until a message or reaction"_q;
			case ShowMode::Mention: return u"hidden until a mention"_q;
			default: break;
			}
			return u"hidden"_q;
		};
		const auto state = hidden
			? waiting()
			: verdict.notify
			? u"shown"_q
			: u"silenced"_q;
		const auto folder = (!hidden && verdict.show == ShowMode::Never)
			? u", shown by a folder"_q
			: QString();
		const auto effective = ListFor(peer);
		const auto list = effective
			? LookupList(AccountSettings(session), ActiveResolved(), effective->list)
			: nullptr;
		const auto where = list
			? u"In '%1'"_q.arg(DisplayTitle(*list))
			: u"In no list '%1' names"_q.arg(ViewName());

		// A wrapping label rather than an ordinary row. An ordinary one elides
		// at the menu's widthMax, which cut this off after about thirty-five
		// characters - and the tail is where the answer is, so what survived
		// was the half nobody came here to read.
		//
		// No FixAmpersandInAction on this one, unlike the list rows below: a
		// label draws '&' as itself, and only an action's text reads it as a
		// mnemonic.
		const auto &st = menu->st().menu;
		auto label = base::make_unique_q<Ui::Menu::MultilineAction>(
			menu->menu(),
			st,
			st::defaultFlatLabel,
			// Where an ordinary row puts its text, so this lines up with the
			// list rows under it and clears the icon on its left.
			QPoint(st.itemPadding.left(), st.itemPadding.top()),
			tr::marked(u"%1: %2%3"_q.arg(where, state, folder)
				+ (summary.isEmpty() ? QString() : '\n' + summary)),
			&st::menuIconInfo);
		const auto action = menu->addAction(std::move(label));
		QObject::connect(action, &QAction::triggered, action, [=] {
			if (const auto live = weakSession.get()) {
				show->showBox(Box(PresetBox, not_null{ live }));
			}
		});
		menu->addSeparator();
	}

	for (const auto &list : AccountSettings(session).lists) {
		const auto name = list.name;
		auto captureError = QString();
		const auto edit = AccountListEditFor(session, name, &captureError);
		const auto &members = edit ? edit->members : list.members;
		const auto member = std::find(members.begin(), members.end(), id) != members.end();
		// Every list, including one that matches by kind: adding a chat there
		// writes an explicit member id, which is how you pull one chat out of
		// a rule that would otherwise have swept it up somewhere else.
		menu->addAction(
			Ui::Text::FixAmpersandInAction(DisplayTitle(list)),
			[=] {
				// Written before the write, because a successful one reloads
				// settings.toml and rebuilds every chat list from inside this
				// callback - with the menu that owns it still open.
				menu->hideMenu();

				const auto live = weakSession.get();
				if (!live) {
					if (show && *show) { show->showToast(tr::lng_purple_wait_account_changed(tr::now)); }
					return;
				}
				const auto title = TitleResolver(not_null{ live });
				const auto notice = edit ? AccountListEditNotice(not_null{ live }, *edit, !member) : QString();
				const auto result = edit ? SetAccountListMember(not_null{ live }, *edit, id, !member, title)
					: AccountSettingsEditResult{ .error = !captureError.isEmpty() ? captureError
						: u"List editing is unavailable for the opening account. Reopen the menu."_q };
				if (!result.ok) {
					if (show && *show) {
						ShowAccountSettingsRefusal(show, result.error, SessionAccountKey(not_null{ live }));
					}
				} else if (!notice.isEmpty() && show && *show) {
					show->showToast(notice);
				}
			},
			(member ? &st::mediaPlayerMenuCheck : nullptr));
	}

	for (const auto &target : AccountListDeletes(session)) {
		menu->addAction(tr::lng_purple_lists_delete_named(tr::now, lt_account, session->user()->name(),
			lt_list, target.list), [=] {
			menu->hideMenu();
			if (const auto live = weakSession.get()) {
				show->showBox(Box(DeleteAccountListMembersBox, not_null{ live }, target));
			}
		});
	}

	// The "until" decisions, one submenu each. Set apart from the lists above,
	// because a list is a standing rule and these are a thing you are doing
	// this afternoon.
	if (Filtering(session)) {
		const auto current = OverrideFor(peer);
		const auto fill = [&](
				const QString &text,
				OverrideKind kind,
				const style::icon *icon) {
			auto submenu = std::make_unique<Ui::PopupMenu>(
				menu.get(),
				st::popupMenuWithIcons);
			for (const auto &span : kOverrideSpans) {
				const auto seconds = span.second;
				submenu->addAction(QString::fromLatin1(span.first), [=] {
					// Hidden first, as everywhere else here: the write reloads
					// state.toml and rebuilds the chat lists from inside this
					// callback, with the menu that owns it still open.
					menu->hideMenu();
					if (const auto live = weakSession.get()) {
						if (const auto currentPeer = live->data().peerLoaded(peerId)) {
							SetOverride(currentPeer, kind, seconds);
						}
					}
				});
			}
			menu->addAction(text, std::move(submenu), icon);
		};
		menu->addSeparator();
		fill(u"Show until..."_q, OverrideKind::Show, &st::menuIconShowInChat);
		fill(u"Hide until..."_q, OverrideKind::Hide, &st::menuIconClear);
		fill(u"Notify until..."_q, OverrideKind::Notify, &st::menuIconUnmute);
		if (current) {
			// Only offered when there is one, so the menu does not carry a
			// permanently greyed-out row for a feature most chats never use.
			menu->addAction(u"Cancel '%1 until'"_q.arg(
				OverrideKindName(*current)), [=] {
				menu->hideMenu();
				if (const auto live = weakSession.get()) {
					if (const auto currentPeer = live->data().peerLoaded(peerId)) {
						ClearOverride(currentPeer);
					}
				}
			}, &st::menuIconCancel);
		}
	}

	menu->addSeparator();
	menu->addAction(u"New list..."_q, [=] {
		// Same reason the rows above hide first: a successful write reloads
		// settings.toml and rebuilds every chat list from inside this callback.
		menu->hideMenu();
		if (!weakSession) {
			return;
		}
		const auto scope = AccountSettingsScopeCaption(session, std::nullopt);
		show->showBox(Box(NewListBox, [=](QString name) {
			if (!weakSession) {
				return;
			}
			// Said out loud, because the list is real and does nothing: no
			// preset names it yet, and there is no way to guess which one
			// should. Ticking chats into it works from this menu immediately.
			show->showToast(u"List '%1' created. Add it to a preset's "
				"list_order in %2 to make it do anything."_q.arg(
					name,
					SettingsFilePath()) + (scope.isEmpty() ? QString() : '\n' + scope));
		}, Fn<bool()>([=] {
			const auto live = weakSession.get();
			return live && !live->account().loggingOut() && live->account().maybeSession() == live;
		})));
	}, &st::menuIconAdd);
}

void AddListsSubmenu(
		const Ui::Menu::MenuCallback &add,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	if ((!HasCustomLists() && !Filtering()) || !IdOf(peer)) {
		// Left out entirely rather than opening on an empty submenu, so an
		// unconfigured fork's menus are untouched.
		return;
	}
	// Set apart above and below. Everything around it acts on the chat itself;
	// this one acts on the preset that is deciding the chat, and reaching for
	// it by mistake edits a file. No separator style named: the chat list
	// builds its menu with popupMenuExpandedSeparator, so this is the thick
	// divider there and the ordinary hairline elsewhere, which is right in
	// both.
	add(Ui::Menu::MenuCallback::Args{ .isSeparator = true });
	add(Ui::Menu::MenuCallback::Args{
		.text = u"Work Mode"_q,
		.handler = nullptr,
		.icon = &st::menuIconAddToFolder,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			FillListsMenu(menu, show, peer);
		},
		.submenuSt = &st::popupMenuWithIcons,
	});
	add(Ui::Menu::MenuCallback::Args{ .isSeparator = true });
}

} // namespace Purple
