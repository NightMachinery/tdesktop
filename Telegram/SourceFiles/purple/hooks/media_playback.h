/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QSize>
#include <QtCore/qnamespace.h>

#include <memory>

class ClickHandler;
class DocumentData;
class QWidget;

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Purple {

[[nodiscard]] bool RestartOnDoubleClick(
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

} // namespace Purple
