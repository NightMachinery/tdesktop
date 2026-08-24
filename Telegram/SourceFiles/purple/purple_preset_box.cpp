/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_preset_box.h"

#include "base/timer.h"
#include "core/file_utilities.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_engine.h"
#include "purple/purple_gate.h"
#include "purple/purple_schedule.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtGui/QKeySequence>

namespace Purple {
namespace {

// settings.toml holds a hotkey as Qt portable text, because that is what
// QKeySequence parses and what the docs can describe once for every platform.
// What is printed on the keyboard is another matter: on macOS Qt reads "Ctrl"
// as Command, so a label repeating the file would send someone to a key that
// does nothing.
[[nodiscard]] QString HotkeyText(const QString &keys) {
	const auto sequence = QKeySequence(keys, QKeySequence::PortableText);
	return sequence.isEmpty()
		? keys
		: sequence.toString(QKeySequence::NativeText);
}

// What a preset will do, in the words a chat list would use, so the choice can
// be made from the box rather than from memory of what was typed in the file.
[[nodiscard]] QString Summary(const Settings &settings, const QString &name) {
	const auto resolved = Resolve(settings, name);
	if (!resolved) {
		return u"does not resolve"_q;
	}
	auto through = 0;
	auto silenced = 0;
	auto gated = 0;
	for (const auto &list : resolved->lists) {
		if (list.show == ShowMode::Never) {
			continue;
		}
		++through;
		if (!list.notify) {
			++silenced;
		}
		// An entry that said nothing counts as gated too, because the per-kind
		// defaults gate everything except channels - calling it ungated would
		// be the wrong way round for most of a file.
		if (!list.show || ShowModeWatchesUnread(*list.show)) {
			++gated;
		}
	}
	const auto lists = [](int count) {
		return (count == 1) ? u"1 list"_q : u"%1 lists"_q.arg(count);
	};

	// Said the way the model now works: everything is hidden until a list lets
	// it through, so counting what it hides would be counting the whole account.
	auto parts = QStringList{
		through
			? u"lets through "_q + lists(through)
			: u"lets nothing through"_q,
	};
	if (silenced) {
		parts.push_back(u"silences "_q + lists(silenced));
	}
	if (gated) {
		parts.push_back(lists(gated) + u" only when unread"_q);
	}
	const auto all = ranges::any_of(resolved->folders, IsAllFolders);
	parts.push_back(all
		? u"every folder"_q
		: resolved->folders.empty()
		? u"no folders"_q
		: u"%1 folders"_q.arg(resolved->folders.size()));
	if (const auto views = int(resolved->views.size())) {
		parts.push_back((views == 1)
			? u"1 extra view"_q
			: u"%1 extra views"_q.arg(views));
	}
	return parts.join(u", "_q);
}

[[nodiscard]] QString RowText(const Settings &settings, const QString &name) {
	if (name == NormalPreset()) {
		// Normal is a bypass rather than a permissive preset, and saying so
		// here is the difference between "the one that allows everything" and
		// "the one that is not running".
		return u"Normal  -  stock Telegram Desktop"_q;
	}
	// The name the preset's own tab will carry, not the TOML key underneath it.
	// A picker offering "work" above a tab reading "Work" is one thing with two
	// names, and a preset that renamed its tab had no way to say so here at all.
	const auto preset = settings.preset(name);
	// The key beside the preset it presses, in the spelling the keyboard has -
	// a binding nothing displays is a binding nobody remembers, which is the
	// same reason the peek checkbox prints its own.
	const auto key = (preset && !preset->hotkey.isEmpty())
		? u"  (%1)"_q.arg(HotkeyText(preset->hotkey))
		: QString();
	return u"%1%2  -  %3"_q.arg(
		preset ? PresetTitle(*preset) : DefaultViewName(name),
		key,
		Summary(settings, name));
}

[[nodiscard]] QString HotkeyText() {
	return HotkeyText(ActiveSettings().peek.hotkey);
}

[[nodiscard]] QString Remaining(int seconds) {
	return u"%1:%2"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'));
}

// What the preset is doing at this moment, rather than what it says it will do.
// The rows above are read off the file and would look exactly the same if a
// list name were misspelled; this is read off the chat list, and it is the line
// that answers the question everyone actually has - is it working.
[[nodiscard]] QString EffectText(not_null<Main::Session*> session) {
	if (!Filtering()) {
		return QString();
	}
	const auto owner = &session->data();
	const auto total = owner->chatsList()->indexed()->size();
	const auto shown = owner->purpleViewList()->indexed()->size();
	auto result = u"Showing %1 of %2 loaded chats."_q.arg(shown).arg(total);

	// Named rather than numbered, because a count against a tab nobody can
	// identify is not worth the line it costs.
	const auto &views = ExtraViews();
	auto extras = QStringList();
	for (auto i = 0; i != int(views.size()); ++i) {
		extras.push_back(u"%1 %2"_q
			.arg(views[i].name)
			.arg(owner->purpleViewList(i + 1)->indexed()->size()));
	}
	if (!extras.isEmpty()) {
		result += '\n' + extras.join(u", "_q) + '.';
	}
	return result;
}

} // namespace

void PresetBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(rpl::single(u"Work Mode"_q));
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	auto padding = st::boxRowPadding;
	padding.setTop(st::boxOptionListSkip / 2);
	padding.setBottom(st::boxOptionListSkip / 2);

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"A preset decides, for every chat, whether it appears in the "
			"chat list and whether it may interrupt you."_q,
			st::boxLabel),
		st::boxRowPadding);

	const auto rows = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	// Directly under the choice it describes, and wrapped rather than hidden so
	// that switching to Normal takes the space with it instead of leaving a gap
	// where a number used to be.
	const auto effect = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				EffectText(session),
				st::boxDividerLabel),
			padding));
	effect->toggle(Filtering(), anim::type::instant);

	const auto problems = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	// Peek is otherwise a hotkey and nothing else, and a hotkey with no visible
	// affordance is a hotkey nobody remembers. This is where someone would
	// look for it, next to the preset it suspends. Under Normal it says why it
	// does nothing rather than sitting there greyed out with no explanation.
	//
	// A peek also ends on a clock, and nothing anywhere said when. The toast at
	// the start was the only warning, so chats reappearing and then going again
	// two minutes later had no visible cause. The countdown below runs only
	// while this box is open, which is the only time there is anyone to read it.
	const auto peekText = [] {
		const auto &resolved = ActiveResolved();
		if (resolved.normal) {
			return u"Peek - nothing is hidden under Normal"_q;
		} else if (!resolved.peeking) {
			return u"Peek - show what the preset hides (%1)"_q.arg(HotkeyText());
		}
		const auto deadline = CurrentState().peekDeadlineUnix;
		if (!deadline) {
			// auto_off = "off": it runs until it is turned off, and saying so
			// is the whole difference from a countdown that never moves.
			return u"Peeking - until you turn it off"_q;
		}
		const auto left = std::max(
			int(deadline - QDateTime::currentSecsSinceEpoch()),
			0);
		return u"Peeking - %1 left"_q.arg(Remaining(left));
	};
	const auto peek = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			peekText(),
			Peeking(),
			st::defaultCheckbox),
		padding);
	peek->setDisabled(ActiveResolved().normal);
	peek->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked != Peeking()) {
			TogglePeek();
		}
	}, peek->lifetime());

	// Ticks the countdown while one is running, and only then: a timer left
	// running behind a closed box would repaint a label nobody is looking at
	// once a second for as long as the app is up.
	const auto ticker = box->lifetime().make_state<base::Timer>();
	ticker->setCallback([=] { peek->setText(peekText()); });

	// Follows the engine rather than the click, so a peek started from the
	// hotkey, or ended by its own timer, moves the tick here too. Disabled
	// under Normal, which hides nothing there is anything to peek at.
	const auto refreshPeek = [=] {
		peek->setChecked(
			Peeking(),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
		peek->setDisabled(ActiveResolved().normal);
		peek->setText(peekText());
		if (Peeking() && CurrentState().peekDeadlineUnix) {
			ticker->callEach(crl::time(1000));
		} else {
			ticker->cancel();
		}
	};
	refreshPeek();

	ActiveChanges(
	) | rpl::on_next([=] {
		refreshPeek();

		// Deferred, because this fires from the gate and the chat lists it is
		// about are rebuilt by another subscriber to the same signal. Counting
		// here would count whatever the previous preset left behind.
		crl::on_main(effect, [=] {
			const auto text = EffectText(session);
			effect->entity()->setText(text);
			effect->toggle(!text.isEmpty(), anim::type::normal);
		});
	}, peek->lifetime());

	// Only worth a row when the file describes a schedule, since a switch that
	// holds off nothing explains nothing. Wrapped rather than hidden so the
	// space goes with it when a reload takes the last rule away.
	const auto paused = container->add(
		object_ptr<Ui::SlideWrap<Ui::Checkbox>>(
			container,
			object_ptr<Ui::Checkbox>(
				container,
				u"Pause the schedule"_q,
				SchedulePaused(),
				st::defaultCheckbox),
			padding));
	paused->toggle(ScheduleConfigured(), anim::type::instant);

	const auto pause = paused->entity();
	pause->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked != SchedulePaused()) {
			SetSchedulePaused(checked);
		}
	}, pause->lifetime());

	// state.toml is still hand-editable and the schedule writes to it too, so
	// the tick follows the file rather than only the click.
	StateChanges(
	) | rpl::on_next([=] {
		pause->setChecked(
			SchedulePaused(),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
	}, pause->lifetime());
	SettingsChanges(
	) | rpl::on_next([=] {
		paused->toggle(ScheduleConfigured(), anim::type::normal);
	}, paused->lifetime());

	// The path was already here, and already the answer to "where do I write
	// one". Clicking it is the part that was missing: everything this box can
	// do beyond choosing is done in that file, and until now finding it meant
	// retyping a path out of a label.
	auto written = TextWithEntities{ u"Presets are written in "_q };
	written.append(Ui::Text::Link(SettingsFilePath()));
	written.append(u", which reloads as you save it."_q);
	const auto path = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(std::move(written)),
			st::boxDividerLabel),
		st::boxRowPadding);
	path->setClickHandlerFilter([=](const auto &...) {
		// Reveal rather than open: the file has no registered handler on most
		// installs, and a TOML file opening in whatever claimed .toml is a
		// worse surprise than a Finder window.
		File::ShowInFolder(SettingsFilePath());
		return false;
	});

	// Indices into the radio group, so the callback can name what was picked.
	const auto names = box->lifetime().make_state<std::vector<QString>>();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>();

	group->setChangedCallback([=](int value) {
		if (value < 0 || value >= int(names->size())) {
			return;
		}
		const auto &name = (*names)[value];
		if (name == CurrentState().activePreset) {
			// Also the guard against re-entry: keeping the selection in step
			// with the state below sets the value that is already active, and
			// this is where that stops.
			return;
		}
		UpdateState([&](State &state) {
			state.activePreset = name;
			state.activeSource = PresetSource::Manual;
		});
	});

	const auto rebuild = [=] {
		rows->clear();
		names->clear();

		const auto &settings = ActiveSettings();
		names->push_back(NormalPreset());
		for (const auto &preset : settings.presets) {
			names->push_back(preset.name);
		}

		const auto active = CurrentState().activePreset;
		auto selected = -1;
		for (auto i = 0; i != int(names->size()); ++i) {
			const auto &name = (*names)[i];
			if (name == active) {
				selected = i;
			}
			const auto row = rows->add(
				object_ptr<Ui::Radiobutton>(
					rows,
					group,
					i,
					RowText(settings, name),
					st::defaultCheckbox),
				padding);

			// A checkbox is one elided line by default, and a summary saying
			// what a preset lets through, silences and gates does not fit in
			// one. Elided, the box was describing every preset as "lets
			// through 3 lists, silences 1 li..." - which is worse than no
			// summary, because it looks like the whole answer.
			row->setAllowTextLines(0);
		}
		if (selected >= 0) {
			group->setValue(selected);
		}

		problems->clear();
		const auto &found = SettingsProblems();
		auto errors = QStringList();
		if (selected < 0) {
			// Nothing is checked, which is the honest picture. Selecting Normal
			// instead would look tidier and would be a disaster: the callback
			// would fire and switch the user to Normal, unhiding every chat the
			// missing preset was hiding, over a typo mid-edit.
			errors.push_back(u"Error: the active preset '%1' is not in this "
				"file. The last resolution that worked is still in "
				"effect."_q.arg(active));
		}
		if (!found.error.isEmpty()) {
			errors.push_back(u"Error: "_q + found.error);
		}
		if (!errors.isEmpty()) {
			const auto label = problems->add(
				object_ptr<Ui::FlatLabel>(
					problems,
					errors.join('\n'),
					st::boxLabel),
				padding);

			// An error and a warning read identically in body text, and the
			// difference is the whole point: one means the file did not load,
			// the other means it loaded with something ignored. The "Error:"
			// prefix stays, so this does not rest on colour alone.
			label->setTextColorOverride(st::attentionButtonFg->c);
		}
		if (!found.warnings.empty()) {
			auto lines = QStringList();
			for (const auto &warning : found.warnings) {
				lines.push_back(u"Warning: "_q + warning);
			}
			problems->add(
				object_ptr<Ui::FlatLabel>(
					problems,
					lines.join('\n'),
					st::boxDividerLabel),
				padding);
		}
	};

	// Only settings.toml can change what the choices are. Rebuilding on a state
	// change instead would destroy the radio button whose click is still on the
	// stack, since choosing a preset is itself a state write.
	SettingsChanges(
	) | rpl::on_next([=] { rebuild(); }, box->lifetime());

	// A schedule or a focus change can move the active preset out from under an
	// open box, so the selection follows the state even though the rows do not.
	StateChanges(
	) | rpl::on_next([=] {
		const auto &active = CurrentState().activePreset;
		for (auto i = 0; i != int(names->size()); ++i) {
			if ((*names)[i] == active) {
				group->setValue(i);
				return;
			}
		}
	}, box->lifetime());

	rebuild();

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

rpl::producer<QString> PresetMenuLabel() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		ActiveChanges()
	) | rpl::map([] {
		const auto &resolved = ActiveResolved();
		return resolved.normal
			? u"Work Mode"_q
			: resolved.peeking
			// A peek reveals the chats a preset hides, which is the one time
			// the chat list stops matching the preset the label names.
			? u"Work Mode: %1 (peeking)"_q.arg(ViewName())
			: u"Work Mode: %1"_q.arg(ViewName());
	});
}

} // namespace Purple
