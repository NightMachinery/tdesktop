/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_config_history.h"
#include "purple/purple_sync_config_review.h"

#include <optional>

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Purple {

struct SyncConfigReviewBoxArgs {
	SyncConfigReview review;
	Fn<void(std::optional<QString>)> choose;
};

void SyncConfigReviewBox(
	not_null<Ui::GenericBox*> box,
	SyncConfigReviewBoxArgs args);

void SyncConfigHistoryBox(
	not_null<Ui::GenericBox*> box,
	Fn<bool(const SyncConfigHistoryEntry&)> restore);

} // namespace Purple
