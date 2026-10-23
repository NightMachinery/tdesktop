/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_config.h"
#include "purple/purple_sync_config_history.h"
#include "purple/purple_sync_config_review.h"

#include <optional>

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Purple {

struct SyncConfigReviewBoxArgs {
	SyncConfigReview review;
	QString accountCaption;
	AccountListPreviewNotes listNotes;
	AccountTransferReview replacement;
	Fn<void(std::optional<QString>, AccountTransferReview)> choose;
};

void SyncConfigReviewBox(
	not_null<Ui::GenericBox*> box,
	SyncConfigReviewBoxArgs args);

using SyncHistoryReview = Fn<std::optional<AccountTransferReview>(const QByteArray&)>;
using SyncHistoryRestore = Fn<bool(const SyncConfigHistoryEntry&, const AccountTransferReview&)>;

void SyncConfigHistoryEntryBox(
	not_null<Ui::GenericBox*> box,
	SyncHistoryAccess access,
	SyncConfigHistoryEntry entry,
	SyncHistoryReview review,
	SyncHistoryRestore restore,
	Fn<void()> closeList,
	AccountListPreviewNotes listNotes,
	QString confirmText);

void SyncConfigHistoryBox(
	not_null<Ui::GenericBox*> box,
	SyncHistoryAccess access,
	SyncHistoryReview review,
	SyncHistoryRestore restore,
	AccountListPreviewNotes listNotes);

} // namespace Purple
