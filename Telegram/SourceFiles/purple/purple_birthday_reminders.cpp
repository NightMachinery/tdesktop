/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_birthday_reminders.h"

#include "base/flat_set.h"
#include "base/timer_rpl.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_controllers.h"
#include "boxes/star_gift_box.h"
#include "data/components/promo_suggestions.h"
#include "data/data_birthday.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_picker.h"
#include "purple/purple_suggestions.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"

#include <QtCore/QDate>

namespace Purple {
namespace {

enum class Audience {
	Nobody,
	Contacts,
	Everyone,
};

constexpr auto kAudience = "purple-birthday-audience";
constexpr auto kNever = "purple-birthday-never";
constexpr auto kHiddenDay = "purple-birthday-hidden-day";

rpl::event_stream<Main::Session*> Changes;

Audience ReadAudience(not_null<Main::Session*> session) {
	const auto value = session->local().readPref<QByteArray>(kAudience);
	return (value == "everyone")
		? Audience::Everyone
		: (value == "contacts")
		? Audience::Contacts
		: Audience::Nobody;
}

rpl::producer<QString> AudienceLabel(Audience audience) {
	switch (audience) {
	case Audience::Nobody: return tr::lng_edit_privacy_nobody();
	case Audience::Contacts: return tr::lng_edit_privacy_contacts();
	case Audience::Everyone: return tr::lng_edit_privacy_everyone();
	}
	Unexpected("Invalid birthday reminder audience.");
}

void WriteAudience(not_null<Main::Session*> session, Audience audience) {
	const auto value = (audience == Audience::Everyone)
		? "everyone"
		: (audience == Audience::Contacts)
		? "contacts"
		: "nobody";
	session->local().writePref<QByteArray>(kAudience, QByteArray(value));
	Changes.fire_copy(session.get());
}

base::flat_set<UserId> Excluded(not_null<Main::Session*> session) {
	auto result = base::flat_set<UserId>();
	const auto stored = session->local().readPref<QByteArray>(kNever);
	for (const auto &entry : stored.split(',')) {
		const auto id = entry.toULongLong();
		if (id) {
			result.emplace(id);
		}
	}
	return result;
}

void WriteExcluded(
		not_null<Main::Session*> session,
		const base::flat_set<UserId> &excluded) {
	auto encoded = QList<QByteArray>();
	for (const auto id : excluded) {
		encoded.push_back(QByteArray::number(id.bare));
	}
	session->local().writePref<QByteArray>(kNever, encoded.join(','));
	Changes.fire_copy(session.get());
}

QByteArray Today() {
	return QByteArray::number(QDate::currentDate().toJulianDay());
}

bool Enabled(not_null<Main::Session*> session) {
	return (ReadAudience(session) != Audience::Nobody)
		&& (session->local().readPref<QByteArray>(kHiddenDay) != Today());
}

bool Eligible(not_null<UserData*> user) {
	return !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& !user->isInaccessible()
		&& !user->isBlocked();
}

bool Matches(
		not_null<UserData*> user,
		Audience audience,
		const base::flat_set<UserId> &excluded) {
	return Eligible(user)
		&& (audience == Audience::Everyone || user->isContact())
		&& !excluded.contains(peerToUser(user->id))
		&& Data::IsBirthdayToday(user->birthday());
}

class ExclusionsController final : public ChatsListBoxController {
public:
	explicit ExclusionsController(
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
		delegate()->peerListSetTitle(tr::lng_purple_birthday_never());
		delegate()->peerListAddSelectedPeers(_selected);
	}

	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		const auto user = history->peer->asUser();
		return (user && Eligible(user))
			? std::make_unique<Row>(history)
			: nullptr;
	}

	const not_null<Main::Session*> _session;
	std::vector<not_null<PeerData*>> _selected;

};

void EditExclusions(not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	const auto original = Excluded(session);
	auto selected = std::vector<not_null<PeerData*>>();
	for (const auto id : original) {
		selected.push_back(session->data().user(id));
	}
	auto init = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), [=] {
			auto checked = base::flat_set<UserId>();
			for (const auto peer : box->collectSelectedRows()) {
				checked.insert(peerToUser(peer->id));
			}
			auto next = Excluded(session);
			for (const auto id : original) {
				if (!checked.contains(id)) {
					next.remove(id);
				}
			}
			for (const auto id : checked) {
				if (!original.contains(id)) {
					next.insert(id);
				}
			}
			box->closeBox();
			WriteExcluded(session, next);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	window->show(
		Box<PeerListBox>(
			std::make_unique<ExclusionsController>(session, std::move(selected)),
			std::move(init)),
		Ui::LayerOption::KeepOther);
}

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	const auto initial = ReadAudience(session);
	const auto group = std::make_shared<Ui::RadioenumGroup<Audience>>(initial);
	const auto layout = box->verticalLayout();
	box->setTitle(tr::lng_purple_birthday_reminders());
	Ui::AddSkip(layout);
	Ui::AddSubsectionTitle(layout, tr::lng_purple_birthday_who());
	for (const auto audience : {
		Audience::Everyone,
		Audience::Contacts,
		Audience::Nobody,
	}) {
		const auto row = layout->add(
			object_ptr<Ui::Radioenum<Audience>>(
				layout,
				group,
				audience,
				QString(),
				st::settingsPrivacyOption),
			st::settingsSendTypePadding + style::margins(
				-st::lineWidth,
				st::settingsPrivacySkipTop,
				0,
				0));
		AudienceLabel(audience) | rpl::on_next([=](const QString &label) {
			row->setText(label);
		}, row->lifetime());
	}
	Ui::AddDividerText(layout, tr::lng_purple_birthday_audience_about());
	auto neverCount = rpl::single(rpl::empty)
		| rpl::then(BirthdayReminderChanges(session))
		| rpl::map([=] { return QString::number(Excluded(session).size()); });
	::Settings::AddButtonWithLabel(
		layout,
		tr::lng_purple_birthday_never(),
		std::move(neverCount),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] { EditExclusions(window); });
	box->addButton(tr::lng_settings_save(), [=] {
		if (group->current() != initial) {
			WriteAudience(session, group->current());
		}
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

class RemindersController final : public PeerListController {
public:
	explicit RemindersController(not_null<Window::SessionController*> window)
	: _window(base::make_weak(window.get()))
	, _session(&window->session()) {
	}

	Main::Session &session() const override {
		return *_session;
	}

	void prepare() override {
		delegate()->peerListSetTitle(tr::lng_purple_birthday_reminders());
		refresh();
		BirthdayReminderChanges(_session) | rpl::on_next([=] {
			refresh();
		}, lifetime());
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		const auto window = _window.get();
		const auto peer = row->peer();
		if (!window || !PickerVisible(peer)) {
			return;
		}
		if (Ui::CanSendStarGiftTo(peer)) {
			Ui::ShowStarGiftBox(window, peer, true);
		} else {
			window->showPeerHistory(peer, Window::SectionShow::Way::Forward);
		}
	}

	void rowRightActionClicked(not_null<PeerListRow*> row) override {
		auto excluded = Excluded(_session);
		excluded.insert(peerToUser(row->peer()->id));
		WriteExcluded(_session, excluded);
	}

private:
	void refresh() {
		const auto ids = BirthdayReminderUsers(_session);
		const auto list = delegate();
		while (list->peerListFullRowsCount()) {
			list->peerListRemoveRow(list->peerListRowAt(0));
		}
		for (const auto id : ids) {
			auto row = std::make_unique<PeerListRowWithLink>(
				_session->data().user(id));
			row->setActionLink(tr::lng_purple_birthday_never(tr::now));
			list->peerListAppendRow(std::move(row));
		}
		setDescriptionText(ids.empty()
			? tr::lng_purple_birthday_empty(tr::now)
			: QString());
		list->peerListRefreshRows();
	}

	const base::weak_ptr<Window::SessionController> _window;
	const not_null<Main::Session*> _session;

};

} // namespace

void AddBirthdayReminderSettings(::Settings::Builder::SectionBuilder &builder) {
	const auto session = builder.session();
	const auto window = builder.controller();
	auto label = rpl::single(rpl::empty)
		| rpl::then(BirthdayReminderChanges(session))
		| rpl::map([=] { return AudienceLabel(ReadAudience(session)); })
		| rpl::flatten_latest();
	builder.addButton({
		.id = u"purple/birthday_reminders"_q,
		.title = tr::lng_purple_birthday_reminders(),
		.label = std::move(label),
		.onClick = [weak = window ? base::make_weak(window)
			: base::weak_ptr<Window::SessionController>()] {
			if (const auto live = weak.get()) {
				live->show(Box(SettingsBox, not_null{ live }));
			}
		},
		.keywords = { u"birthday"_q, u"reminder"_q, u"notification"_q },
	});
}

rpl::producer<> BirthdayReminderChanges(not_null<Main::Session*> session) {
	auto rules = Changes.events() | rpl::filter([=](Main::Session *changed) {
		return changed == session.get();
	}) | rpl::to_empty;
	using Flag = Data::PeerUpdate::Flag;
	auto people = session->changes().peerUpdates(
		Flag::Birthday | Flag::IsContact | Flag::IsBlocked
	) | rpl::to_empty;
	auto day = rpl::single() | rpl::then(base::timer_each(crl::time(60'000)))
		| rpl::map(Today)
		| rpl::distinct_until_changed()
		| rpl::skip(1)
		| rpl::to_empty;
	return WithPresetChanges(rpl::merge(
		std::move(rules),
		std::move(people),
		std::move(day)));
}

bool BirthdayReminderFor(not_null<UserData*> user) {
	const auto session = &user->session();
	return Enabled(session)
		&& Matches(user, ReadAudience(session), Excluded(session));
}

rpl::producer<bool> BirthdayReminderValue(not_null<UserData*> user) {
	return rpl::single()
		| rpl::then(BirthdayReminderChanges(&user->session()))
		| rpl::map([=] { return BirthdayReminderFor(user); })
		| rpl::distinct_until_changed();
}

bool BirthdayRemindersAvailable(not_null<Main::Session*> session) {
	return Enabled(session)
		&& (!session->promoSuggestions().knownBirthdaysToday()
			|| !BirthdayReminderUsers(session).empty());
}

std::vector<UserId> BirthdayReminderUsers(not_null<Main::Session*> session) {
	if (!Enabled(session)) {
		return {};
	}
	const auto audience = ReadAudience(session);
	const auto excluded = Excluded(session);
	auto people = std::vector<UserId>();
	session->data().enumerateUsers([&](not_null<UserData*> user) {
		if (Matches(user, audience, excluded) && PickerVisible(user)) {
			people.push_back(peerToUser(user->id));
		}
	});
	return people;
}

void HideBirthdayRemindersToday(not_null<Main::Session*> session) {
	session->local().writePref<QByteArray>(kHiddenDay, Today());
	Changes.fire_copy(session.get());
}

void ShowBirthdayReminders(not_null<Window::SessionController*> controller) {
	controller->show(Box<PeerListBox>(
		std::make_unique<RemindersController>(controller),
		[](not_null<PeerListBox*> box) {
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
}

} // namespace Purple
