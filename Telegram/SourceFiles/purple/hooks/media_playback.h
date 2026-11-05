/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_audio_msg_id.h"

#include <QtCore/QSize>
#include <QtCore/qnamespace.h>

#include <memory>
#include <optional>

class ClickHandler;
class DocumentData;
class History;
class HistoryItem;
class QWidget;

namespace Media::Player {
class Float;
} // namespace Media::Player

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Purple {

[[nodiscard]] bool RestartOnDoubleClick(
	not_null<QWidget*> widget,
	const std::shared_ptr<ClickHandler> &active,
	Qt::MouseButton button,
	bool selecting);

[[nodiscard]] QSize VideoMessageThumbSize(
	const DocumentData *document,
	const HistoryView::Element *parent,
	int &inOutWidthMax);
[[nodiscard]] int VideoMessageBubbleWidthLimit(
	const DocumentData *document,
	const HistoryView::Element *parent,
	int maxWidth,
	bool ephemeral);

[[nodiscard]] int FloatPlayerSize(
	const QWidget *parent,
	const DocumentData *document,
	int size);

[[nodiscard]] bool RestartFloatOnDoubleClick(const HistoryItem *item);
void DecorateFloat(
	not_null<Media::Player::Float*> floating,
	Fn<void(not_null<const HistoryItem*>)> open);

[[nodiscard]] std::optional<HistoryItem*> ConfinedVoiceNext(
	AudioMsgId::Type type,
	bool autonext,
	const History *history,
	int from,
	Fn<HistoryItem*(int)> itemAt);

} // namespace Purple
