#pragma once

#include "purple/purple_keys_colors.h"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtGui/QFontMetrics>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>

#include <array>
#include <optional>
#include <vector>

namespace Purple::ModalUi {

enum class BadgeKind {
	Normal,
	Insert,
	Leader,
	Local,
	Hints,
	Palette,
	DescribeKey,
	Help,
};

struct StatusHint {
	QString keys;
	QString label;
};

struct StatusState {
	BadgeKind badge = BadgeKind::Normal;
	QString badgeText;
	QString paneLabel;
	QString localleaderKey;
	QString pendingText;
	QString pendingGroup;
	QString message;
	bool messageIsError = false;
	bool paneInDialogs = false;
	std::vector<StatusHint> hints;
	int unreadDm = 0;
	int unreadGp = 0;
	int unreadCh = 0;
	bool unreadKnown = false;
	bool zenMode = false;
};

struct StatusColumns {
	QRect rail;
	QRect dialogs;
	QRect chat;
	QRect info;
	QRect dialogsDivider;
	QRect infoDivider;
	int dialogsLeadingInset = 0;
	std::optional<int> chatLeadingInset;
	std::optional<int> chatTrailingInset;

	friend bool operator==(const StatusColumns &, const StatusColumns &) = default;
};

struct StatusPalette {
	QColor window;
	QColor rail;
	QColor dialogs;
	QColor chat;
	QColor info;
	QColor divider;
	QColor text;
	QColor muted;
	QColor error;
	QColor accent;
};

[[nodiscard]] inline std::vector<StatusHint> UnreadStatusHints(const StatusState &state) {
	auto result = std::vector<StatusHint>();
	if (state.unreadKnown) {
		for (const auto &[label, count] : std::array{
				std::pair{ QStringLiteral("DM"), state.unreadDm },
				std::pair{ QStringLiteral("GP"), state.unreadGp },
				std::pair{ QStringLiteral("CH"), state.unreadCh },
			}) {
			if (count > 0) {
				result.push_back({ .keys = QString::number(count), .label = label });
			}
		}
	}
	return result;
}

namespace StatusPaint {

[[nodiscard]] inline QRect Across(QRect span, QRect bar) {
	return QRect(span.x(), bar.y(), span.width(), bar.height()).intersected(bar);
}

[[nodiscard]] inline QRect TextArea(
		QRect pane,
		int leading,
		int trailing,
		bool rtl) {
	const auto start = std::clamp(leading, 0, pane.width());
	const auto end = std::clamp(trailing, 0, pane.width() - start);
	return rtl ? pane.adjusted(end, 0, -start, 0)
		: pane.adjusted(start, 0, -end, 0);
}

inline QRect TakeEdge(QRect &space, int width, bool fromRight) {
	const auto actual = std::clamp(width, 0, space.width());
	auto part = space;
	if (fromRight) {
		part.setLeft(space.right() - actual + 1);
		space.setRight(part.left() - 1);
	} else {
		part.setWidth(actual);
		space.setLeft(part.right() + 1);
	}
	return part;
}

[[nodiscard]] inline QColor Surface(QColor color, QColor canvas) {
	return color.alpha() == 255 ? color : BlendColor(canvas, color, color.alphaF());
}

[[nodiscard]] inline int TextWidth(const QFont &font, const QString &text) {
	return text.isEmpty() ? 0 : int(std::ceil(QFontMetricsF(font).horizontalAdvance(text))) + 1;
}

struct Ink {
	QFont regular;
	QFont bold;
	QColor text;
	QColor muted;
	QColor alert;
	QColor accent;

	[[nodiscard]] int width(const QString &value, bool strong = false) const {
		return TextWidth(strong ? bold : regular, value);
	}
};

inline void Text(
		QPainter &p,
		QRect rect,
		const QString &text,
		const QFont &font,
		QColor color,
		Qt::AlignmentFlag align) {
	if (!rect.isEmpty() && !text.isEmpty()) {
		p.save();
		p.setClipRect(rect, Qt::IntersectClip);
		p.setFont(font);
		p.setPen(color);
		p.drawText(
			rect,
			int(align | Qt::AlignAbsolute | Qt::AlignVCenter) | Qt::TextSingleLine,
			(TextWidth(font, text) <= rect.width())
				? text : QFontMetrics(font).elidedText(text, Qt::ElideRight, rect.width()));
		p.restore();
	}
}

[[nodiscard]] inline int HintWidth(const StatusHint &hint, const Ink &ink) {
	const auto space = ink.width(QStringLiteral(" "));
	return ink.width(hint.keys, true) + ink.width(hint.label)
		+ (hint.keys.isEmpty() || hint.label.isEmpty() ? 0 : space);
}

inline void Hint(
		QPainter &p,
		QRect rect,
		const StatusHint &hint,
		const Ink &ink,
		bool rtl,
		bool labelFirst = false) {
	const auto align = rtl ? Qt::AlignRight : Qt::AlignLeft;
	const auto space = ink.width(QStringLiteral(" "));
	const auto keyW = ink.width(hint.keys, true);
	const auto labelW = ink.width(hint.label);
	if (labelFirst) {
		Text(p, TakeEdge(rect, labelW, rtl), hint.label, ink.regular, ink.muted, align);
		TakeEdge(rect, space, rtl);
		Text(p, rect, hint.keys, ink.bold, ink.text, align);
	} else {
		Text(p, TakeEdge(rect, keyW, rtl), hint.keys, ink.bold, ink.text, align);
		if (!hint.keys.isEmpty() && !hint.label.isEmpty()) {
			TakeEdge(rect, space, rtl);
		}
		Text(p, rect, hint.label, ink.regular, ink.muted, align);
	}
}

inline void Hints(
		QPainter &p,
		QRect space,
		const std::vector<StatusHint> &hints,
		const Ink &ink,
		bool rtl,
		bool trailing,
		bool labelFirst = false) {
	const auto separator = QStringLiteral(" · ");
	const auto sepW = ink.width(separator);
	const auto fromRight = (rtl != trailing);
	for (auto i = size_t(0); i != hints.size(); ++i) {
		const auto &hint = hints[trailing ? hints.size() - 1 - i : i];
		const auto width = HintWidth(hint, ink);
		if (space.width() < width + (i ? sepW : 0)) {
			break;
		}
		if (i) {
			Text(p, TakeEdge(space, sepW, fromRight), separator, ink.regular, ink.muted, Qt::AlignHCenter);
		}
		Hint(p, TakeEdge(space, width, fromRight), hint, ink, rtl, labelFirst);
	}
}

} // namespace StatusPaint

inline void PaintModalStatusBar(
		QPainter &p,
		QRect bar,
		const StatusColumns &columns,
		const StatusState &state,
		const StatusPalette &palette,
		const QFont &regular,
		const QFont &bold,
		const QFont &modeFont,
		QColor modeAccent,
		bool rtl) {
	using namespace StatusPaint;

	const auto rail = Across(columns.rail, bar);
	const auto dialogs = Across(columns.dialogs, bar);
	const auto chat = Across(columns.chat, bar);
	const auto info = Across(columns.info, bar);
	const auto railBg = Surface(palette.rail, palette.window);
	const auto dialogsBg = Surface(palette.dialogs, palette.window);
	const auto chatBg = Surface(palette.chat, palette.window);
	const auto infoBg = Surface(palette.info, palette.window);
	p.fillRect(bar, palette.window);
	p.fillRect(rail, railBg);
	p.fillRect(dialogs, dialogsBg);
	p.fillRect(chat, chatBg);
	p.fillRect(info, infoBg);
	p.fillRect(Across(columns.dialogsDivider, bar), palette.divider);
	p.fillRect(Across(columns.infoDivider, bar), palette.divider);

	const auto makeInk = [&](QColor bg) {
		return Ink{
			.regular = regular,
			.bold = bold,
			.text = ReadableText(Surface(palette.text, bg), bg),
			.muted = ReadableText(Surface(palette.muted, bg), bg),
			.alert = ReadableText(Surface(palette.error, bg), bg),
			.accent = ReadableText(Surface(palette.accent, bg), bg),
		};
	};
	const auto align = rtl ? Qt::AlignRight : Qt::AlignLeft;
	const auto badge = state.badgeText.isEmpty()
		? QStringLiteral("NORMAL")
		: state.badgeText;
	const auto pad = QFontMetrics(regular).horizontalAdvance(QStringLiteral("  "));
	const auto inset = [&](QRect rect) {
		const auto side = std::min(pad, rect.width() / 4);
		return rect.adjusted(side, 0, -side, 0);
	};
	auto dialogsText = TextArea(dialogs, columns.dialogsLeadingInset, pad, rtl);
	const auto hasChat = !chat.isEmpty();
	auto chatText = hasChat
		? TextArea(chat, columns.chatLeadingInset.value_or(pad),
			columns.chatTrailingInset.value_or(pad), rtl)
		: dialogs.isEmpty() ? inset(bar) : dialogsText;
	const auto ink = makeInk(hasChat ? chatBg : dialogs.isEmpty() ? palette.window : dialogsBg);

	if (!rail.isEmpty()) {
		Text(p, inset(rail), badge, modeFont,
			ReadableText(modeAccent, railBg), Qt::AlignHCenter);
	} else {
		auto &space = (!dialogs.isEmpty() && hasChat) ? dialogsText : chatText;
		const auto bg = !dialogs.isEmpty() ? dialogsBg : hasChat ? chatBg : palette.window;
		const auto modeWidth = TextWidth(modeFont, QStringLiteral("MESSAGES"));
		const auto badgeRect = TakeEdge(space, std::min(modeWidth, space.width() / 2), rtl);
		Text(p, badgeRect, badge, modeFont, ReadableText(modeAccent, bg), align);
		TakeEdge(space, pad, rtl);
	}

	const auto counts = UnreadStatusHints(state);
	const auto dialogsInk = makeInk(dialogsBg);
	auto pane = [&](QRect &space, const Ink &colors, int budget) {
		const auto titleW = std::min(budget, colors.width(state.paneLabel, true));
		Text(p, TakeEdge(space, titleW, rtl), state.paneLabel, bold, colors.text, align);
		const auto local = state.localleaderKey + QStringLiteral(" local");
		const auto localWidth = colors.width(local);
		if (!state.localleaderKey.isEmpty() && localWidth + pad <= space.width()
				&& localWidth + pad <= budget - titleW) {
			TakeEdge(space, pad, rtl);
			Text(p, TakeEdge(space, localWidth, rtl), local, regular, colors.muted, align);
		}
	};
	const auto paneInDialogs = hasChat && !dialogsText.isEmpty() && state.paneInDialogs;
	if (hasChat && !dialogsText.isEmpty()) {
		if (paneInDialogs) {
			pane(dialogsText, dialogsInk, counts.empty() ? dialogsText.width() : dialogsText.width() / 2);
			TakeEdge(dialogsText, pad, rtl);
		}
		Hints(p, dialogsText, counts, dialogsInk, rtl, false, true);
	}

	const auto hasMessage = !state.message.isEmpty();
	const auto beforePane = chatText.width();
	if (!paneInDialogs) {
		const auto primaryWidth = (hasMessage ? ink.width(state.message) + pad : 0)
			+ (state.pendingText.isEmpty() ? 0 : ink.width(state.pendingText, true) + pad);
		const auto available = std::max(0, chatText.width() - primaryWidth - pad);
		if (ink.width(state.paneLabel, true) <= available) {
			pane(chatText, ink, available);
		}
	}
	if (chatText.width() != beforePane) {
		TakeEdge(chatText, pad, rtl);
	}
	const auto drawLeading = [&](const QString &value, bool strong, QColor color, int limit) {
		if (value.isEmpty()) {
			return;
		}
		const auto run = TakeEdge(chatText, std::min(limit, ink.width(value, strong)), rtl);
		Text(p, run, value, strong ? bold : regular, color, align);
		if (!run.isEmpty()) {
			TakeEdge(chatText, pad, rtl);
		}
	};
	drawLeading(state.pendingText, true, ink.accent,
		hasMessage ? chatText.width() / 2 : chatText.width());
	drawLeading(state.message, false, state.messageIsError ? ink.alert : ink.text,
		chatText.width());
	drawLeading(state.pendingGroup, false, ink.muted, chatText.width());
	if (state.zenMode) {
		drawLeading(QStringLiteral("ZEN"), true, ink.accent, chatText.width());
	}
	if (!hasChat && !dialogs.isEmpty() && !counts.empty()
		&& (state.badge == BadgeKind::Normal || state.badge == BadgeKind::Insert)
		&& state.pendingText.isEmpty() && !hasMessage) {
		Hints(p, chatText, counts, dialogsInk, rtl, false, true);
	} else {
		Hints(p, chatText, state.hints, ink, rtl, true);
	}
}

} // namespace Purple::ModalUi
