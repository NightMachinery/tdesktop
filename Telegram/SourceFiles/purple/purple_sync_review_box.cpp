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
	Fn<void(std::optional<QString>, AccountTransferReview)> choose;
	AccountTransferReview replacement;
	std::vector<SyncConfigChoice> choices;
	Ui::FlatLabel *impact = nullptr;
	Ui::RoundButton *apply = nullptr;
	std::vector<std::optional<QString>> keys;
	rpl::variable<QString> confirmText;
	ChangePreview preview;
	Ui::FlatLabel *compare = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *newer = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *disclosure = nullptr;
	int selected = -1;
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
		not_null<Ui::VerticalLayout*> container,
		AccountListPreviewNotes listNotes) {
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
		auto changes = SyncConfigChangeLines(
			SummarizeConfigChange(before, after),
			state->diff);
		if (listNotes) {
			const auto notes = listNotes(before, after);
			if (!notes.isEmpty()) { changes.push_back(notes); }
		}
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
	auto impacts = QStringList();
	const auto &choice = state->choices[index];
	for (const auto &change : choice.impacts) {
		const auto source = change.replacedKey ? OfferedRecord(review, *change.replacedKey) : nullptr;
		impacts.push_back((source ? SyncDeviceName(*source) + u":\n"_q : QString())
			+ ConfigImpactDisplayText(change.impact));
	}
	if (state->replacement.usingGood) {
		impacts.push_back(tr::lng_purple_transfer_good(tr::now));
	}
	if (choice.dangerous && state->replacement.context.role == ConfigImpactFileRole::Global) {
		impacts.prepend(tr::lng_purple_transfer_global_warning(tr::now));
	}
	state->impact->setText(impacts.join(u"\n\n"_q));
	state->apply->setDisabled(!choice.impactReady);
	state->confirmText = SyncConfigChoiceButtonText(review, key);
}

} // namespace

void SyncConfigHistoryEntryBox(
		not_null<Ui::GenericBox*> box,
		SyncHistoryAccess access,
		SyncConfigHistoryEntry entry,
		SyncHistoryReview review,
		SyncHistoryRestore restore,
		Fn<void()> closeList,
		AccountListPreviewNotes listNotes,
		QString confirmText) {
	box->setTitle(rpl::single(u"Saved settings"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	AddLabel(container, SyncConfigHistoryRowText(entry));
	const auto text = entry.existed ? ReadSyncConfigHistory(access, entry.id) : std::nullopt;
	const auto replacement = text && review ? review(*text) : std::nullopt;
	const auto restorable = replacement && replacement->impact.status == ConfigImpactStatus::Ready;
	if (!entry.existed) {
		AddLabel(container, u"settings.toml did not exist when this entry was saved, so there is nothing to restore."_q);
	} else if (!restorable) {
		AddLabel(container, tr::lng_purple_transfer_unavailable(tr::now));
	} else {
		if (replacement->impact.dangerous && replacement->context.role == ConfigImpactFileRole::Global) {
			AddLabel(container, tr::lng_purple_transfer_global_warning(tr::now));
		}
		AddLabel(container, replacement->destination.file.path);
		AddLabel(container, ConfigImpactDisplayText(replacement->impact));
		if (replacement->usingGood) {
			AddLabel(container, tr::lng_purple_transfer_good(tr::now));
		}
		const auto preview = AddChangePreview(container, listNotes);
		preview(replacement->context.current.text, *text);
		box->addButton(rpl::single(confirmText), [=] {
			if (access.valid && access.valid() && restore && restore(entry, *replacement)) {
				box->closeBox();
				if (closeList) { closeList(); }
			}
		});
	}
	const auto cancel = box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { cancel->setFocus(); });
}

void SyncConfigReviewBox(
		not_null<Ui::GenericBox*> box,
		SyncConfigReviewBoxArgs args) {
	const auto state = box->lifetime().make_state<ReviewBoxState>();
	state->review = std::move(args.review);
	state->choose = std::move(args.choose);
	state->replacement = std::move(args.replacement);
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
	AddLabel(container, args.accountCaption);
	AddLabel(container, state->replacement.context.role == ConfigImpactFileRole::Global
		? tr::lng_purple_transfer_global_role(tr::now) : tr::lng_purple_transfer_own_role(tr::now));
	AddLabel(container, state->replacement.destination.file.path);
	AddLabel(container, SyncConfigChoiceIntro(review));
	Ui::AddSkip(container);

	auto labels = std::vector<QString>();
	state->choices = SyncConfigChoices(
		review,
		SyncSettingsPublishable(review),
		state->replacement.context);
	const auto &choices = state->choices;
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
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(-1);
	for (auto i = 0; i != int(labels.size()); ++i) {
		container->add(object_ptr<Ui::Radiobutton>(container, group, i,
			labels[i], st::defaultCheckbox), st::boxRowPadding);
	}
	group->setChangedCallback([=](int value) {
		if (value >= 0 && value < int(state->keys.size())) { SelectReviewOption(state, value); }
	});
	Ui::AddSkip(container);
	state->compare = AddLabel(container, QString());
	state->preview = AddChangePreview(container, std::move(args.listNotes));
	state->newer = AddHiddenLabel(
		container,
		u"Made with a newer Purple. Settings this version does not know "
			"may be ignored here."_q,
		st::boxDividerLabel);
	state->disclosure = AddHiddenLabel(
		container,
		QString(),
		st::boxDividerLabel);
	state->impact = AddLabel(container, QString());

	state->confirmText = u"Apply"_q;
	state->apply = box->addButton(state->confirmText.value(), [=] {
		if (state->done || state->selected < 0 || !state->choices[state->selected].impactReady) {
			return;
		}
		state->done = true;
		const auto choose = state->choose;
		const auto key = state->keys[state->selected];
		if (choose) {
			auto replacement = state->replacement;
			const auto record = key ? OfferedRecord(state->review, *key) : nullptr;
			replacement.context.proposed = { record ? record->text : state->review.local.text, true };
			replacement.impact = AnalyzeConfigChange(replacement.context);
			box->closeBox();
			choose(key, std::move(replacement));
		}
	});
	state->apply->setDisabled(true);
	const auto cancel = box->addButton(
		rpl::single(update ? u"Not now"_q : u"Cancel"_q),
		[=] { box->closeBox(); });
	box->setFocusCallback([=] { cancel->setFocus(); });
}

void SyncConfigHistoryBox(
		not_null<Ui::GenericBox*> box,
		SyncHistoryAccess access,
		SyncHistoryReview review,
		SyncHistoryRestore restore,
		AccountListPreviewNotes listNotes) {
	box->setTitle(rpl::single(u"Settings History"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	AddLabel(container, u"Copies of settings.toml kept before sync, a "
		"restore or an undo replaced it, newest first. Up to 30 are "
		"kept."_q);
	Ui::AddSkip(container);
	const auto entries = ListSyncConfigHistory(access);
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
				SyncConfigHistoryEntryBox,
				access,
				entry,
				review,
				restore,
				closeList,
				listNotes,
				u"Restore"_q));
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Purple
