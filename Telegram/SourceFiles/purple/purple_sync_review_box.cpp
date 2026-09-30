/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_review_box.h"

#include "lang/lang_keys.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_config_text.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"

#include <QtCore/QStringList>

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <memory>
#include <vector>

namespace Purple {
namespace {

using ChangePreview = Fn<void(const QByteArray&, const QByteArray&)>;

struct PreviewState {
	ConfigTextDiff diff;
	bool shown = false;
};

struct ReviewBoxState {
	SyncConfigReview review;
	Fn<void(std::optional<QString>)> choose;
	std::vector<std::optional<QString>> keys;
	rpl::variable<QString> confirmText;
	ChangePreview preview;
	Ui::FlatLabel *compare = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *newer = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *disclosure = nullptr;
	int selected = 0;
	bool done = false;
};

Ui::FlatLabel *AddLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		const style::FlatLabel &st = st::boxLabel) {
	return container->add(
		object_ptr<Ui::FlatLabel>(container, text, st),
		st::boxRowPadding);
}

[[nodiscard]] Ui::SlideWrap<Ui::FlatLabel> *AddHiddenLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		const style::FlatLabel &st) {
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(container, text, st)),
		st::boxRowPadding);
	wrap->hide(anim::type::instant);
	return wrap;
}

[[nodiscard]] ChangePreview AddChangePreview(
		not_null<Ui::VerticalLayout*> container) {
	const auto summary = AddLabel(container, QString());
	const auto toggle = container->add(
		object_ptr<Ui::LinkButton>(container, u"Show lines"_q),
		st::boxRowPadding);
	const auto wrap = AddHiddenLabel(container, QString(), st::boxLabel);
	const auto lines = wrap->entity();
	lines->setSelectable(true);
	const auto state = container->lifetime().make_state<PreviewState>();
	const auto refreshLines = [=] {
		if (state->shown) {
			lines->setMarkedText(Ui::Text::Wrapped(
				tr::marked(SyncConfigDiffText(state->diff)),
				EntityType::Pre));
		}
	};
	toggle->setClickedCallback([=] {
		state->shown = !state->shown;
		toggle->setText(state->shown ? u"Hide lines"_q : u"Show lines"_q);
		refreshLines();
		wrap->toggle(state->shown, anim::type::instant);
	});
	return [=](const QByteArray &before, const QByteArray &after) {
		state->diff = DiffConfigText(before, after);
		const auto changes = SyncConfigChangeLines(
			SummarizeConfigChange(before, after),
			state->diff);
		summary->setText(
			QStringList(changes.begin(), changes.end()).join(u'\n'));
		refreshLines();
	};
}

[[nodiscard]] const SyncConfigHeadRecord *OfferedRecord(
		const SyncConfigReview &review,
		const QString &key) {
	for (const auto &head : review.plan.offered) {
		if (head.key == key) {
			return FindSyncConfigHeadRecord(review, head);
		}
	}
	return nullptr;
}

void SelectReviewOption(not_null<ReviewBoxState*> state, int index) {
	const auto &review = state->review;
	const auto update = (review.plan.verdict == ConfigSyncVerdict::UpdateReady);
	state->selected = index;
	const auto key = state->keys[index];
	if (key) {
		const auto record = OfferedRecord(review, *key);
		state->compare->setText(update
			? u"Applying changes this device's settings like this:"_q
			: u"If you use this version, this device's settings change like "
				"this:"_q);
		state->preview(
			review.local.text,
			record ? record->text : QByteArray());
		state->newer->toggle(
			record && record->newerSchema,
			anim::type::instant);
	} else {
		const auto first = review.plan.offered.empty()
			? nullptr
			: FindSyncConfigHeadRecord(review, review.plan.offered.front());
		state->compare->setText(first
			? u"Your other devices will be offered this device's settings. "
				"Compared with %1's version:"_q.arg(SyncDeviceName(*first))
			: u"Your other devices will be offered this device's "
				"settings."_q);
		state->preview(first ? first->text : QByteArray(), review.local.text);
		state->newer->toggle(false, anim::type::instant);
	}
	const auto publishes = !update && SyncChoicePublishes(review, key);
	state->disclosure->entity()->setText(publishes
		? u"This choice also sends this device's settings to this account's "
			"Saved Messages. "_q + SyncCloudDisclosureText()
		: u"Joining links this device to this account's sync, so settings "
			"you publish from it later are sent to Saved Messages. "_q
			+ SyncCloudDisclosureText());
	state->disclosure->toggle(
		publishes || !review.bound,
		anim::type::instant);
	state->confirmText = SyncConfigChoiceButtonText(review, key);
}

void HistoryEntryBox(
		not_null<Ui::GenericBox*> box,
		SyncConfigHistoryEntry entry,
		Fn<bool(const SyncConfigHistoryEntry&)> restore,
		Fn<void()> closeList) {
	box->setTitle(rpl::single(u"Saved settings"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	AddLabel(container, SyncConfigHistoryRowText(entry));
	Ui::AddSkip(container);
	const auto text = entry.existed
		? ReadSyncConfigHistory(entry.id)
		: std::optional<QByteArray>();
	const auto current = ReadSyncSettingsFile(SettingsFilePath());
	const auto restorable = entry.existed
		&& text.has_value()
		&& SyncSettingsTextWritable(*text)
		&& current.status != SyncSettingsFileStatus::Invalid;
	if (!entry.existed) {
		AddLabel(container, u"settings.toml did not exist when this entry "
			"was saved, so there is nothing to restore."_q);
	} else if (!text) {
		AddLabel(container, u"This copy could not be read. It may have been "
			"removed or damaged."_q);
	} else if (!SyncSettingsTextWritable(*text)) {
		AddLabel(container, u"This copy is not valid UTF-8 text, so it "
			"cannot be restored here."_q);
	} else if (current.status == SyncSettingsFileStatus::Invalid) {
		AddLabel(container, u"settings.toml cannot be read right now, so it "
			"cannot be replaced."_q);
	} else {
		AddLabel(container, (current.status == SyncSettingsFileStatus::Absent)
			? u"settings.toml is missing now. Restoring creates it with this "
				"version:"_q
			: u"Restoring changes this device's settings like this:"_q);
		const auto preview = AddChangePreview(container);
		preview(current.text, *text);
	}
	if (restorable) {
		box->addButton(rpl::single(u"Restore"_q), [=] {
			box->uiShow()->showBox(Ui::MakeConfirmBox({
				.text = u"Replace settings.toml with the version from %1? "
					"The current file is kept in History. The change stays "
					"on this device until you publish it."_q.arg(
						SyncConfigHistoryMomentText(entry)),
				.confirmed = crl::guard(box, [=](Fn<void()> close) {
					close();
					if (restore && restore(entry)) {
						box->closeBox();
						if (closeList) {
							closeList();
						}
					}
				}),
				.confirmText = u"Restore"_q,
			}));
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void SyncConfigReviewBox(
		not_null<Ui::GenericBox*> box,
		SyncConfigReviewBoxArgs args) {
	const auto state = box->lifetime().make_state<ReviewBoxState>();
	state->review = std::move(args.review);
	state->choose = std::move(args.choose);
	const auto &review = state->review;
	const auto update = (review.plan.verdict == ConfigSyncVerdict::UpdateReady);
	const auto first = review.plan.offered.empty()
		? nullptr
		: FindSyncConfigHeadRecord(review, review.plan.offered.front());
	box->setTitle(rpl::single(update
		? u"Update from %1"_q.arg(first
			? SyncDeviceName(*first)
			: u"another device"_q)
		: review.bound
		? u"Choose settings"_q
		: u"Join sync"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	AddLabel(container, SyncConfigChoiceIntro(review));
	Ui::AddSkip(container);

	auto labels = std::vector<QString>();
	const auto choices = SyncConfigChoices(
		review,
		SyncSettingsPublishable(review));
	for (const auto &choice : choices) {
		state->keys.push_back(choice.key);
		if (choice.key) {
			const auto &record = review.heads[choice.head];
			labels.push_back(u"%1 · changed %2"_q.arg(
				SyncDeviceName(record),
				SyncRecordTimeText(record.at)));
		} else {
			labels.push_back(u"This device's settings"_q);
		}
	}
	if (state->keys.empty()) {
		AddLabel(container, u"Nothing can be chosen from this check. Check "
			"again."_q);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	if (update) {
		AddLabel(container, labels.front());
	} else {
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
		for (auto i = 0; i != int(labels.size()); ++i) {
			container->add(
				object_ptr<Ui::Radiobutton>(
					container,
					group,
					i,
					labels[i],
					st::defaultCheckbox),
				st::boxRowPadding);
		}
		group->setChangedCallback([=](int value) {
			if (value >= 0 && value < int(state->keys.size())) {
				SelectReviewOption(state, value);
			}
		});
	}
	Ui::AddSkip(container);
	state->compare = AddLabel(container, QString());
	state->preview = AddChangePreview(container);
	state->newer = AddHiddenLabel(
		container,
		u"Made with a newer Purple. Settings this version does not know "
			"may be ignored here."_q,
		st::boxDividerLabel);
	state->disclosure = AddHiddenLabel(
		container,
		QString(),
		st::boxDividerLabel);
	SelectReviewOption(state, 0);

	box->addButton(state->confirmText.value(), [=] {
		if (state->done) {
			return;
		}
		state->done = true;
		const auto choose = state->choose;
		const auto key = state->keys[state->selected];
		box->closeBox();
		if (choose) {
			choose(key);
		}
	});
	box->addButton(
		rpl::single(update ? u"Not now"_q : u"Cancel"_q),
		[=] { box->closeBox(); });
}

void SyncConfigHistoryBox(
		not_null<Ui::GenericBox*> box,
		Fn<bool(const SyncConfigHistoryEntry&)> restore) {
	box->setTitle(rpl::single(u"Settings History"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	AddLabel(container, u"Copies of settings.toml kept before sync, a "
		"restore or an undo replaced it, newest first. Up to 30 are "
		"kept."_q);
	Ui::AddSkip(container);
	const auto entries = ListSyncConfigHistory();
	if (entries.empty()) {
		AddLabel(container, u"No copies yet."_q, st::boxDividerLabel);
	}
	const auto closeList = crl::guard(box, [=] { box->closeBox(); });
	for (const auto &entry : entries) {
		const auto row = ::Settings::AddButtonWithIcon(
			container,
			rpl::single(SyncConfigHistoryRowText(entry)),
			st::settingsButtonNoIcon);
		row->setClickedCallback([=] {
			box->uiShow()->showBox(Box(
				HistoryEntryBox,
				entry,
				restore,
				closeList));
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Purple
