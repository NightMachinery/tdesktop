/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_preset_box.h"

#include "lang/lang_keys.h"
#include "purple/purple_config.h"
#include "purple/purple_engine.h"
#include "purple/purple_gate.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Purple {
namespace {

// What a preset will do, in the words a chat list would use, so the choice can
// be made from the box rather than from memory of what was typed in the file.
[[nodiscard]] QString Summary(const Settings &settings, const QString &name) {
	const auto resolved = Resolve(settings, name);
	if (!resolved) {
		return u"does not resolve"_q;
	}
	auto hidden = 0;
	auto silenced = 0;
	auto gated = 0;
	for (const auto &list : resolved->lists) {
		if (!list.show) {
			++hidden;
		} else {
			if (!list.notify) {
				++silenced;
			}
			if (list.groupsRequireMention) {
				++gated;
			}
		}
	}
	const auto lists = [](int count) {
		return (count == 1) ? u"1 list"_q : u"%1 lists"_q.arg(count);
	};
	auto parts = QStringList();
	if (hidden) {
		parts.push_back(u"hides "_q + lists(hidden));
	}
	if (silenced) {
		parts.push_back(u"silences "_q + lists(silenced));
	}
	if (gated) {
		parts.push_back(u"mentions only in "_q + lists(gated));
	}
	if (resolved->folders) {
		parts.push_back(resolved->folders->empty()
			? u"no folders"_q
			: u"%1 folders"_q.arg(resolved->folders->size()));
	}
	return parts.empty()
		? u"changes nothing"_q
		: parts.join(u", "_q);
}

[[nodiscard]] QString RowText(const Settings &settings, const QString &name) {
	if (name == NormalPreset()) {
		// Normal is a bypass rather than a permissive preset, and saying so
		// here is the difference between "the one that allows everything" and
		// "the one that is not running".
		return u"Normal  -  stock Telegram Desktop"_q;
	}
	return u"%1  -  %2"_q.arg(name, Summary(settings, name));
}

[[nodiscard]] QString ProblemsText(const Problems &problems) {
	auto lines = QStringList();
	if (!problems.error.isEmpty()) {
		lines.push_back(u"Error: "_q + problems.error);
	}
	for (const auto &warning : problems.warnings) {
		lines.push_back(u"Warning: "_q + warning);
	}
	return lines.join('\n');
}

} // namespace

void PresetBox(not_null<Ui::GenericBox*> box) {
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
	const auto problems = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	// Peek is otherwise a hotkey and nothing else, and a hotkey with no visible
	// affordance is a hotkey nobody remembers. This is where someone would
	// look for it, next to the preset it suspends. Under Normal it says why it
	// does nothing rather than sitting there greyed out with no explanation.
	const auto peekText = [] {
		return ActiveResolved().normal
			? u"Peek - nothing is hidden under Normal"_q
			: u"Peek - show what the preset hides (%1)"_q.arg(
				ActiveSettings().peek.hotkey);
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

	// Follows the engine rather than the click, so a peek started from the
	// hotkey, or ended by its own timer, moves the tick here too. Disabled
	// under Normal, which hides nothing there is anything to peek at.
	ActiveChanges(
	) | rpl::on_next([=] {
		peek->setChecked(
			Peeking(),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
		peek->setDisabled(ActiveResolved().normal);
		peek->setText(peekText());
	}, peek->lifetime());

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"Presets are written in "_q
				+ SettingsFilePath()
				+ u", which reloads as you save it."_q,
			st::boxDividerLabel),
		st::boxRowPadding);

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
			rows->add(
				object_ptr<Ui::Radiobutton>(
					rows,
					group,
					i,
					RowText(settings, name),
					st::defaultCheckbox),
				padding);
		}
		if (selected >= 0) {
			group->setValue(selected);
		}

		problems->clear();
		auto text = ProblemsText(SettingsProblems());
		if (selected < 0) {
			// Nothing is checked, which is the honest picture. Selecting Normal
			// instead would look tidier and would be a disaster: the callback
			// would fire and switch the user to Normal, unhiding every chat the
			// missing preset was hiding, over a typo mid-edit.
			text = u"The active preset '%1' is not in this file. The last "
				"resolution that worked is still in effect."_q.arg(active)
				+ (text.isEmpty() ? QString() : ('\n' + text));
		}
		if (!text.isEmpty()) {
			problems->add(
				object_ptr<Ui::FlatLabel>(problems, text, st::boxLabel),
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
			? u"Work Mode: %1 (peeking)"_q.arg(resolved.preset)
			: u"Work Mode: %1"_q.arg(resolved.preset);
	});
}

} // namespace Purple
