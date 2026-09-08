/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_schedule_box.h"

#include "base/unixtime.h"
#include "lang/lang_keys.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_engine.h"
#include "purple/purple_schedule.h"
#include "settings/settings_common.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

namespace Purple {
namespace {

// A day, which is what "pause until" almost always means. The picker opens on
// it rather than on this minute, because a default of "now" is a default that
// is always wrong.
constexpr auto kPauseDefault = 24 * 3600;

// The scopes a ruleset can be written for, in the order the box offers them:
// widest first, so the list reads as a funnel rather than as an alphabet. The
// device ids come after these, out of the file and out of this machine.
constexpr auto kScopes = std::array{
	std::pair{ "any", "Any device" },
	std::pair{ "desktop", "Any desktop" },
	std::pair{ "mobile", "Any phone" },
	std::pair{ "android", "Android" },
	std::pair{ "ios", "iOS" },
	std::pair{ "macos", "macOS" },
	std::pair{ "windows", "Windows" },
	std::pair{ "linux", "Linux" },
};

[[nodiscard]] style::margins RowPadding() {
	auto result = st::boxRowPadding;
	result.setTop(st::boxOptionListSkip / 2);
	result.setBottom(st::boxOptionListSkip / 2);
	return result;
}

// The ruleset a name addresses, with the empty name meaning the flat
// [[schedule.rules]] array - which the parser hands back as a ruleset like any
// other, so this is a lookup and not a special case. Null when the file no
// longer holds it, which is what an open box discovers after an outside edit.
[[nodiscard]] const ScheduleRuleset *FindRuleset(
		const Settings &settings,
		const QString &name) {
	for (const auto &ruleset : settings.schedule.rulesets) {
		if (name.isEmpty()
			? ruleset.implicit()
			: !ruleset.name.compare(name, Qt::CaseInsensitive)) {
			return &ruleset;
		}
	}
	return nullptr;
}

[[nodiscard]] QString ScopeText(const QString &device) {
	const auto trimmed = device.trimmed();
	if (trimmed.isEmpty()) {
		return QString::fromLatin1(kScopes.front().second);
	}
	for (const auto &[value, text] : kScopes) {
		if (!trimmed.compare(QLatin1String(value), Qt::CaseInsensitive)) {
			return QString::fromLatin1(text);
		}
	}
	// Anything the list above does not hold is a device id, which is the
	// parser's rule too: the platforms are a closed list, the devices are not.
	const auto label = DeviceLabelText(trimmed);
	return trimmed.compare(ThisDevice().id, Qt::CaseInsensitive)
		? label
		: u"This device (%1)"_q.arg(label);
}

[[nodiscard]] QString ModeText(RulesetMode mode) {
	switch (mode) {
	case RulesetMode::Disabled: return u"Off"_q;
	case RulesetMode::Always: return u"Always on"_q;
	default: break;
	}
	return u"On"_q;
}

// "Mon-Fri", "Sat, Sun", "Every day". Runs are collapsed because a rule for
// the working week is the common one and spelling it out five times is five
// times the reading for the same fact.
[[nodiscard]] QString DaysText(const std::vector<int> &days) {
	auto sorted = days;
	ranges::sort(sorted);
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	if (sorted.empty()) {
		return u"No days"_q;
	} else if (sorted.size() == 7) {
		return u"Every day"_q;
	}
	const auto name = [](int day) {
		// QLocale rather than the core's WeekdayName(), which answers "mon"
		// because that is the spelling settings.toml uses. Right for a file,
		// wrong for a sentence.
		return QLocale().dayName(day, QLocale::ShortFormat);
	};
	auto parts = QStringList();
	for (auto i = 0; i != int(sorted.size());) {
		auto last = i;
		while (last + 1 != int(sorted.size())
			&& sorted[last + 1] == sorted[last] + 1) {
			++last;
		}
		parts.push_back((last > i + 1)
			? u"%1-%2"_q.arg(name(sorted[i]), name(sorted[last]))
			: (last > i)
			? u"%1, %2"_q.arg(name(sorted[i]), name(sorted[last]))
			: name(sorted[i]));
		i = last + 1;
	}
	return parts.join(u", "_q);
}

[[nodiscard]] QString RuleText(
		const Settings &settings,
		const ScheduleRule &rule) {
	const auto text = u"%1, %2-%3  -  %4"_q.arg(
		DaysText(rule.days),
		TimeOfDayText(rule.from),
		TimeOfDayText(rule.till),
		PresetDisplayName(settings, rule.preset));

	// Said on the row rather than shown as a greyed-out one: a rule switched
	// off is still a rule somebody wrote, and it has to stay as readable as
	// the others for the switch to be worth having.
	return rule.enabled ? text : (text + u"  (off)"_q);
}

// How the parser names this ruleset's rules in the warnings it writes, which is
// the only handle there is on a rule it threw away - the rule itself never
// reached us. See ReadScheduleRules() in the core.
[[nodiscard]] QString WarningPrefix(const QString &ruleset) {
	return ruleset.isEmpty()
		? u"schedule rule "_q
		: u"schedule ruleset '%1' rule "_q.arg(ruleset);
}

// The warnings about rules the parser refused, for the ruleset they belong to.
[[nodiscard]] QStringList RuleWarnings(const QString &ruleset) {
	const auto prefix = WarningPrefix(ruleset);
	auto result = QStringList();
	for (const auto &warning : SettingsProblems().warnings) {
		if (warning.startsWith(prefix)) {
			result.push_back(warning);
		}
	}
	return result;
}

// Everything else the parser said about the schedule: a ruleset it skipped
// whole, a malformed 'outside', an array that is not an array. Shown on the
// front page because those are the warnings no ruleset row could carry.
[[nodiscard]] QStringList ScheduleWarnings(const Settings &settings) {
	auto prefixes = QStringList{ WarningPrefix(QString()) };
	for (const auto &ruleset : settings.schedule.rulesets) {
		if (!ruleset.implicit()) {
			prefixes.push_back(WarningPrefix(ruleset.name));
		}
	}
	auto result = QStringList();
	for (const auto &warning : SettingsProblems().warnings) {
		if (!warning.startsWith(u"schedule"_q)) {
			continue;
		}
		const auto claimed = ranges::any_of(prefixes, [&](
				const QString &prefix) {
			return warning.startsWith(prefix);
		});
		if (!claimed) {
			result.push_back(warning);
		}
	}
	return result;
}

void ShowWarnings(
		not_null<Ui::VerticalLayout*> container,
		const QStringList &warnings,
		const QString &title) {
	if (warnings.isEmpty()) {
		return;
	}
	auto lines = QStringList{ title };
	for (const auto &warning : warnings) {
		lines.push_back(u"- "_q + warning);
	}
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			lines.join('\n'),
			st::boxDividerLabel),
		RowPadding());
}

// Every write this box makes. A refusal is the core saying no in words - "the
// rule at that index is not the one you read" - and those words are the only
// useful thing to put in front of somebody whose edit did not land, so they go
// on screen rather than only in the log.
bool Write(
		not_null<Ui::GenericBox*> box,
		Fn<SpliceResult(const QString&)> op,
		const QString &what) {
	const auto result = WriteSettings(std::move(op), what);
	if (!result.ok) {
		box->uiShow()->showBox(Ui::MakeInformBox(result.error));
	}
	return result.ok;
}

// A preset by name, Normal first, because Normal is what a rule aims at when it
// means "stop" and that is the most common thing to pick.
//
// `fallback' non-empty adds a row above Normal standing for "say nothing here",
// which is how a ruleset leaves the question to [schedule] outside instead of
// writing down the same answer twice.
void ChoosePresetBox(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		const QString &current,
		const QString &fallback,
		Fn<void(QString)> chosen) {
	box->setTitle(rpl::single(title));

	const auto &settings = ActiveSettings();
	auto values = std::vector<QString>();
	auto labels = std::vector<QString>();
	if (!fallback.isEmpty()) {
		values.push_back(QString());
		labels.push_back(fallback);
	}
	values.push_back(NormalPreset());
	labels.push_back(PresetDisplayName(settings, NormalPreset()));
	for (const auto &preset : settings.presets) {
		values.push_back(preset.name);
		labels.push_back(PresetDisplayName(settings, preset.name));
	}
	auto selected = -1;
	for (auto i = 0; i != int(values.size()); ++i) {
		if (!values[i].compare(current, Qt::CaseInsensitive)) {
			selected = i;
		}
	}
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(selected);
	const auto container = box->verticalLayout();
	for (auto i = 0; i != int(values.size()); ++i) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				i,
				labels[i],
				st::defaultCheckbox),
			RowPadding());
	}

	// Applied on the click rather than behind a Save button: this box asks one
	// question, and a second button to confirm a radio choice is a step that
	// only ever means "yes, the one I just pressed".
	const auto picked = box->lifetime().make_state<std::vector<QString>>(
		std::move(values));
	group->setChangedCallback([=](int value) {
		if (value >= 0 && value < int(picked->size())) {
			const auto name = (*picked)[value];
			box->closeBox();
			chosen(name);
		}
	});

	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

// Which devices a ruleset is for. The closed list of platforms first, then
// every device the file has a name for, then a field for one it does not -
// which is how an id gets into the file the first time, before any machine has
// been given a label.
void ChooseScopeBox(
		not_null<Ui::GenericBox*> box,
		const QString &current,
		Fn<void(QString)> chosen) {
	box->setTitle(rpl::single(u"Applies to"_q));

	const auto &settings = ActiveSettings();
	auto values = std::vector<QString>();
	auto labels = std::vector<QString>();
	for (const auto &[value, text] : kScopes) {
		values.push_back(QString::fromLatin1(value));
		labels.push_back(QString::fromLatin1(text));
	}
	const auto known = [&](const QString &id) {
		return ranges::any_of(values, [&](const QString &value) {
			return !value.compare(id, Qt::CaseInsensitive);
		});
	};
	const auto offer = [&](const QString &id) {
		if (id.isEmpty() || known(id)) {
			return;
		}
		values.push_back(id);
		labels.push_back(ScopeText(id));
	};
	offer(ThisDevice().id);
	for (const auto &device : settings.devices) {
		offer(device.id);
	}
	offer(current);

	auto selected = 0;
	for (auto i = 0; i != int(values.size()); ++i) {
		if (!values[i].compare(current.trimmed(), Qt::CaseInsensitive)) {
			selected = i;
		}
	}
	const auto other = int(values.size());
	labels.push_back(u"Another device, by id"_q);

	const auto group = std::make_shared<Ui::RadiobuttonGroup>(selected);
	const auto container = box->verticalLayout();
	for (auto i = 0; i != int(labels.size()); ++i) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				i,
				labels[i],
				st::defaultCheckbox),
			RowPadding());
	}
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Device id"_q)));

	const auto picked = box->lifetime().make_state<std::vector<QString>>(
		std::move(values));
	const auto submit = [=] {
		const auto value = group->current();
		const auto name = (value == other)
			? field->getLastText().trimmed()
			: (value >= 0 && value < int(picked->size()))
			? (*picked)[value]
			: QString();
		if (name.isEmpty()) {
			field->showError();
			return;
		}
		box->closeBox();
		chosen(name);
	};
	field->submits() | rpl::on_next(submit, field->lifetime());

	box->addButton(rpl::single(u"Choose"_q), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

// One rule: the days, the window, the preset, and whether it counts. `existing'
// is the rule as it was read, which is both what the fields start on and the
// fingerprint the write is checked against - so a box left open while the file
// changed underneath refuses rather than rewriting a rule nobody looked at.
void RuleBox(
		not_null<Ui::GenericBox*> box,
		const QString &ruleset,
		std::optional<ScheduleRule> existing) {
	box->setTitle(rpl::single(existing ? u"Rule"_q : u"New rule"_q));
	box->setWidth(st::boxWideWidth);

	const auto padding = RowPadding();
	const auto container = box->verticalLayout();
	const auto start = existing.value_or(ScheduleRule{
		.days = { 1, 2, 3, 4, 5 },
		.from = 9 * 60,
		.till = 17 * 60,
		.preset = NormalPreset(),
	});

	container->add(
		object_ptr<Ui::FlatLabel>(container, u"On these days"_q, st::boxLabel),
		padding);
	const auto days = box->lifetime().make_state<
		std::array<Ui::Checkbox*, 7>>();
	for (auto day = 1; day != 8; ++day) {
		(*days)[day - 1] = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				QLocale().dayName(day, QLocale::LongFormat),
				ranges::contains(start.days, day),
				st::defaultCheckbox),
			padding);
	}

	// Text fields rather than a time picker, because the file's spelling is
	// "09:00" and the core already parses exactly that. A picker would be a
	// second notion of what a time is, and the one place they disagreed would
	// be the one that mattered.
	const auto from = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"From, as 09:00"_q),
		TimeOfDayText(start.from)));
	const auto till = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"To, as 17:00"_q),
		TimeOfDayText(start.till)));

	const auto preset = box->lifetime().make_state<QString>(start.preset);
	const auto presetLabel = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	const auto presetRow = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Preset"_q),
		presetLabel->events_starting_with(
			PresetDisplayName(ActiveSettings(), *preset)),
		st::settingsButtonNoIcon);
	presetRow->setClickedCallback([=] {
		box->uiShow()->showBox(Box(
			ChoosePresetBox,
			u"Preset"_q,
			*preset,
			QString(),
			Fn<void(QString)>([=](QString name) {
				*preset = name;
				presetLabel->fire(PresetDisplayName(ActiveSettings(), name));
			})));
	});

	const auto enabled = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Enabled"_q,
			start.enabled,
			st::defaultCheckbox),
		padding);

	const auto save = [=] {
		auto rule = ScheduleRule();
		rule.enabled = enabled->checked();
		for (auto day = 1; day != 8; ++day) {
			if ((*days)[day - 1]->checked()) {
				rule.days.push_back(day);
			}
		}
		const auto parsedFrom = ParseTimeOfDay(from->getLastText().trimmed());
		const auto parsedTill = ParseTimeOfDay(till->getLastText().trimmed());
		if (!parsedFrom) {
			from->showError();
			return;
		} else if (!parsedTill) {
			till->showError();
			return;
		} else if (*parsedFrom == *parsedTill) {
			// The core's rule, checked here so the message can say why rather
			// than leaving a refusal to explain itself after the fact. A window
			// of no length is the one pair of times that cannot mean anything.
			till->showError();
			box->uiShow()->showBox(Ui::MakeInformBox(
				u"A window has to start and end at different times."_q));
			return;
		} else if (rule.days.empty()) {
			box->uiShow()->showBox(Ui::MakeInformBox(
				u"Pick at least one day."_q));
			return;
		}
		rule.from = *parsedFrom;
		rule.till = *parsedTill;
		rule.preset = *preset;

		const auto path = SettingsFilePath();
		const auto index = start.sourceIndex;
		const auto expected = ScheduleRuleExpected{
			.from = start.from,
			.till = start.till,
			.preset = start.preset,
		};
		const auto written = existing
			? Write(box, [=](const QString &text) {
				return SetScheduleRule(
					text,
					path,
					ruleset,
					index,
					expected,
					rule);
			}, u"a schedule rule"_q)
			: Write(box, [=](const QString &text) {
				return AppendScheduleRule(text, path, ruleset, rule);
			}, u"a new schedule rule"_q);

		// Closed either way. On a refusal the file is not what this box was
		// opened on any more, and the list behind it has already rebuilt itself
		// from what the file now says - so the honest next step is to look at
		// that list rather than to keep editing a rule out of a stale copy.
		box->closeBox();
	};

	box->addButton(rpl::single(u"Save"_q), save);
	if (existing) {
		const auto path = SettingsFilePath();
		const auto index = start.sourceIndex;
		const auto expected = ScheduleRuleExpected{
			.from = start.from,
			.till = start.till,
			.preset = start.preset,
		};
		box->addLeftButton(rpl::single(u"Delete"_q), [=] {
			Write(box, [=](const QString &text) {
				return RemoveScheduleRule(text, path, ruleset, index, expected);
			}, u"deleting a schedule rule"_q);
			box->closeBox();
		});
	}
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddRulesetBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"New ruleset"_q));
	box->setWidth(st::boxWideWidth);

	const auto padding = RowPadding();
	const auto container = box->verticalLayout();
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Name"_q)));
	box->setFocusCallback([=] { field->setFocusFast(); });

	const auto scope = box->lifetime().make_state<QString>(u"any"_q);
	const auto scopeLabel = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	const auto scopeRow = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Applies to"_q),
		scopeLabel->events_starting_with(ScopeText(*scope)),
		st::settingsButtonNoIcon);
	scopeRow->setClickedCallback([=] {
		box->uiShow()->showBox(Box(
			ChooseScopeBox,
			*scope,
			Fn<void(QString)>([=](QString value) {
				*scope = value;
				scopeLabel->fire(ScopeText(value));
			})));
	});

	const auto outside = box->lifetime().make_state<QString>();
	const auto outsideLabel = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	const auto outsideRow = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Outside these windows"_q),
		outsideLabel->events_starting_with(u"As the schedule says"_q),
		st::settingsButtonNoIcon);
	outsideRow->setClickedCallback([=] {
		box->uiShow()->showBox(Box(
			ChoosePresetBox,
			u"Outside these windows"_q,
			*outside,
			u"As the schedule says"_q,
			Fn<void(QString)>([=](QString name) {
				*outside = name;
				outsideLabel->fire(name.isEmpty()
					? u"As the schedule says"_q
					: PresetDisplayName(ActiveSettings(), name));
			})));
	});

	container->add(
		object_ptr<Ui::FlatLabel>(container, u"When it runs"_q, st::boxLabel),
		padding);
	const auto mode = std::make_shared<Ui::RadiobuttonGroup>(
		int(RulesetMode::Enabled));
	const auto addMode = [&](RulesetMode value, const QString &text) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				mode,
				int(value),
				text,
				st::defaultCheckbox),
			padding);
	};
	addMode(RulesetMode::Disabled, u"Off - written down, never used"_q);
	addMode(RulesetMode::Enabled, u"On - unless a more specific one wins"_q);
	addMode(RulesetMode::Always, u"Always on - alongside whichever wins"_q);

	const auto save = [=] {
		const auto name = field->getLastText().trimmed();
		if (name.isEmpty()) {
			field->showError();
			return;
		}
		const auto path = SettingsFilePath();
		const auto device = *scope;
		const auto chosen = RulesetMode(mode->current());
		if (!Write(box, [=](const QString &text) {
			return AddRuleset(text, path, name, device, chosen);
		}, u"a new ruleset"_q)) {
			return;
		}
		if (!outside->isEmpty()) {
			// A second write, because AddRuleset writes the shape of a ruleset
			// and nothing else. It is a second write to the same file a moment
			// apart, which is exactly what the automatic send's five seconds
			// are for.
			const auto wanted = *outside;
			Write(box, [=](const QString &text) {
				return SetRulesetString(
					text,
					path,
					name,
					u"outside"_q,
					wanted);
			}, u"a ruleset's outside preset"_q);
		}
		box->closeBox();
	};
	field->submits() | rpl::on_next(save, field->lifetime());

	box->addButton(rpl::single(u"Create"_q), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

// One ruleset and its rules. The implicit one - the flat [[schedule.rules]]
// array the parser wraps for us - shows only its rules: there is no
// [[schedule.rulesets]] block behind it to write a mode, a device or an outside
// preset into, and offering those rows would be offering an edit that cannot
// land anywhere.
void RulesetBox(not_null<Ui::GenericBox*> box, QString name) {
	box->setTitle(rpl::single(name.isEmpty() ? u"Rules"_q : name));
	box->setWidth(st::boxWideWidth);

	const auto padding = RowPadding();
	const auto container = box->verticalLayout();
	const auto rows = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	const auto rebuild = box->lifetime().make_state<Fn<void()>>();
	*rebuild = [=] {
		rows->clear();

		const auto &settings = ActiveSettings();
		const auto ruleset = FindRuleset(settings, name);
		if (!ruleset) {
			rows->add(
				object_ptr<Ui::FlatLabel>(
					rows,
					u"This ruleset is no longer in the file."_q,
					st::boxLabel),
				padding);
			return;
		}
		const auto path = SettingsFilePath();
		const auto again = [=] { crl::on_main(box, [=] { (*rebuild)(); }); };

		if (!ruleset->implicit()) {
			Ui::AddSubsectionTitle(rows, rpl::single(u"This ruleset"_q));

			const auto mode = std::make_shared<Ui::RadiobuttonGroup>(
				int(ruleset->mode));
			const auto addMode = [&](RulesetMode value, const QString &text) {
				rows->add(
					object_ptr<Ui::Radiobutton>(
						rows,
						mode,
						int(value),
						text,
						st::defaultCheckbox),
					padding);
			};
			addMode(RulesetMode::Disabled, u"Off - written down, never used"_q);
			addMode(
				RulesetMode::Enabled,
				u"On - unless a more specific one wins"_q);
			addMode(
				RulesetMode::Always,
				u"Always on - alongside whichever wins"_q);
			const auto was = int(ruleset->mode);
			mode->setChangedCallback([=](int value) {
				if (value == was) {
					return;
				}
				Write(box, [=](const QString &text) {
					return SetRulesetString(
						text,
						path,
						name,
						u"mode"_q,
						RulesetModeName(RulesetMode(value)));
				}, u"a ruleset's mode"_q);
				again();
			});

			const auto scope = ruleset->device;
			const auto scopeRow = ::Settings::AddButtonWithLabel(
				rows,
				rpl::single(u"Applies to"_q),
				rpl::single(ScopeText(scope)),
				st::settingsButtonNoIcon);
			scopeRow->setClickedCallback([=] {
				box->uiShow()->showBox(Box(
					ChooseScopeBox,
					scope,
					Fn<void(QString)>([=](QString value) {
						Write(box, [=](const QString &text) {
							return SetRulesetString(
								text,
								path,
								name,
								u"device"_q,
								value);
						}, u"a ruleset's device"_q);
						again();
					})));
			});

			const auto outside = ruleset->outside.value_or(QString());
			const auto outsideRow = ::Settings::AddButtonWithLabel(
				rows,
				rpl::single(u"Outside these windows"_q),
				rpl::single(outside.isEmpty()
					? u"As the schedule says"_q
					: PresetDisplayName(settings, outside)),
				st::settingsButtonNoIcon);
			outsideRow->setClickedCallback([=] {
				box->uiShow()->showBox(Box(
					ChoosePresetBox,
					u"Outside these windows"_q,
					outside,
					u"As the schedule says"_q,
					Fn<void(QString)>([=](QString value) {
						// An empty value takes the key back out of the file,
						// which is how a ruleset says "leave it to [schedule]"
						// instead of writing that answer down a second time.
						Write(box, [=](const QString &text) {
							return SetRulesetString(
								text,
								path,
								name,
								u"outside"_q,
								value);
						}, u"a ruleset's outside preset"_q);
						again();
					})));
			});
			Ui::AddSkip(rows);
			Ui::AddDivider(rows);
		}

		Ui::AddSubsectionTitle(rows, rpl::single(u"Windows"_q));
		for (const auto &rule : ruleset->rules) {
			const auto row = ::Settings::AddButtonWithIcon(
				rows,
				rpl::single(RuleText(settings, rule)),
				st::settingsButtonNoIcon);
			const auto copy = rule;
			row->setClickedCallback([=] {
				box->uiShow()->showBox(Box(RuleBox, name, copy));
			});
		}
		const auto add = ::Settings::AddButtonWithIcon(
			rows,
			rpl::single(u"Add a window..."_q),
			st::settingsButtonNoIcon);
		add->setClickedCallback([=] {
			box->uiShow()->showBox(
				Box(RuleBox, name, std::optional<ScheduleRule>()));
		});

		ShowWarnings(
			rows,
			RuleWarnings(name),
			u"Rules this file describes that the parser could not use:"_q);
	};
	(*rebuild)();

	SettingsChanges(
	) | rpl::on_next([=] {
		// Deferred: a switch in this box is one of the things that fires this,
		// and rebuilding straight from the callback would destroy the widget
		// whose click is still on the stack.
		crl::on_main(box, [=] { (*rebuild)(); });
	}, box->lifetime());

	// The other half of "Add a ruleset...". A ruleset you can create and not
	// remove is a one-way door, and switching it Off is not the same answer:
	// that is a ruleset kept for later, which is a thing the mode exists to
	// say. Only for a real one - the implicit ruleset is the flat array of
	// rules and there is no block to take out.
	if (!name.isEmpty()) {
		const auto path = SettingsFilePath();
		box->addLeftButton(rpl::single(u"Delete"_q), [=] {
			box->uiShow()->showBox(Ui::MakeConfirmBox({
				.text = u"Delete the ruleset '%1' and the windows in it?"_q.arg(
					name),
				.confirmed = [=](Fn<void()> close) {
					close();
					Write(box, [=](const QString &text) {
						return RemoveRuleset(text, path, name);
					}, u"deleting a ruleset"_q);
					box->closeBox();
				},
				.confirmText = u"Delete"_q,
			}));
		});
	}

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void ScheduleBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Schedule"_q));
	box->setWidth(st::boxWideWidth);

	const auto padding = RowPadding();
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"A window is a time of day, on some days of the week, that wants "
			"a preset. The first one that covers the moment decides; between "
			"them the schedule falls back to the preset below."_q,
			st::boxLabel),
		st::boxRowPadding);

	const auto rows = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	const auto rebuild = box->lifetime().make_state<Fn<void()>>();
	*rebuild = [=] {
		rows->clear();

		const auto &settings = ActiveSettings();
		const auto path = SettingsFilePath();
		const auto again = [=] { crl::on_main(box, [=] { (*rebuild)(); }); };

		const auto wasEnabled = settings.schedule.enabled;
		const auto enabled = rows->add(
			object_ptr<Ui::Checkbox>(
				rows,
				u"Follow the schedule"_q,
				wasEnabled,
				st::defaultCheckbox),
			padding);
		enabled->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (checked == wasEnabled) {
				return;
			}
			Write(box, [=](const QString &text) {
				return SetTableBool(
					text,
					path,
					u"schedule"_q,
					u"enabled_p"_q,
					checked);
			}, u"the schedule switch"_q);

			// Rebuilt whether or not the write landed, so the switch ends up
			// where the file is rather than where the click left it.
			again();
		}, enabled->lifetime());

		const auto paused = SchedulePaused();
		const auto pause = rows->add(
			object_ptr<Ui::Checkbox>(
				rows,
				u"Pause the schedule"_q,
				paused,
				st::defaultCheckbox),
			padding);
		pause->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (checked != SchedulePaused()) {
				SetSchedulePaused(checked);
				again();
			}
		}, pause->lifetime());

		if (paused) {
			const auto until = CurrentState().schedulePausedUntil;
			const auto row = ::Settings::AddButtonWithLabel(
				rows,
				rpl::single(u"Until"_q),
				rpl::single(until
					? langDateTime(QDateTime::fromSecsSinceEpoch(until))
					: u"You unpause it"_q),
				st::settingsButtonNoIcon);
			row->setClickedCallback([=] {
				box->uiShow()->showBox(Box([=](
						not_null<Ui::GenericBox*> inner) {
					Ui::ChooseDateTimeBox(inner, {
						.title = rpl::single(u"Pause until"_q),
						.submit = rpl::single(u"Pause"_q),
						.done = [=](TimeId result) {
							SetSchedulePaused(true, result);
							inner->closeBox();
							again();
						},
						.min = [] { return base::unixtime::now() + 60; },
						.time = base::unixtime::now() + kPauseDefault,
					});
				}));
			});
		}

		const auto status = ScheduleStatusText();
		if (!status.isEmpty()) {
			rows->add(
				object_ptr<Ui::FlatLabel>(rows, status, st::boxLabel),
				padding);
		}
		rows->add(
			object_ptr<Ui::FlatLabel>(
				rows,
				ScheduleDeviceText(),
				st::boxDividerLabel),
			padding);

		const auto outside = settings.schedule.outside;
		const auto outsideRow = ::Settings::AddButtonWithLabel(
			rows,
			rpl::single(u"Outside these windows"_q),
			rpl::single(PresetDisplayName(settings, outside)),
			st::settingsButtonNoIcon);
		outsideRow->setClickedCallback([=] {
			box->uiShow()->showBox(Box(
				ChoosePresetBox,
				u"Outside these windows"_q,
				outside,
				QString(),
				Fn<void(QString)>([=](QString name) {
					Write(box, [=](const QString &text) {
						return SetTableString(
							text,
							path,
							u"schedule"_q,
							u"outside"_q,
							name);
					}, u"the preset between windows"_q);
					again();
				})));
		});

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		Ui::AddSubsectionTitle(rows, rpl::single(u"Rulesets"_q));

		for (const auto &ruleset : settings.schedule.rulesets) {
			// The implicit one is the flat array of rules a file has when it
			// never grew a ruleset, so it is named for what it is rather than
			// for a block that is not in the file.
			const auto name = ruleset.implicit() ? QString() : ruleset.name;
			const auto count = int(ruleset.rules.size());
			const auto row = ::Settings::AddButtonWithLabel(
				rows,
				rpl::single(name.isEmpty() ? u"Rules"_q : name),
				rpl::single(u"%1 · %2 · %3"_q.arg(
					ScopeText(ruleset.device),
					ModeText(ruleset.mode),
					(count == 1)
						? u"1 window"_q
						: u"%1 windows"_q.arg(count))),
				st::settingsButtonNoIcon);
			row->setClickedCallback([=] {
				box->uiShow()->showBox(Box(RulesetBox, name));
			});
		}

		const auto add = ::Settings::AddButtonWithIcon(
			rows,
			rpl::single(u"Add a ruleset..."_q),
			st::settingsButtonNoIcon);
		add->setClickedCallback([=] {
			box->uiShow()->showBox(Box(AddRulesetBox));
		});

		ShowWarnings(
			rows,
			ScheduleWarnings(settings),
			u"The parser could not use everything in [schedule]:"_q);
	};
	(*rebuild)();

	rpl::merge(
		SettingsChanges(),
		StateChanges()
	) | rpl::on_next([=] {
		crl::on_main(box, [=] { (*rebuild)(); });
	}, box->lifetime());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void DeviceLabelBox(not_null<Ui::GenericBox*> box) {
	const auto id = ThisDevice().id;
	box->setTitle(rpl::single(u"This device"_q));

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			id.isEmpty()
				? u"This device reports no id, so there is nothing for a "
					"ruleset to name it by."_q
				: u"Rulesets name this device by its id, %1. The name below "
					"is what every device reading this file calls it."_q.arg(
						id),
			st::boxLabel));
	if (id.isEmpty()) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	const auto named = ActiveSettings().device(id);
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Name"_q),
		named ? named->label : QString()));
	box->setFocusCallback([=] { field->setFocusFast(); });

	const auto save = [=] {
		const auto label = field->getLastText().trimmed();
		const auto path = SettingsFilePath();
		Write(box, [=](const QString &text) {
			return SetTableString(text, path, u"devices"_q, id, label);
		}, u"this device's name"_q);
		box->closeBox();
	};
	field->submits() | rpl::on_next(save, field->lifetime());

	box->addButton(rpl::single(u"Save"_q), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace Purple
