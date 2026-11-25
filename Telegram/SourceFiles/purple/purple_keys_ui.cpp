#include "purple/purple_keys_ui.h"

#include "base/event_filter.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "main/main_session.h"
#include <crl/crl_time.h>
#include "ui/controls/keys_legend.h"
#include "ui/painter.h"
#include "ui/round_rect.h"
#include "ui/style/style_core.h"
#include "ui/text/text.h"
#include "ui/userpic_view.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/scroll_area.h"

#include "styles/style_boxes.h"
#include "styles/style_keys_legend.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetricsF>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QTextBlock>
#include <QtGui/QTextLayout>
#include <QtWidgets/QTextEdit>

#ifdef Q_OS_MAC
#include <Carbon/Carbon.h>
#endif // Q_OS_MAC

#include <algorithm>
#include <cmath>
#include <map>

namespace Purple::ModalUi {
namespace {

#ifdef Q_OS_MAC
constexpr auto kCtrlMod = Qt::MetaModifier;
#else
constexpr auto kCtrlMod = Qt::ControlModifier;
#endif // Q_OS_MAC

[[nodiscard]] Ui::KeysLegend::St CapStyle() {
	return {
		.titleStyle = &st::keysLegendTitleStyle,
		.titleFg = &st::keysLegendTitleFg,
		.titleSkip = st::keysLegendTitleSkip,
		.rowSkip = st::keysLegendRowSkip,
		.capStyle = &st::keysLegendCapStyle,
		.capHeight = st::keysLegendCapHeight,
		.capMinWidth = st::keysLegendCapMinWidth,
		.capMaxWidth = st::keysLegendCapMaxWidth,
		.capPadding = st::keysLegendCapPadding,
		.capRadius = st::keysLegendCapRadius,
		.capSkip = st::keysLegendCapSkip,
		.capBg = &st::keysLegendCapBg,
		.capBorder = &st::keysLegendCapBorder,
		.capFg = &st::keysLegendCapFg,
		.textSkip = st::keysLegendTextSkip,
		.textWidth = st::keysLegendTextWidth,
		.textMinWidth = st::keysLegendTextMinWidth,
		.textFg = &st::keysLegendTextFg,
	};
}

[[nodiscard]] QFont MonoFont(int pixelSize, bool bold = false) {
	auto f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	f.setPixelSize(pixelSize);
	f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
	return f;
}

[[nodiscard]] bool IsRtl(const QWidget *) {
	return style::RightToLeft();
}

[[nodiscard]] int MeasureTextWidth(const QFont &font, const QString &text) {
	if (text.isEmpty()) {
		return 0;
	}
	const auto fmF = QFontMetricsF(font);
	const auto fm = QFontMetrics(font);
	return std::max(
		int(std::ceil(fmF.horizontalAdvance(text))),
		fm.horizontalAdvance(text)) + 2;
}

[[nodiscard]] QString ElideText(
		const QFont &font,
		const QString &text,
		int maxWidth,
		Qt::TextElideMode mode = Qt::ElideRight) {
	if (maxWidth <= 0 || text.isEmpty()) {
		return QString();
	}
	const auto fmF = QFontMetricsF(font);
	if (int(std::ceil(fmF.horizontalAdvance(text))) <= maxWidth) {
		return text;
	}
	return QFontMetrics(font).elidedText(text, mode, maxWidth);
}

[[nodiscard]] QColor BadgeColor(BadgeKind kind) {
	switch (kind) {
	case BadgeKind::Normal: return QColor(0x2f, 0x7d, 0x44);
	case BadgeKind::Insert: return QColor(0x25, 0x63, 0xeb);
	case BadgeKind::Leader: return QColor(0x6d, 0x28, 0xd9);
	case BadgeKind::Local: return QColor(0x0f, 0x76, 0x6e);
	case BadgeKind::Hints: return QColor(0x7c, 0x3a, 0xed);
	case BadgeKind::Palette: return QColor(0x6d, 0x28, 0xd9);
	case BadgeKind::DescribeKey: return QColor(0xb4, 0x53, 0x09);
	case BadgeKind::Help: return QColor(0x03, 0x69, 0xa1);
	}
	return QColor(0x2f, 0x7d, 0x44);
}

[[nodiscard]] QColor BlendColor(QColor a, QColor b, double ratio) {
	const auto inv = 1.0 - ratio;
	return QColor(
		int(a.red() * inv + b.red() * ratio),
		int(a.green() * inv + b.green() * ratio),
		int(a.blue() * inv + b.blue() * ratio),
		255);
}

[[nodiscard]] int MeasureChordsWidth(
		const std::vector<QStringList> &chords,
		const Ui::KeysLegend::St &st,
		const QFontMetrics &sepMetrics) {
	if (chords.empty()) {
		return 0;
	}
	const auto sepW = sepMetrics.horizontalAdvance(u" / "_q);
	auto total = 0;
	for (auto c = size_t(0); c < chords.size(); ++c) {
		if (c > 0) {
			total += sepW;
		}
		for (auto i = 0; i < chords[c].size(); ++i) {
			if (i > 0) {
				total += st.capSkip;
			}
			total += Ui::KeysLegend::MakeCap(chords[c][i], st).width;
		}
	}
	return total;
}

int PaintChords(
		QPainter &p,
		const std::vector<QStringList> &chords,
		int x,
		int centerY,
		const Ui::KeysLegend::St &st) {
	const auto sepFont = st::normalFont;
	const auto sepMetrics = QFontMetrics(sepFont);
	const auto sepW = sepMetrics.horizontalAdvance(u" / "_q);
	auto cursor = x;
	for (auto c = size_t(0); c < chords.size(); ++c) {
		if (c > 0) {
			p.setFont(sepFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(cursor, centerY - st.capHeight / 2, sepW, st.capHeight),
				Qt::AlignCenter,
				u" / "_q);
			cursor += sepW;
		}
		for (auto i = 0; i < chords[c].size(); ++i) {
			if (i > 0) {
				cursor += st.capSkip;
			}
			const auto cap = Ui::KeysLegend::MakeCap(chords[c][i], st);
			Ui::KeysLegend::PaintCap(
				p,
				cap,
				cursor,
				centerY - cap.height / 2,
				st);
			cursor += cap.width;
		}
	}
	return cursor - x;
}

int PhysicalKeyForNav(const QKeyEvent &event) {
	const auto logical = (event.key() == Qt::Key_Enter)
		? Qt::Key_Return
		: event.key();
#ifdef Q_OS_MAC
	if (event.timestamp() && event.modifiers().testFlag(Qt::AltModifier)) {
		switch (event.nativeVirtualKey()) {
		case kVK_ANSI_P: return Qt::Key_P;
		case kVK_ANSI_N: return Qt::Key_N;
		case kVK_ANSI_X: return Qt::Key_X;
		default: break;
		}
	}
#endif // Q_OS_MAC
	return logical;
}

void PaintHighlightedLabel(
		QPainter &p,
		const QRect &rect,
		const QString &label,
		const std::vector<ModalMx::HighlightSpan> &spans,
		const QFont &baseFont,
		const QFont &boldFont,
		const QColor &baseColor,
		const QColor &highlightColor) {
	if (spans.empty()) {
		p.setFont(baseFont);
		p.setPen(baseColor);
		const auto elided = ElideText(baseFont, label, rect.width());
		p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
		return;
	}
	auto x = rect.left();
	const auto maxX = rect.right();
	auto cursor = 0;
	const auto drawChunk = [&](const QString &text, bool hi) {
		if (text.isEmpty() || x >= maxX) {
			return;
		}
		const auto &font = hi ? boldFont : baseFont;
		const auto avail = maxX - x;
		const auto piece = ElideText(font, text, avail);
		const auto w = int(std::ceil(QFontMetricsF(font).horizontalAdvance(piece)));
		p.setFont(font);
		p.setPen(hi ? highlightColor : baseColor);
		p.drawText(QRect(x, rect.top(), avail, rect.height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			piece);
		x += w;
	};
	for (const auto &span : spans) {
		if (span.start > cursor) {
			drawChunk(label.mid(cursor, span.start - cursor), false);
		}
		drawChunk(label.mid(span.start, span.length), true);
		cursor = span.start + span.length;
	}
	if (cursor < label.size()) {
		drawChunk(label.mid(cursor), false);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// ModalStatusBar
// ---------------------------------------------------------------------------

ModalStatusBar::ModalStatusBar(not_null<QWidget*> parent)
: RpWidget(parent) {
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);
}

void ModalStatusBar::setState(StatusState state) {
	_state = std::move(state);
	update();
}

void ModalStatusBar::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto r = rect();
	const auto rtl = IsRtl(this);

	const auto bg = BlendColor(st::windowBg->c, st::windowBgOver->c, 0.55);
	p.fillRect(r, bg);
	p.fillRect(0, 0, r.width(), 1, st::boxDividerBg->c);

	const auto badgeFont = MonoFont(11, true);
	const auto badgeFm = QFontMetrics(badgeFont);
	const auto badgeText = _state.badgeText.isEmpty()
		? u"NORMAL"_q
		: _state.badgeText;
	const auto badgeW = std::max(72, badgeFm.horizontalAdvance(badgeText) + 22);
	const auto badgeRect = rtl
		? QRect(r.width() - badgeW, 1, badgeW, r.height() - 1)
		: QRect(0, 1, badgeW, r.height() - 1);
	p.fillRect(badgeRect, BadgeColor(_state.badge));
	p.setFont(badgeFont);
	p.setPen(Qt::white);
	p.drawText(badgeRect, Qt::AlignCenter, badgeText);

	auto leftX = rtl ? 12 : (badgeW + 12);
	auto rightX = rtl ? (r.width() - badgeW - 12) : (r.width() - 12);

	const auto uiFont = st::normalFont;
	const auto uiBold = st::semiboldFont;
	const auto monoFont = MonoFont(11, false);
	const auto monoBold = MonoFont(11, true);

	// Right-side quiet discovery cue (layer-specific when a layer is open).
	const auto cue = !_state.rightCue.isEmpty()
		? _state.rightCue
		: (_state.localleaderHint.isEmpty()
			? u"SPC leader  ·  , local  ·  M-x run  ·  ? help"_q
			: (_state.localleaderHint + u"  ·  M-x run  ·  ? help"_q));
	const auto cueFm = QFontMetrics(uiFont);
	const auto cueW = cueFm.horizontalAdvance(cue);
	const auto minMiddle = _state.rightCue.isEmpty() ? 240 : 120;
	if (rightX - leftX > cueW + minMiddle) {
		p.setFont(uiFont);
		p.setPen(st::windowSubTextFg);
		const auto cueRect = rtl
			? QRect(leftX, 1, cueW, r.height() - 1)
			: QRect(rightX - cueW, 1, cueW, r.height() - 1);
		p.drawText(cueRect, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), cue);
		if (rtl) {
			leftX += cueW + 14;
		} else {
			rightX -= cueW + 14;
		}
	}

	auto cursor = rtl ? rightX : leftX;
	const auto advance = [&](int w, int gap = 10) {
		if (rtl) {
			cursor -= w + gap;
		} else {
			cursor += w + gap;
		}
	};
	const auto slotRect = [&](int w) {
		return rtl
			? QRect(cursor - w, 1, w, r.height() - 1)
			: QRect(cursor, 1, w, r.height() - 1);
	};
	const auto remaining = [&] {
		return std::max(0, rtl ? (cursor - leftX) : (rightX - cursor));
	};

	// Pane label.
	if (!_state.paneLabel.isEmpty() && remaining() > 40) {
		const auto fm = QFontMetrics(uiBold);
		const auto w = std::min(remaining(), fm.horizontalAdvance(_state.paneLabel));
		p.setFont(uiBold);
		p.setPen(st::windowFg);
		p.drawText(slotRect(w), Qt::AlignVCenter | Qt::AlignLeft, _state.paneLabel);
		advance(w, 8);
	}

	// Context chip (signals active localleader scope).
	if (!_state.contextChip.isEmpty() && remaining() > 56) {
		const auto fm = QFontMetrics(monoFont);
		const auto textW = fm.horizontalAdvance(_state.contextChip);
		const auto chipW = std::min(remaining(), textW + 14);
		const auto chipRect = slotRect(chipW).adjusted(0, 4, 0, -4);
		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(st::windowBg->c);
		p.drawRoundedRect(QRectF(chipRect).adjusted(.5, .5, -.5, -.5), 4, 4);
		p.setFont(monoFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(chipRect, Qt::AlignCenter, _state.contextChip);
		advance(chipW, 10);
	}

	// Compact ZEN mode indicator pill.
	if (_state.zenMode && remaining() > 44) {
		const auto zenText = u"ZEN"_q;
		const auto zenW = MeasureTextWidth(monoBold, zenText) + 10;
		if (zenW <= remaining()) {
			const auto zenRect = slotRect(zenW).adjusted(0, 4, 0, -4);
			auto fill = BadgeColor(BadgeKind::Leader);
			fill.setAlpha(30);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(zenRect, 4, 4);
			p.setFont(monoBold);
			p.setPen(BadgeColor(BadgeKind::Leader));
			p.drawText(zenRect, Qt::AlignCenter, zenText);
			advance(zenW, 8);
		}
	}

	// Unread unmuted CHATS breakdown pill: DM / GP / CH.
	if (_state.unreadKnown && _state.pendingText.isEmpty() && remaining() > 120) {
		struct BucketPart {
			QString text;
			bool active = false;
		};
		const std::array<BucketPart, 3> parts = { {
			{ u"DM "_q + QString::number(_state.unreadDm), _state.unreadDm > 0 },
			{ u"GP "_q + QString::number(_state.unreadGp), _state.unreadGp > 0 },
			{ u"CH "_q + QString::number(_state.unreadCh), _state.unreadCh > 0 },
		} };
		const auto sep = u" · "_q;
		const auto sepW = MeasureTextWidth(monoFont, sep) - 2;
		auto innerW = 0;
		std::array<int, 3> partW = {};
		for (auto i = 0; i < 3; ++i) {
			partW[i] = MeasureTextWidth(parts[i].active ? monoBold : monoFont, parts[i].text) - 1;
			innerW += partW[i];
			if (i > 0) {
				innerW += sepW;
			}
		}
		const auto pillW = innerW + 14;
		if (pillW <= remaining()) {
			const auto pillRect = slotRect(pillW).adjusted(0, 4, 0, -4);
			p.setPen(QPen(st::boxDividerBg->c, 1));
			p.setBrush(st::windowBg->c);
			p.drawRoundedRect(QRectF(pillRect).adjusted(.5, .5, -.5, -.5), 4, 4);
			auto px = pillRect.left() + 7;
			for (auto i = 0; i < 3; ++i) {
				if (i > 0) {
					p.setFont(monoFont);
					p.setPen(st::windowSubTextFg);
					p.drawText(QRect(px, pillRect.top(), sepW, pillRect.height()), Qt::AlignCenter, sep);
					px += sepW;
				}
				p.setFont(parts[i].active ? monoBold : monoFont);
				p.setPen(parts[i].active ? st::windowActiveTextFg : st::windowSubTextFg);
				p.drawText(
					QRect(px, pillRect.top(), partW[i], pillRect.height()),
					Qt::AlignCenter,
					parts[i].text);
				px += partW[i];
			}
			advance(pillW, 10);
		}
	}

	// 0ms live pending count/chord echo chip.
	if (!_state.pendingText.isEmpty() && remaining() > 48) {
		const auto fm = QFontMetrics(monoBold);
		const auto pillText = _state.pendingText;
		const auto pillW = std::min(remaining(), fm.horizontalAdvance(pillText) + 14);
		const auto pillRect = slotRect(pillW).adjusted(0, 4, 0, -4);
		auto accentFill = st::windowActiveTextFg->c;
		accentFill.setAlpha(28);
		p.setPen(Qt::NoPen);
		p.setBrush(accentFill);
		p.drawRoundedRect(pillRect, 4, 4);
		p.setFont(monoBold);
		p.setPen(st::windowActiveTextFg);
		p.drawText(pillRect, Qt::AlignCenter, pillText);
		advance(pillW, 8);

		if (!_state.pendingGroup.isEmpty() && remaining() > 48) {
			const auto grp = u"+"_q + _state.pendingGroup;
			const auto gfm = QFontMetrics(uiBold);
			const auto gw = std::min(remaining(), gfm.horizontalAdvance(grp));
			p.setFont(uiBold);
			p.setPen(st::windowActiveTextFg);
			p.drawText(slotRect(gw), Qt::AlignVCenter | Qt::AlignLeft, grp);
			advance(gw, 10);
		}
	}

	// Transient feedback or validation warning.
	if (!_state.message.isEmpty() && remaining() > 40) {
		const auto fm = QFontMetrics(uiFont);
		const auto avail = remaining();
		const auto elided = fm.elidedText(_state.message, Qt::ElideRight, avail);
		p.setFont(uiFont);
		p.setPen(_state.messageIsError
			? QColor(0xd9, 0x3b, 0x3b)
			: st::windowFg->c);
		p.drawText(slotRect(avail), Qt::AlignVCenter | Qt::AlignLeft, elided);
	}
}

// ---------------------------------------------------------------------------
// WhichKeyOverlay
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] int MeasureWhichKeyCapsWidth(
		const QStringList &keys,
		const Ui::KeysLegend::St &st,
		const QFontMetrics &sepFm) {
	if (keys.isEmpty()) {
		return 0;
	}
	const auto sepW = sepFm.horizontalAdvance(u"/"_q);
	auto total = 0;
	for (auto i = 0; i < keys.size(); ++i) {
		if (i > 0) {
			total += sepW + 4;
		}
		total += Ui::KeysLegend::MakeCap(keys[i], st).width;
	}
	return total;
}

void PaintWhichKeyCaps(
		QPainter &p,
		const QStringList &keys,
		int x,
		int centerY,
		const Ui::KeysLegend::St &st,
		const QFontMetrics &sepFm) {
	const auto sepW = sepFm.horizontalAdvance(u"/"_q);
	auto cursor = x;
	for (auto i = 0; i < keys.size(); ++i) {
		if (i > 0) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(cursor + 2, centerY - st.capHeight / 2, sepW, st.capHeight),
				Qt::AlignCenter,
				u"/"_q);
			cursor += sepW + 4;
		}
		const auto cap = Ui::KeysLegend::MakeCap(keys[i], st);
		Ui::KeysLegend::PaintCap(p, cap, cursor, centerY - cap.height / 2, st);
		cursor += cap.width;
	}
}

} // namespace

WhichKeyOverlay::WhichKeyOverlay(not_null<QWidget*> parent)
: RpWidget(parent) {
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

int WhichKeyOverlay::computeColumns(int availWidth) const {
	const auto usable = std::max(1, availWidth - 36);
	return std::clamp(usable / 235, 1, 4);
}

int WhichKeyOverlay::computeHeight(int availWidth, int maxHeight) const {
	constexpr auto kHeaderH = 38;
	constexpr auto kPadY = 12;
	constexpr auto kRowH = 26;
	constexpr auto kSectionGap = 13;
	const auto cols = computeColumns(availWidth);
	const auto groupRows = _state.groups.empty()
		? 0
		: (int(_state.groups.size()) + cols - 1) / cols;
	const auto leafRows = _state.leaves.empty()
		? 0
		: (int(_state.leaves.size()) + cols - 1) / cols;
	const auto gap = (groupRows > 0 && leafRows > 0) ? kSectionGap : 0;
	const auto totalRows = std::max(1, groupRows + leafRows);
	const auto desired = kHeaderH + 2 * kPadY + totalRows * kRowH + gap;
	return std::min(desired, std::max(kHeaderH + 2 * kPadY + kRowH, maxHeight));
}

void WhichKeyOverlay::setState(WhichKeyState state) {
	_state = std::move(state);
	update();
}

void WhichKeyOverlay::updateBounds(QRect availableAboveStatusBar) {
	if (_state.groups.empty() && _state.leaves.empty()) {
		hide();
		return;
	}
	const auto drawerW = std::min(availableAboveStatusBar.width(), 1120);
	const auto maxH = std::max(140, int(availableAboveStatusBar.height() * 0.44));
	const auto drawerH = computeHeight(drawerW, maxH);
	_columns = computeColumns(drawerW);
	_columnWidth = std::max(1, (drawerW - 36 - (_columns - 1) * 18) / _columns);

	const auto left = availableAboveStatusBar.x()
		+ (availableAboveStatusBar.width() - drawerW) / 2;
	const auto top = availableAboveStatusBar.y()
		+ availableAboveStatusBar.height() - drawerH;
	setGeometry(left, top, drawerW, drawerH);
	show();
	raise();
	update();
}

void WhichKeyOverlay::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto r = rect();
	const auto st = CapStyle();
	const auto sepFm = QFontMetrics(st::normalFont);
	const auto rtl = IsRtl(this);

	// Elevated rounded-top drawer surface.
	constexpr auto kRadius = 10.0;
	auto path = QPainterPath();
	path.moveTo(0, r.height());
	path.lineTo(0, kRadius);
	path.quadTo(0, 0, kRadius, 0);
	path.lineTo(r.width() - kRadius, 0);
	path.quadTo(r.width(), 0, r.width(), kRadius);
	path.lineTo(r.width(), r.height());
	path.closeSubpath();

	p.setPen(QPen(st::boxDividerBg->c, 1));
	p.setBrush(st::windowBg->c);
	p.drawPath(path);

	// Header strip.
	constexpr auto kHeaderH = 38;
	p.fillRect(1, kHeaderH - 1, r.width() - 2, 1, st::boxDividerBg->c);

	auto hx = 18;
	const auto hMidY = kHeaderH / 2;
	for (const auto &capText : _state.prefixCaps) {
		const auto cap = Ui::KeysLegend::MakeCap(capText, st);
		const auto drawX = rtl ? (r.width() - hx - cap.width) : hx;
		Ui::KeysLegend::PaintCap(p, cap, drawX, hMidY - cap.height / 2, st);
		hx += cap.width + st.capSkip;
	}
	hx += 6;

	if (!_state.title.isEmpty()) {
		p.setFont(st::semiboldFont);
		p.setPen(_state.isLocal ? BadgeColor(BadgeKind::Local) : st::windowActiveTextFg->c);
		const auto fm = QFontMetrics(st::semiboldFont);
		const auto w = fm.horizontalAdvance(_state.title);
		const auto drawX = rtl ? (r.width() - hx - w) : hx;
		p.drawText(QRect(drawX, 0, w, kHeaderH), Qt::AlignVCenter | Qt::AlignLeft, _state.title);
		hx += w + 10;
	}

	if (!_state.contextBadge.isEmpty()) {
		const auto bf = MonoFont(11, true);
		const auto bfm = QFontMetrics(bf);
		const auto bw = bfm.horizontalAdvance(_state.contextBadge) + 14;
		const auto bx = rtl ? (r.width() - hx - bw) : hx;
		const auto pill = QRect(bx, hMidY - 10, bw, 20);
		auto fill = BadgeColor(BadgeKind::Local);
		fill.setAlpha(28);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(pill, 4, 4);
		p.setFont(bf);
		p.setPen(BadgeColor(BadgeKind::Local));
		p.drawText(pill, Qt::AlignCenter, _state.contextBadge);
	}

	// Header right micro-hint.
	const auto hint = u"DEL back  ·  ESC cancel"_q;
	const auto hfm = QFontMetrics(st::normalFont);
	const auto hintW = hfm.horizontalAdvance(hint);
	if (r.width() - hx > hintW + 40) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto drawX = rtl ? 18 : (r.width() - 18 - hintW);
		p.drawText(QRect(drawX, 0, hintW, kHeaderH), Qt::AlignVCenter | Qt::AlignLeft, hint);
	}

	// Balanced multi-column continuation grid: groups block, divider, then leaf commands block.
	constexpr auto kPadX = 18;
	constexpr auto kPadY = 10;
	constexpr auto kRowH = 26;
	constexpr auto kColGap = 18;
	constexpr auto kSectionGap = 13;
	const auto bottomLimit = r.height() - kPadY;
	const auto arrowGlyph = rtl ? u"\u2190"_q : u"\u2192"_q;

	const auto paintBlock = [&](const std::vector<WhichKeyEntry> &list, int startY) -> int {
		if (list.empty()) {
			return startY;
		}
		const auto rows = (int(list.size()) + _columns - 1) / _columns;
		for (auto i = 0; i < int(list.size()); ++i) {
			const auto col = i / rows;
			const auto row = i % rows;
			const auto rowY = startY + row * kRowH;
			if (col >= _columns || rowY + kRowH > bottomLimit + 2) {
				continue;
			}
			const auto &entry = list[i];
			const auto colX = rtl
				? (r.width() - kPadX - (col + 1) * _columnWidth - col * kColGap)
				: (kPadX + col * (_columnWidth + kColGap));
			const auto midY = rowY + kRowH / 2;

			const auto capsW = MeasureWhichKeyCapsWidth(entry.keys, st, sepFm);
			const auto capSlotWidth = std::max(28, capsW);
			const auto capX = rtl
				? (colX + _columnWidth - capsW)
				: colX;
			PaintWhichKeyCaps(p, entry.keys, capX, midY, st, sepFm);

			const auto arrowX = rtl
				? (colX + _columnWidth - capSlotWidth - 18)
				: (colX + capSlotWidth + 6);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(QRect(arrowX, rowY, 14, kRowH), Qt::AlignCenter, arrowGlyph);

			const auto labelW = std::max(0, _columnWidth - capSlotWidth - 24);
			const auto labelX = rtl ? colX : (colX + capSlotWidth + 24);
			const auto text = entry.isGroup ? (u"+"_q + entry.label) : entry.label;
			const auto &font = entry.isGroup ? st::semiboldFont : st::normalFont;
			p.setFont(font);
			p.setPen(entry.isGroup ? st::windowActiveTextFg : st::windowFg);
			const auto elided = ElideText(font, text, labelW);
			p.drawText(
				QRect(labelX, rowY, labelW, kRowH),
				Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft),
				elided);
		}
		return startY + rows * kRowH;
	};

	auto curY = kHeaderH + kPadY;
	if (!_state.groups.empty()) {
		curY = paintBlock(_state.groups, curY);
		if (!_state.leaves.empty() && curY + kSectionGap + kRowH <= bottomLimit + 2) {
			p.fillRect(kPadX, curY + kSectionGap / 2, r.width() - 2 * kPadX, 1, st::boxDividerBg->c);
			curY += kSectionGap;
		}
	}
	if (!_state.leaves.empty()) {
		paintBlock(_state.leaves, curY);
	}
}

// ---------------------------------------------------------------------------
// ModalHelpLayer (? Curated Cheatsheet + TAB All Active Bindings)
// ---------------------------------------------------------------------------

namespace {

class CheatsheetGrid final : public Ui::RpWidget {
public:
	CheatsheetGrid(not_null<QWidget*> parent, std::vector<HelpSection> cards)
	: RpWidget(parent)
	, _cards(std::move(cards)) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		constexpr auto kPad = 20;
		constexpr auto kGap = 14;
		constexpr auto kCardPad = 14;
		constexpr auto kTitleH = 26;
		constexpr auto kRowH = 26;

		const auto avail = std::max(1, newWidth - 2 * kPad);
		_columns = std::clamp((avail + kGap) / (290 + kGap), 1, 3);
		_colWidth = std::max(1, (avail - (_columns - 1) * kGap) / _columns);

		auto heights = std::vector<int>(_columns, kPad);
		_layout.clear();
		_layout.reserve(_cards.size());
		for (const auto &card : _cards) {
			const auto cardH = kCardPad * 2 + kTitleH + 8
				+ int(card.rows.size()) * kRowH;
			auto bestCol = 0;
			for (auto c = 1; c < _columns; ++c) {
				if (heights[c] < heights[bestCol]) {
					bestCol = c;
				}
			}
			const auto x = kPad + bestCol * (_colWidth + kGap);
			const auto y = heights[bestCol];
			_layout.push_back(QRect(x, y, _colWidth, cardH));
			heights[bestCol] = y + cardH + kGap;
		}
		const auto maxH = heights.empty()
			? 2 * kPad
			: *std::max_element(heights.begin(), heights.end());
		return maxH + 6;
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto st = CapStyle();
		const auto sepFm = QFontMetrics(st::normalFont);
		const auto tagFont = MonoFont(10, false);
		const auto tagFm = QFontMetrics(tagFont);
		const auto cardBg = BlendColor(st::windowBg->c, st::windowBgOver->c, 0.25);
		const auto rtl = IsRtl(this);

		constexpr auto kCardPad = 14;
		constexpr auto kTitleH = 24;
		constexpr auto kRowH = 26;

		for (auto i = size_t(0); i < _cards.size() && i < _layout.size(); ++i) {
			const auto &card = _cards[i];
			const auto rect = rtl
				? QRect(width() - _layout[i].x() - _layout[i].width(), _layout[i].y(), _layout[i].width(), _layout[i].height())
				: _layout[i];
			p.setPen(QPen(st::boxDividerBg->c, 1));
			p.setBrush(cardBg);
			p.drawRoundedRect(QRectF(rect).adjusted(.5, .5, -.5, -.5), 10, 10);

			const auto inner = rect.adjusted(kCardPad, kCardPad - 2, -kCardPad, -kCardPad);
			p.setFont(st::keysLegendTitleStyle.font);
			p.setPen(st::windowActiveTextFg);
			p.drawText(
				QRect(inner.x(), inner.y(), inner.width(), kTitleH),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				card.title);
			p.fillRect(inner.x(), inner.y() + kTitleH + 2, inner.width(), 1, st::boxDividerBg->c);

			auto cardKeyCol = 82;
			for (const auto &row : card.rows) {
				cardKeyCol = std::max(
					cardKeyCol,
					MeasureChordsWidth(row.chords, st, sepFm) + 12);
			}
			cardKeyCol = std::min(cardKeyCol, std::max(82, int(inner.width() * 0.46)));

			auto rowY = inner.y() + kTitleH + 8;
			for (const auto &row : card.rows) {
				const auto midY = rowY + kRowH / 2;
				const auto usedW = MeasureChordsWidth(row.chords, st, sepFm);
				const auto colW = std::max(cardKeyCol, usedW + 10);
				if (rtl) {
					PaintChords(p, row.chords, inner.right() - usedW + 1, midY, st);
				} else {
					PaintChords(p, row.chords, inner.x(), midY, st);
				}

				auto tagReserve = 0;
				if (!row.contextTag.isEmpty()) {
					const auto tw = tagFm.horizontalAdvance(row.contextTag) + 10;
					const auto tagRect = rtl
						? QRect(inner.x(), midY - 9, tw, 18)
						: QRect(inner.right() - tw + 1, midY - 9, tw, 18);
					auto fill = BadgeColor(BadgeKind::Local);
					fill.setAlpha(24);
					p.setPen(Qt::NoPen);
					p.setBrush(fill);
					p.drawRoundedRect(tagRect, 4, 4);
					p.setFont(tagFont);
					p.setPen(BadgeColor(BadgeKind::Local));
					p.drawText(tagRect, Qt::AlignCenter, row.contextTag);
					tagReserve = tw + 8;
				}

				const auto textW = std::max(40, inner.width() - colW - tagReserve);
				const auto textX = rtl
					? (inner.x() + tagReserve)
					: (inner.x() + colW);
				p.setFont(st::normalFont);
				p.setPen(st::windowFg);
				const auto elided = ElideText(st::normalFont, row.label, textW);
				p.drawText(
					QRect(textX, rowY, textW, kRowH),
					(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
					elided);
				rowY += kRowH;
			}
		}
	}

private:
	std::vector<HelpSection> _cards;
	std::vector<QRect> _layout;
	int _columns = 2;
	int _colWidth = 300;
};

class ActiveBindingsList final : public Ui::RpWidget {
public:
	ActiveBindingsList(not_null<QWidget*> parent, std::vector<HelpSection> groups)
	: RpWidget(parent)
	, _allGroups(std::move(groups))
	, _filtered(_allGroups) {
	}

	void setFilter(const QString &query) {
		const auto tokens = ModalMx::ParseTokens(query);
		_filtered.clear();
		for (const auto &group : _allGroups) {
			auto kept = HelpSection{ .title = group.title };
			for (const auto &row : group.rows) {
				if (tokens.empty()) {
					kept.rows.push_back(row);
					continue;
				}
				auto keysFlat = QStringList();
				for (const auto &c : row.chords) {
					keysFlat.push_back(c.join(u' '));
				}
				const auto hay = (group.title + u' ' + row.label + u' '
					+ row.commandId + u' ' + keysFlat.join(u' ')).toCaseFolded();
				const auto init = ModalMx::WordInitials(row.label);
				const auto ok = std::all_of(
					tokens.begin(),
					tokens.end(),
					[&](const ModalMx::OrderlessToken &t) {
						return ModalMx::MatchToken(t, hay, init);
					});
				if (ok) {
					kept.rows.push_back(row);
				}
			}
			if (!kept.rows.empty()) {
				_filtered.push_back(std::move(kept));
			}
		}
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		constexpr auto kPadY = 14;
		constexpr auto kSectionH = 30;
		constexpr auto kRowH = 28;
		constexpr auto kSectionGap = 12;
		if (_filtered.empty()) {
			return 80;
		}
		auto h = kPadY;
		for (const auto &group : _filtered) {
			h += kSectionH + int(group.rows.size()) * kRowH + kSectionGap;
		}
		return h + kPadY;
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto st = CapStyle();
		const auto sepFm = QFontMetrics(st::normalFont);
		const auto monoFont = MonoFont(11, false);

		constexpr auto kPadX = 24;
		constexpr auto kSectionH = 30;
		constexpr auto kRowH = 28;
		constexpr auto kSectionGap = 12;
		constexpr auto kKeyColW = 168;

		if (_filtered.empty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, u"No matching bindings."_q);
			return;
		}

		auto y = 14;
		const auto rtl = IsRtl(this);
		const auto contentW = std::max(1, width() - 2 * kPadX);
		for (const auto &group : _filtered) {
			p.setFont(st::keysLegendTitleStyle.font);
			p.setPen(st::windowActiveTextFg);
			p.drawText(
				QRect(kPadX, y, contentW, kSectionH - 4),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				group.title);
			p.fillRect(kPadX, y + kSectionH - 3, contentW, 1, st::boxDividerBg->c);
			y += kSectionH;

			for (const auto &row : group.rows) {
				const auto midY = y + kRowH / 2;
				const auto usedW = MeasureChordsWidth(row.chords, st, sepFm);
				if (rtl) {
					PaintChords(p, row.chords, width() - kPadX - usedW, midY, st);
				} else {
					PaintChords(p, row.chords, kPadX, midY, st);
				}

				const auto idText = row.commandId.isEmpty()
					? QString()
					: (u":"_q + row.commandId);
				const auto idW = idText.isEmpty()
					? 0
					: std::min(280, MeasureTextWidth(monoFont, idText));
				const auto keyCol = std::max(kKeyColW, usedW + 14);
				const auto labelW = std::max(60, contentW - keyCol - (idW > 0 ? (idW + 14) : 0));
				const auto labelX = rtl
					? (kPadX + (idW > 0 ? (idW + 14) : 0))
					: (kPadX + keyCol);

				p.setFont(st::normalFont);
				p.setPen(st::windowFg);
				p.drawText(
					QRect(labelX, y, labelW, kRowH),
					(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
					ElideText(st::normalFont, row.label, labelW));

				if (!idText.isEmpty() && idW > 0) {
					p.setFont(monoFont);
					p.setPen(st::windowSubTextFg);
					p.drawText(
						QRect(rtl ? kPadX : (width() - kPadX - idW), y, idW, kRowH),
						(rtl ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
						ElideText(monoFont, idText, idW));
				}
				y += kRowH;
			}
			y += kSectionGap;
		}
	}

private:
	std::vector<HelpSection> _allGroups;
	std::vector<HelpSection> _filtered;
};

class ModalHelpLayer final : public Ui::LayerWidget {
public:
	explicit ModalHelpLayer(HelpData data)
	: _data(std::move(data))
	, _showAll(_data.startOnAllBindings)
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _filter(Ui::CreateChild<Ui::InputField>(
		this,
		st::defaultMultiSelectSearchField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Filter bindings (orderless, e.g. \"chat pin\")..."_q))) {
		setFocusPolicy(Qt::StrongFocus);
		_close->setClickedCallback([=] { closeLayer(); });
		_filter->changes() | rpl::on_next([=] {
			if (_bindingsWidget) {
				_bindingsWidget->setFilter(_filter->getLastText());
			}
		}, lifetime());
		base::install_event_filter(_filter->rawTextEdit(), [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::KeyPress) {
				return base::EventFilterResult::Continue;
			}
			const auto k = static_cast<QKeyEvent*>(e.get());
			const auto mods = k->modifiers() & ~Qt::KeypadModifier;
			if (k->key() == Qt::Key_Escape
				|| (k->key() == Qt::Key_G && mods == kCtrlMod)) {
				closeLayer();
				return base::EventFilterResult::Cancel;
			} else if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) {
				toggleTab();
				return base::EventFilterResult::Cancel;
			}
			return base::EventFilterResult::Continue;
		});
		rebuildContent();
	}

protected:
	bool focusNextPrevChild(bool) override {
		return false;
	}

	void doSetInnerFocus() override {
		if (_showAll) {
			_filter->setFocusFast();
		} else {
			setFocus();
		}
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto margin = (parentSize.width() < st::keysLegendCompactWidth) ? 8 : 32;
		const auto w = std::min(parentSize.width() - 2 * margin, _showAll ? 820 : 1020);
		const auto h = std::min(parentSize.height() - 2 * margin, 740);
		setGeometry(
			(parentSize.width() - w) / 2,
			(parentSize.height() - h) / 2,
			w,
			h);
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		layoutControls();
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (_tabSheetRect.contains(e->pos()) && _showAll) {
			toggleTab();
			return;
		}
		if (_tabAllRect.contains(e->pos()) && !_showAll) {
			toggleTab();
			return;
		}
		LayerWidget::mousePressEvent(e);
	}

	void keyPressEvent(QKeyEvent *e) override {
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
		const auto key = e->key();
		if (key == Qt::Key_Escape
			|| (key == Qt::Key_Q && !mods)
			|| (key == Qt::Key_G && mods == kCtrlMod)) {
			closeLayer();
			return;
		} else if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
			toggleTab();
			return;
		} else if ((key == Qt::Key_J || key == Qt::Key_Down) && !mods) {
			_scroll->scrollToY(_scroll->scrollTop() + 48);
			return;
		} else if ((key == Qt::Key_K || key == Qt::Key_Up) && !mods) {
			_scroll->scrollToY(std::max(0, _scroll->scrollTop() - 48));
			return;
		} else if (key == Qt::Key_Slash && !mods) {
			if (!_showAll) {
				toggleTab();
			}
			_filter->setFocusFast();
			return;
		}
		LayerWidget::keyPressEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		constexpr auto kHeaderH = 58;
		constexpr auto kFooterH = 34;
		const auto rtl = IsRtl(this);

		// Header title.
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		const auto titleText = _showAll
			? u"All Active Bindings"_q
			: u"Keyboard Cheatsheet"_q;
		const auto titleFm = QFontMetrics(st::keysLegendHeaderStyle.font);
		const auto titleW = titleFm.horizontalAdvance(titleText);
		const auto titleX = rtl ? (width() - 24 - titleW - 8) : 24;
		p.drawText(
			QRect(titleX, 0, titleW + 8, kHeaderH),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			titleText);

		// Segmented tabs in header.
		const auto tabFont = st::semiboldFont;
		const auto tabFm = QFontMetrics(tabFont);
		const auto t1 = u"Cheatsheet"_q;
		const auto t2 = u"All Bindings"_q;
		const auto w1 = tabFm.horizontalAdvance(t1) + 22;
		const auto w2 = tabFm.horizontalAdvance(t2) + 22;
		const auto tabsW = w1 + w2 + 6;
		const auto tabsX = rtl ? (_close->x() + _close->width() + 14) : (_close->x() - 14 - tabsW);
		const auto hasTabsRoom = rtl
			? (tabsX + tabsW <= titleX - 18)
			: (tabsX >= 24 + titleW + 18);
		if (hasTabsRoom) {
			const auto pillBg = QRect(tabsX, 14, tabsW, 30);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver->c);
			p.drawRoundedRect(pillBg, 8, 8);

			_tabSheetRect = rtl
				? QRect(tabsX + 3 + w2, 17, w1, 24)
				: QRect(tabsX + 3, 17, w1, 24);
			_tabAllRect = rtl
				? QRect(tabsX + 3, 17, w2, 24)
				: QRect(tabsX + 3 + w1, 17, w2, 24);

			const auto activeRect = _showAll ? _tabAllRect : _tabSheetRect;
			p.setBrush(st::windowBg->c);
			p.setPen(QPen(st::boxDividerBg->c, 1));
			p.drawRoundedRect(QRectF(activeRect).adjusted(.5, .5, -.5, -.5), 6, 6);

			p.setFont(tabFont);
			p.setPen(_showAll ? st::windowSubTextFg : st::windowActiveTextFg);
			p.drawText(_tabSheetRect, Qt::AlignCenter, t1);
			p.setPen(_showAll ? st::windowActiveTextFg : st::windowSubTextFg);
			p.drawText(_tabAllRect, Qt::AlignCenter, t2);
		} else {
			_tabSheetRect = {};
			_tabAllRect = {};
		}

		p.fillRect(0, kHeaderH - 1, width(), 1, st::boxDividerBg->c);

		// Search bar backdrop in All Bindings mode.
		if (_showAll) {
			const auto searchRect = QRect(22, kHeaderH + 8, std::max(120, width() - 44), 32);
			p.setPen(QPen(st::boxDividerBg->c, 1));
			p.setBrush(BlendColor(st::windowBg->c, st::windowBgOver->c, 0.35));
			p.drawRoundedRect(QRectF(searchRect).adjusted(.5, .5, -.5, -.5), 8, 8);
			p.fillRect(0, kHeaderH + 47, width(), 1, st::boxDividerBg->c);
		}

		// Footer strip.
		const auto footerRect = QRect(0, height() - kFooterH, width(), kFooterH);
		const auto footerBg = BlendColor(st::windowBg->c, st::windowBgOver->c, 0.45);
		p.fillRect(footerRect.adjusted(0, 1, 0, 0), footerBg);
		p.fillRect(0, height() - kFooterH, width(), 1, st::boxDividerBg->c);

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto leftHint = u"TAB switch view  ·  j/k scroll  ·  / filter  ·  q close"_q;
		p.drawText(
			QRect(rtl ? (width() / 2) : 20, height() - kFooterH, width() / 2 - 20, kFooterH),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			leftHint);

		if (!_data.keymapError.isEmpty() || !_data.keymapPath.isEmpty()) {
			auto prettyPath = _data.keymapPath;
			const auto home = QDir::homePath();
			if (!home.isEmpty() && prettyPath.startsWith(home + u'/')) {
				prettyPath = u"~"_q + prettyPath.mid(home.size());
			}
			const auto rightText = !_data.keymapError.isEmpty()
				? (u"keys.edn error (defaults active): "_q + _data.keymapError)
				: (u"Keys: "_q + prettyPath);
			const auto mono = MonoFont(11, false);
			p.setFont(mono);
			p.setPen(!_data.keymapError.isEmpty()
				? QColor(0xd9, 0x3b, 0x3b)
				: st::windowSubTextFg->c);
			const auto avail = std::max(60, width() / 2 - 24);
			p.drawText(
				QRect(rtl ? 20 : (width() - 20 - avail), height() - kFooterH, avail, kFooterH),
				(rtl ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
				ElideText(mono, rightText, avail, Qt::ElideMiddle));
		}
	}

private:
	void layoutControls() {
		constexpr auto kHeaderH = 58;
		constexpr auto kFilterH = 48;
		constexpr auto kFooterH = 34;
		if (IsRtl(this)) {
			_close->move(12, (kHeaderH - _close->height()) / 2);
		} else {
			_close->move(width() - 12 - _close->width(), (kHeaderH - _close->height()) / 2);
		}

		const auto bodyTop = _showAll ? (kHeaderH + kFilterH) : kHeaderH;
		_filter->setVisible(_showAll);
		if (_showAll) {
			_filter->setGeometry(34, kHeaderH + 8, std::max(120, width() - 68), 32);
			_filter->raise();
		}
		_scroll->setGeometry(0, bodyTop, width(), std::max(40, height() - bodyTop - kFooterH));
		if (auto *widget = _scroll->widget()) {
			widget->show();
			static_cast<Ui::RpWidget*>(widget)->resizeToWidth(_scroll->width());
		}
	}

	void toggleTab() {
		_showAll = !_showAll;
		rebuildContent();
		parentResized();
		layoutControls();
		doSetInnerFocus();
		if (_data.onTabChanged) {
			_data.onTabChanged(_showAll);
		}
		update();
	}

	void rebuildContent() {
		_bindingsWidget = nullptr;
		if (_showAll) {
			auto list = object_ptr<ActiveBindingsList>(_scroll, _data.activeBindingGroups);
			_bindingsWidget = list.data();
			_scroll->setOwnedWidget(std::move(list));
			if (!_filter->getLastText().isEmpty()) {
				_bindingsWidget->setFilter(_filter->getLastText());
			}
		} else {
			_scroll->setOwnedWidget(
				object_ptr<CheatsheetGrid>(_scroll, _data.cheatsheetCards));
		}
		_scroll->scrollToY(0);
	}

	HelpData _data;
	bool _showAll = false;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::IconButton *_close = nullptr;
	Ui::InputField *_filter = nullptr;
	QPointer<ActiveBindingsList> _bindingsWidget;
	QRect _tabSheetRect;
	QRect _tabAllRect;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

// ---------------------------------------------------------------------------
// DescribeLayer (SPC h k / SPC h f)
// ---------------------------------------------------------------------------

class DescribeLayer final : public Ui::LayerWidget {
public:
	explicit DescribeLayer(DescribeData data)
	: _data(std::move(data))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose)) {
		setFocusPolicy(Qt::StrongFocus);
		_close->setClickedCallback([=] { closeLayer(); });
	}

protected:
	void doSetInnerFocus() override {
		setFocus();
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 48, 540);
		auto h = 16 + 34 + 16; // top padding + header + body top padding
		if (_data.isKeyMode && !_data.queriedChord.isEmpty()) {
			h += 36;
		}
		h += 28; // title or unbound line
		if (_data.found) {
			h += 34; // availability pill
		}
		if (!_data.bindings.empty()) {
			h += 24 + int(_data.bindings.size()) * 28 + 10;
		} else if (_data.found) {
			h += 30;
		}
		if (!_data.elsewhere.empty()) {
			h += 24 + int(_data.elsewhere.size()) * 28 + 10;
		}
		if (_data.isKeyMode && _data.found && _data.onDescribeCommand) {
			h += 44; // footer divider + link
		} else {
			h += 18;
		}
		h = std::clamp(h, 196, std::min(580, parentSize.height() - 48));
		setGeometry(
			(parentSize.width() - w) / 2,
			std::max(36, int(parentSize.height() * 0.16)),
			w,
			h);
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		if (IsRtl(this)) {
			_close->move(10, 10);
		} else {
			_close->move(width() - 10 - _close->width(), 10);
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
		const auto key = e->key();
		if (key == Qt::Key_Escape
			|| (key == Qt::Key_Q && !mods)
			|| (key == Qt::Key_G && mods == kCtrlMod)) {
			closeLayer();
			return;
		} else if ((key == Qt::Key_Return || key == Qt::Key_Enter)
				&& _data.isKeyMode && _data.found && _data.onDescribeCommand) {
			auto cb = std::move(_data.onDescribeCommand);
			closeLayer();
			cb();
			return;
		}
		LayerWidget::keyPressEvent(e);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (_moreRect.contains(e->pos()) && _data.onDescribeCommand) {
			auto cb = std::move(_data.onDescribeCommand);
			closeLayer();
			cb();
			return;
		}
		LayerWidget::mousePressEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		const auto st = CapStyle();
		const auto sepFm = QFontMetrics(st::normalFont);
		const auto mono = MonoFont(12, false);
		const auto rtl = IsRtl(this);
		const auto hAlign = rtl ? Qt::AlignRight : Qt::AlignLeft;
		constexpr auto kPadX = 24;
		auto y = 16;

		// Header.
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(rtl ? (kPadX + 40) : kPadX, y, width() - 2 * kPadX - 40, 28),
			hAlign | Qt::AlignVCenter,
			_data.isKeyMode ? u"Describe Key"_q : u"Describe Command"_q);
		y += 34;
		p.fillRect(0, y, width(), 1, st::boxDividerBg->c);
		y += 16;

		// Key chord header in Describe Key mode.
		if (_data.isKeyMode && !_data.queriedChord.isEmpty()) {
			const auto cw = MeasureChordsWidth({ _data.queriedChord }, st, sepFm);
			PaintChords(p, { _data.queriedChord }, rtl ? (width() - kPadX - cw) : kPadX, y + st.capHeight / 2, st);
			y += st.capHeight + 14;
		}

		// Title + :command-id.
		if (_data.found) {
			p.setFont(st::semiboldFont);
			p.setPen(st::windowFg);
			const auto titleFm = QFontMetrics(st::semiboldFont);
			const auto tw = titleFm.horizontalAdvance(_data.title);
			p.drawText(QRect(kPadX, y, width() - 2 * kPadX, 24),
				hAlign | Qt::AlignVCenter,
				_data.title);
			if (!_data.commandId.isEmpty()) {
				p.setFont(mono);
				p.setPen(st::windowSubTextFg);
				const auto idRect = rtl
					? QRect(kPadX, y, std::max(40, width() - kPadX * 2 - tw - 10), 24)
					: QRect(kPadX + tw + 10, y, std::max(40, width() - kPadX * 2 - tw - 10), 24);
				p.drawText(idRect, hAlign | Qt::AlignVCenter, u":"_q + _data.commandId);
			}
			y += 28;
		} else {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(QRect(kPadX, y, width() - 2 * kPadX, 24),
				hAlign | Qt::AlignVCenter,
				u"This key sequence is not bound here."_q);
			y += 28;
		}

		// Availability pill.
		if (_data.found) {
			const auto statusColor = _data.availableHere
				? BadgeColor(BadgeKind::Normal)
				: BadgeColor(BadgeKind::DescribeKey);
			const auto statusText = _data.availableHere
				? u"Available here"_q
				: (_data.unavailableReason.isEmpty()
					? u"Unavailable in this view"_q
					: (u"Unavailable: "_q + _data.unavailableReason));
			auto fill = statusColor;
			fill.setAlpha(26);
			const auto fm = QFontMetrics(st::semiboldFont);
			const auto pw = std::min(width() - 2 * kPadX, fm.horizontalAdvance(statusText) + 18);
			const auto pill = QRect(rtl ? (width() - kPadX - pw) : kPadX, y, pw, 22);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(pill, 5, 5);
			p.setFont(st::semiboldFont);
			p.setPen(statusColor);
			p.drawText(pill, Qt::AlignCenter, statusText);
			y += 32;
		}

		const auto drawBindings = [&](const QString &heading, const std::vector<DescribeBinding> &list) {
			if (list.empty()) {
				return;
			}
			p.setFont(st::keysLegendTitleStyle.font);
			p.setPen(st::windowSubTextFg);
			p.drawText(QRect(kPadX, y, width() - 2 * kPadX, 20),
				hAlign | Qt::AlignVCenter,
				heading.toUpper());
			y += 24;
			for (const auto &b : list) {
				const auto cw = MeasureChordsWidth({ b.caps }, st, sepFm);
				const auto usedW = PaintChords(
					p,
					{ b.caps },
					rtl ? (width() - kPadX - cw) : kPadX,
					y + 13,
					st);
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				const auto whereW = std::max(40, width() - 2 * kPadX - usedW - 12);
				p.drawText(
					QRect(rtl ? kPadX : (kPadX + usedW + 12), y, whereW, 26),
					hAlign | Qt::AlignVCenter,
					b.where);
				y += 28;
			}
			y += 6;
		};

		if (!_data.bindings.empty()) {
			drawBindings(u"All bindings"_q, _data.bindings);
		} else if (_data.found) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(QRect(kPadX, y, width() - 2 * kPadX, 24),
				hAlign | Qt::AlignVCenter,
				_data.keylessOnPurpose
					? u"No key (on purpose)  ·  run via M-x or SPC SPC"_q
					: u"Currently unbound  ·  run via M-x or SPC SPC"_q);
			y += 28;
		}

		drawBindings(u"Elsewhere"_q, _data.elsewhere);

		_moreRect = {};
		if (_data.isKeyMode && _data.found && _data.onDescribeCommand) {
			const auto footerTop = std::max(y + 4, height() - 40);
			p.fillRect(0, footerTop, width(), 1, st::boxDividerBg->c);
			const auto linkText = u"Inspect full command details (RET)"_q;
			const auto fm = QFontMetrics(st::semiboldFont);
			const auto lw = fm.horizontalAdvance(linkText);
			_moreRect = QRect(rtl ? (width() - kPadX - lw - 8) : kPadX, footerTop + 8, lw + 8, 24);
			p.setFont(st::semiboldFont);
			p.setPen(st::windowActiveTextFg);
			p.drawText(_moreRect, hAlign | Qt::AlignVCenter, linkText);
		}
	}

private:
	DescribeData _data;
	Ui::IconButton *_close = nullptr;
	QRect _moreRect;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

// ---------------------------------------------------------------------------
// CommandPaletteLayer (M-x / SPC SPC / Describe Command Picker)
// ---------------------------------------------------------------------------

class PaletteRowsWidget final : public Ui::RpWidget {
public:
	PaletteRowsWidget(
		not_null<QWidget*> parent,
		std::vector<PaletteItem> items,
		Fn<void(int rowIndex)> onClick)
	: RpWidget(parent)
	, _items(std::move(items))
	, _onClick(std::move(onClick)) {
		setMouseTracking(true);
		auto seenSessions = QSet<Main::Session*>();
		for (const auto &it : _items) {
			if (const auto s = it.avatarSession.get(); s && !seenSessions.contains(s)) {
				seenSessions.insert(s);
				s->downloaderTaskFinished() | rpl::on_next([this] {
					update();
				}, lifetime());
			}
		}
	}

	static constexpr int kRowHeight = 42;

	void setResults(
			std::vector<int> filtered,
			std::vector<ModalMx::OrderlessToken> tokens,
			int selected) {
		_filtered = std::move(filtered);
		_tokens = std::move(tokens);
		_selected = selected;
		_hovered = -1;
		resize(width(), std::max(56, int(_filtered.size()) * kRowHeight + 8));
		update();
	}

	void setSelected(int selected) {
		_selected = selected;
		update();
	}

protected:
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto row = (e->pos().y() - 4) / kRowHeight;
		const auto next = (row >= 0 && row < int(_filtered.size())) ? row : -1;
		if (next != _hovered) {
			_hovered = next;
			setCursor((next >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEventHook(QEvent *e) override {
		Q_UNUSED(e);
		if (_hovered >= 0) {
			_hovered = -1;
			update();
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto row = (e->pos().y() - 4) / kRowHeight;
		if (row >= 0 && row < int(_filtered.size()) && _onClick) {
			_onClick(row);
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto st = CapStyle();
		const auto sepFm = QFontMetrics(st::normalFont);
		const auto monoFont = MonoFont(11, false);
		const auto monoBold = MonoFont(11, true);
		const auto rtl = IsRtl(this);

		if (_filtered.empty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, u"No matching commands."_q);
			return;
		}

		const auto clip = e->rect();
		const auto first = std::max(0, (clip.top() - 4) / kRowHeight);
		const auto till = std::min(int(_filtered.size()), (clip.bottom() - 4) / kRowHeight + 2);

		for (auto row = first; row < till; ++row) {
			const auto &item = _items[_filtered[row]];
			const auto y = 4 + row * kRowHeight;
			const auto rowRect = QRect(6, y + 1, width() - 12, kRowHeight - 2);
			const auto isActive = (row == _selected);
			const auto isHover = (row == _hovered);

			if (isActive || isHover) {
				auto fill = st::windowBgOver->c;
				if (!isActive) {
					fill.setAlpha(150);
				}
				p.setPen(Qt::NoPen);
				p.setBrush(fill);
				p.drawRoundedRect(rowRect, 7, 7);
				if (isActive) {
					const auto barRect = rtl
						? QRect(rowRect.right() - 2, rowRect.top() + 5, 3, rowRect.height() - 10)
						: QRect(rowRect.left(), rowRect.top() + 5, 3, rowRect.height() - 10);
					p.setBrush(st::windowActiveTextFg->c);
					p.drawRoundedRect(barRect, 1.5, 1.5);
				}
			}

			// Far-side slot: keycaps or unavailable reason pill.
			auto sideReserved = 14;
			const auto midY = rowRect.center().y() + 1;
			if (!item.available) {
				const auto reason = item.unavailableReason.isEmpty()
					? u"unavailable"_q
					: item.unavailableReason;
				const auto rw = std::min(240, MeasureTextWidth(monoFont, reason) + 12);
				const auto pill = rtl
					? QRect(rowRect.left() + 10, midY - 10, rw, 20)
					: QRect(rowRect.right() - 10 - rw, midY - 10, rw, 20);
				p.setPen(QPen(st::boxDividerBg->c, 1));
				p.setBrush(st::windowBg->c);
				p.drawRoundedRect(QRectF(pill).adjusted(.5, .5, -.5, -.5), 4, 4);
				p.setFont(monoFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(pill, Qt::AlignCenter, ElideText(monoFont, reason, rw - 10));
				sideReserved = rw + 22;
			} else if (!item.chords.empty()) {
				const auto shownChords = (item.chords.size() > 2)
					? std::vector<QStringList>(item.chords.begin(), item.chords.begin() + 2)
					: item.chords;
				const auto cw = MeasureChordsWidth(shownChords, st, sepFm);
				const auto cx = rtl
					? (rowRect.left() + 10)
					: (rowRect.right() - 10 - cw);
				PaintChords(p, shownChords, cx, midY, st);
				sideReserved = cw + 22;
			}

			auto avatarReserve = 0;
			if (const auto session = item.avatarSession.get();
				session && item.avatarPeerId != 0) {
				if (const auto peer = session->data().peerLoaded(item.avatarPeerId)) {
					constexpr auto kAvatarSize = 26;
					const auto key = std::pair(session, item.avatarPeerId);
					auto &view = _avatars[key];
					peer->loadUserpic();
					const auto ax = rtl
						? (rowRect.right() - 12 - kAvatarSize)
						: (rowRect.left() + 12);
					peer->paintUserpic(p, view, ax, midY - kAvatarSize / 2, kAvatarSize, true);
					avatarReserve = kAvatarSize + 10;
				}
			}

			const auto textW = std::max(80, rowRect.width() - sideReserved - 13 - avatarReserve);
			const auto leftX = rtl
				? (rowRect.left() + sideReserved)
				: (rowRect.left() + 13 + avatarReserve);
			const auto spans = ModalMx::LabelMatchSpans(item.label, _tokens);
			const auto labelColor = item.available
				? st::windowFg->c
				: st::windowSubTextFg->c;
			PaintHighlightedLabel(
				p,
				QRect(leftX, rowRect.top() + 4, textW, 18),
				item.label,
				spans,
				st::semiboldFont,
				st::semiboldFont,
				labelColor,
				st::windowActiveTextFg->c);

			const auto sub = item.category.isEmpty()
				? (u":"_q + item.id)
				: (item.category + u"  ·  :"_q + item.id);
			const auto subSpans = ModalMx::SubtitleMatchSpans(sub, _tokens);
			PaintHighlightedLabel(
				p,
				QRect(leftX, rowRect.top() + 22, textW, 15),
				sub,
				subSpans,
				monoFont,
				monoBold,
				st::windowSubTextFg->c,
				st::windowActiveTextFg->c);
		}
	}

private:
	std::vector<PaletteItem> _items;
	std::map<std::pair<Main::Session*, PeerId>, Ui::PeerUserpicView> _avatars;
	Fn<void(int)> _onClick;
	std::vector<int> _filtered;
	std::vector<ModalMx::OrderlessToken> _tokens;
	int _selected = 0;
	int _hovered = -1;
};

class CommandPaletteLayer final : public Ui::LayerWidget {
public:
	explicit CommandPaletteLayer(PaletteConfig config)
	: _config(std::move(config))
	, _input(Ui::CreateChild<Ui::InputField>(
		this,
		st::defaultMultiSelectSearchField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(_config.placeholder.isEmpty()
			? u"Type commands or initials (e.g. \"opm\")..."_q
			: _config.placeholder)))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
		_candidates.reserve(_config.items.size());
		for (auto i = 0; i < int(_config.items.size()); ++i) {
			const auto &it = _config.items[i];
			auto keysJoined = QStringList();
			for (const auto &c : it.chords) {
				keysJoined.push_back(c.join(u' '));
			}
			_candidates.push_back({
				.payloadIndex = i,
				.id = it.id,
				.label = it.label,
				.category = it.category,
				.keysText = keysJoined.join(u' '),
				.available = it.available,
			});
		}
		auto rows = object_ptr<PaletteRowsWidget>(
			_scroll,
			_config.items,
			[=](int rowIndex) { activateRow(rowIndex); });
		_rows = rows.data();
		_scroll->setOwnedWidget(std::move(rows));

		_input->changes() | rpl::on_next([=] {
			if (!_steppingHistory) {
				_historyPos = -1;
				_draft = _input->getLastText();
			}
			refilter();
		}, lifetime());
		_input->submits() | rpl::on_next([=](Qt::KeyboardModifiers) {
			activateRow(_selected);
		}, lifetime());

		base::install_event_filter(_input->rawTextEdit(), [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::KeyPress) {
				return base::EventFilterResult::Continue;
			}
			const auto k = static_cast<QKeyEvent*>(e.get());
			const auto layout = _input->rawTextEdit()->textCursor().block().layout();
			if (layout && !layout->preeditAreaText().isEmpty()) {
				return base::EventFilterResult::Continue;
			}
			const auto mods = k->modifiers() & ~Qt::KeypadModifier;
			const auto key = PhysicalKeyForNav(*k);
			if (key == Qt::Key_Escape
				|| (key == Qt::Key_G && mods == kCtrlMod)) {
				cancel();
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Down
				|| (key == Qt::Key_Tab && !mods)
				|| ((key == Qt::Key_N || key == Qt::Key_J) && mods == kCtrlMod)) {
				moveSelection(1);
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Up
				|| key == Qt::Key_Backtab
				|| (key == Qt::Key_Tab && mods == Qt::ShiftModifier)
				|| ((key == Qt::Key_P || key == Qt::Key_K) && mods == kCtrlMod)) {
				moveSelection(-1);
				return base::EventFilterResult::Cancel;
			} else if (mods == Qt::AltModifier && (key == Qt::Key_P || key == Qt::Key_N)) {
				stepHistory(key == Qt::Key_P ? 1 : -1);
				return base::EventFilterResult::Cancel;
			}
			return base::EventFilterResult::Continue;
		});

		refilter();
	}

protected:
	void doSetInnerFocus() override {
		_input->setFocusFast();
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 32, 640);
		constexpr auto kPromptH = 52;
		constexpr auto kFooterH = 30;
		const auto maxListH = std::min(380, std::max(160, int(parentSize.height() * 0.52)));
		const auto rowsH = std::max(
			56,
			int(_filtered.size()) * PaletteRowsWidget::kRowHeight + 8);
		const auto h = kPromptH + std::min(maxListH, rowsH) + kFooterH;
		// Anchor in upper third so the input bar never jumps vertically while typing.
		const auto top = std::clamp(int(parentSize.height() * 0.11), 28, 110);
		setGeometry(
			(parentSize.width() - w) / 2,
			top,
			w,
			std::min(h, parentSize.height() - top - 24));
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		constexpr auto kPromptH = 52;
		constexpr auto kFooterH = 30;
		const auto badgeW = promptBadgeWidth();
		const auto countW = 68;
		const auto inputW = std::max(120, width() - 28 - badgeW - 10 - countW);
		const auto inputX = IsRtl(this) ? (14 + countW) : (14 + badgeW + 10);
		_input->setGeometry(inputX, 10, inputW, 32);
		_scroll->setGeometry(
			0,
			kPromptH,
			width(),
			std::max(40, height() - kPromptH - kFooterH));
		_rows->resize(_scroll->width(), _rows->height());
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		// Crisp outer outline.
		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), st::boxRadius, st::boxRadius);

		constexpr auto kPromptH = 52;
		constexpr auto kFooterH = 30;
		const auto rtl = IsRtl(this);

		// Prompt pill.
		const auto badgeText = _config.promptBadge.isEmpty()
			? u"M-x"_q
			: _config.promptBadge;
		const auto badgeW = promptBadgeWidth();
		const auto pill = QRect(rtl ? (width() - 14 - badgeW) : 14, 14, badgeW, 24);
		auto accentFill = st::windowActiveTextFg->c;
		accentFill.setAlpha(28);
		p.setPen(Qt::NoPen);
		p.setBrush(accentFill);
		p.drawRoundedRect(pill, 6, 6);
		p.setFont(MonoFont(11, true));
		p.setPen(st::windowActiveTextFg);
		p.drawText(pill, Qt::AlignCenter, badgeText);

		// Live count readout.
		const auto countText = QString::number(_filtered.size())
			+ u"/"_q
			+ QString::number(_config.items.size());
		p.setFont(MonoFont(11, false));
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(rtl ? 14 : (width() - 78), 0, 64, kPromptH),
			(rtl ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
			countText);

		p.fillRect(0, kPromptH - 1, width(), 1, st::boxDividerBg->c);

		// Subtle footer strip.
		const auto footerRect = QRect(0, height() - kFooterH, width(), kFooterH);
		const auto footerBg = BlendColor(st::windowBg->c, st::windowBgOver->c, 0.45);
		p.fillRect(footerRect.adjusted(0, 1, 0, 0), footerBg);
		p.fillRect(0, height() - kFooterH, width(), 1, st::boxDividerBg->c);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			footerRect.adjusted(14, 0, -14, 0),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			u"C-n/p move  ·  RET run  ·  M-p/n history  ·  ESC close"_q);
	}

private:
	[[nodiscard]] int promptBadgeWidth() const {
		const auto text = _config.promptBadge.isEmpty()
			? u"M-x"_q
			: _config.promptBadge;
		return std::max(44, QFontMetrics(MonoFont(11, true)).horizontalAdvance(text) + 18);
	}

	void refilter() {
		const auto query = _input->getLastText();
		_filtered = ModalMx::RankAndFilter(_candidates, _config.history, query);
		_selected = 0;
		_rows->setResults(_filtered, ModalMx::ParseTokens(query), _selected);
		_scroll->scrollToY(0);
		if (parentWidget()) {
			parentResized();
		}
		update();
	}

	void moveSelection(int delta) {
		if (_filtered.empty()) {
			return;
		}
		const auto n = int(_filtered.size());
		_selected = (_selected + delta % n + n) % n;
		_rows->setSelected(_selected);
		const auto rowTop = 4 + _selected * PaletteRowsWidget::kRowHeight;
		const auto rowBottom = rowTop + PaletteRowsWidget::kRowHeight;
		const auto viewTop = _scroll->scrollTop();
		const auto viewBottom = viewTop + _scroll->height();
		if (rowTop < viewTop) {
			_scroll->scrollToY(rowTop);
		} else if (rowBottom > viewBottom) {
			_scroll->scrollToY(rowBottom - _scroll->height());
		}
	}

	void stepHistory(int delta) {
		const auto stepped = ModalMx::StepInputHistory(
			_config.history.inputs,
			_historyPos,
			delta,
			_draft);
		if (stepped.index == _historyPos) {
			return;
		}
		_historyPos = stepped.index;
		_steppingHistory = true;
		_input->setText(stepped.text);
		_input->selectAll();
		_steppingHistory = false;
		refilter();
	}

	void activateRow(int rowIndex) {
		if (_closing || rowIndex < 0 || rowIndex >= int(_filtered.size())) {
			return;
		}
		const auto &item = _config.items[_filtered[rowIndex]];
		if (!_config.describeMode && !item.available) {
			return;
		}
		_closing = true;
		const auto actionId = item.actionId;
		const auto query = _input->getLastText();
		auto cb = std::move(_config.onPick);
		_config.onCancel = nullptr;
		if (cb) {
			cb(actionId, query);
		} else {
			closeLayer();
		}
	}

	void cancel() {
		if (_closing) {
			return;
		}
		_closing = true;
		auto cb = std::move(_config.onCancel);
		_config.onPick = nullptr;
		if (cb) {
			cb();
		} else {
			closeLayer();
		}
	}

	PaletteConfig _config;
	std::vector<ModalMx::Candidate> _candidates;
	std::vector<int> _filtered;
	Ui::InputField *_input = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	QPointer<PaletteRowsWidget> _rows;
	int _selected = 0;
	int _historyPos = -1;
	QString _draft;
	bool _steppingHistory = false;
	bool _closing = false;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

// ---------------------------------------------------------------------------
// UnreadBufferLayer (SPC u / SPC b I — iBuffer for unread unmuted chats)
// ---------------------------------------------------------------------------

[[nodiscard]] QString FormatShortTimestamp(int64_t unixSec) {
	if (unixSec <= 0) {
		return QString();
	}
	const auto dt = QDateTime::fromSecsSinceEpoch(unixSec);
	const auto now = QDateTime::currentDateTime();
	if (dt.date() == now.date()) {
		return dt.toString(QStringLiteral("HH:mm"));
	}
	if (dt.daysTo(now) < 7) {
		return dt.toString(QStringLiteral("ddd"));
	}
	return dt.toString(QStringLiteral("MMM d"));
}

[[nodiscard]] QColor TopicColorFromId(int32 colorId) {
	if (colorId > 0) {
		return QColor((colorId >> 16) & 0xff, (colorId >> 8) & 0xff, colorId & 0xff);
	}
	return QColor(0x3b, 0x82, 0xf6);
}

class UnreadBufferRowsWidget final : public Ui::RpWidget {
public:
	UnreadBufferRowsWidget(
		not_null<QWidget*> parent,
		base::weak_ptr<Main::Session> session,
		Fn<void(int row)> onClick)
	: RpWidget(parent)
	, _session(std::move(session))
	, _onClick(std::move(onClick)) {
		setMouseTracking(true);
		if (const auto s = _session.get()) {
			s->downloaderTaskFinished() | rpl::on_next([this] {
				update();
			}, lifetime());
		}
	}

	static constexpr int kRowHeight = 46;

	void setData(
			const std::vector<ModalUnread::BufferItem> &items,
			const std::vector<int> &visible,
			const QSet<uint64_t> &marked,
			int selected) {
		_items = items;
		_visible = visible;
		_marked = marked;
		_selected = selected;
		_hovered = -1;
		resize(width(), std::max(72, int(_visible.size()) * kRowHeight + 8));
		update();
	}

	void setSelectionAndMarks(int selected, const QSet<uint64_t> &marked) {
		_selected = selected;
		_marked = marked;
		update();
	}

protected:
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto row = (e->pos().y() - 4) / kRowHeight;
		const auto next = (row >= 0 && row < int(_visible.size())) ? row : -1;
		if (next != _hovered) {
			_hovered = next;
			setCursor((next >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEventHook(QEvent *) override {
		if (_hovered >= 0) {
			_hovered = -1;
			update();
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto row = (e->pos().y() - 4) / kRowHeight;
		if (row >= 0 && row < int(_visible.size()) && _onClick) {
			_onClick(row);
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto rtl = IsRtl(this);
		const auto monoFont = MonoFont(10, true);
		const auto timeFont = MonoFont(10, false);

		if (_visible.empty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, u"No unread unmuted chats match."_q);
			return;
		}

		const auto clip = e->rect();
		const auto first = std::max(0, (clip.top() - 4) / kRowHeight);
		const auto till = std::min(int(_visible.size()), (clip.bottom() - 4) / kRowHeight + 2);
		const auto session = _session.get();

		for (auto row = first; row < till; ++row) {
			const auto &item = _items[_visible[row]];
			const auto y = 4 + row * kRowHeight;
			const auto rowRect = QRect(8, y + 1, width() - 16, kRowHeight - 2);
			const auto isActive = (row == _selected);
			const auto isHover = (row == _hovered);
			const auto isMarked = _marked.contains(item.peerId);

			if (isActive || isHover || isMarked) {
				auto fill = st::windowBgOver->c;
				if (!isActive && !isMarked) {
					fill.setAlpha(140);
				} else if (isMarked && !isActive) {
					fill = BlendColor(st::windowBg->c, st::windowActiveTextFg->c, 0.08);
				}
				p.setPen(Qt::NoPen);
				p.setBrush(fill);
				p.drawRoundedRect(rowRect, 7, 7);
				if (isActive) {
					const auto barRect = rtl
						? QRect(rowRect.right() - 2, rowRect.top() + 6, 3, rowRect.height() - 12)
						: QRect(rowRect.left(), rowRect.top() + 6, 3, rowRect.height() - 12);
					p.setBrush(st::windowActiveTextFg->c);
					p.drawRoundedRect(barRect, 1.5, 1.5);
				}
			}

			const auto midY = rowRect.center().y() + 1;

			// Mark gutter (* when marked).
			constexpr auto kMarkW = 18;
			const auto markX = rtl
				? (rowRect.right() - 8 - kMarkW)
				: (rowRect.left() + 8);
			if (isMarked) {
				p.setFont(MonoFont(13, true));
				p.setPen(st::windowActiveTextFg);
				p.drawText(QRect(markX, rowRect.top(), kMarkW, rowRect.height()), Qt::AlignCenter, u"*"_q);
			}

			// Circular profile picture (28 px).
			constexpr auto kAvatarSize = 28;
			const auto avatarX = rtl
				? (markX - 6 - kAvatarSize)
				: (markX + kMarkW + 6);
			if (session) {
				if (const auto peer = session->data().peerLoaded(PeerId(item.peerId))) {
					auto &view = _avatars[item.peerId];
					peer->loadUserpic();
					peer->paintUserpic(p, view, avatarX, midY - kAvatarSize / 2, kAvatarSize, true);
				}
			}

			// Far side: timestamp + unread count badge + mention indicator.
			auto farReserve = 12;
			const auto timeStr = FormatShortTimestamp(item.lastTimestamp);
			if (!timeStr.isEmpty()) {
				const auto tw = MeasureTextWidth(timeFont, timeStr);
				const auto tx = rtl
					? (rowRect.left() + farReserve)
					: (rowRect.right() - farReserve - tw);
				p.setFont(timeFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(QRect(tx, rowRect.top() + 4, tw, 16), Qt::AlignCenter, timeStr);
				farReserve += tw + 10;
			}
			const auto badgeStr = (item.unreadCount > 0)
				? QString::number(item.unreadCount)
				: (item.markedUnread ? u"●"_q : QString());
			if (!badgeStr.isEmpty()) {
				const auto bw = std::max(22, MeasureTextWidth(monoFont, badgeStr) + 10);
				const auto bx = rtl
					? (rowRect.left() + 12)
					: (rowRect.right() - 12 - bw);
				const auto badgeRect = QRect(bx, rowRect.top() + 22, bw, 18);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowActiveTextFg->c);
				p.drawRoundedRect(badgeRect, 9, 9);
				p.setFont(monoFont);
				p.setPen(Qt::white);
				p.drawText(badgeRect, Qt::AlignCenter, badgeStr);
				farReserve = std::max(farReserve, bw + 22);
			}
			if (item.mentions > 0) {
				const auto mw = 20;
				const auto mx = rtl
					? (rowRect.left() + farReserve)
					: (rowRect.right() - farReserve - mw);
				const auto mRect = QRect(mx, rowRect.top() + 22, mw, 18);
				auto mFill = BadgeColor(BadgeKind::Local);
				mFill.setAlpha(32);
				p.setPen(Qt::NoPen);
				p.setBrush(mFill);
				p.drawRoundedRect(mRect, 9, 9);
				p.setFont(monoFont);
				p.setPen(BadgeColor(BadgeKind::Local));
				p.drawText(mRect, Qt::AlignCenter, u"@"_q);
				farReserve += mw + 8;
			}

			// Bucket pill (DM / GP / CH) + optional BOT / TOPIC tag beside title.
			const auto tagText = ModalUnread::BucketTag(item.bucket);
			const auto tagW = MeasureTextWidth(monoFont, tagText) + 8;
			const auto startLead = 8 + kMarkW + 6 + kAvatarSize + 10;
			const auto contentW = std::max(80, rowRect.width() - startLead - farReserve - 8);
			const auto contentX = rtl
				? (rowRect.left() + farReserve + 8)
				: (rowRect.left() + startLead);

			const auto tagX = rtl
				? (contentX + contentW - tagW)
				: contentX;
			const auto tagRect = QRect(tagX, rowRect.top() + 5, tagW, 17);
			auto tagBg = (item.bucket == ModalUnread::ChatBucket::DirectMessage)
				? BadgeColor(BadgeKind::Insert)
				: (item.bucket == ModalUnread::ChatBucket::Group)
				? BadgeColor(BadgeKind::Normal)
				: BadgeColor(BadgeKind::Leader);
			auto tagFill = tagBg;
			tagFill.setAlpha(28);
			p.setPen(Qt::NoPen);
			p.setBrush(tagFill);
			p.drawRoundedRect(tagRect, 4, 4);
			p.setFont(monoFont);
			p.setPen(tagBg);
			p.drawText(tagRect, Qt::AlignCenter, tagText);

			auto extraPillW = tagW + 6;
			const auto subTag = item.isForum
				? u"TOPIC"_q
				: (item.isBot ? u"BOT"_q : QString());
			if (!subTag.isEmpty() && contentW > extraPillW + 90) {
				const auto sw = MeasureTextWidth(monoFont, subTag) + 8;
				const auto sx = rtl
					? (contentX + contentW - extraPillW - sw)
					: (contentX + extraPillW);
				const auto sRect = QRect(sx, rowRect.top() + 5, sw, 17);
				auto sFill = BadgeColor(BadgeKind::Local);
				sFill.setAlpha(24);
				p.setPen(Qt::NoPen);
				p.setBrush(sFill);
				p.drawRoundedRect(sRect, 4, 4);
				p.setFont(monoFont);
				p.setPen(BadgeColor(BadgeKind::Local));
				p.drawText(sRect, Qt::AlignCenter, subTag);
				extraPillW += sw + 6;
			}

			const auto titleW = std::max(40, contentW - extraPillW);
			const auto titleX = rtl ? contentX : (contentX + extraPillW);
			p.setFont(st::semiboldFont);
			p.setPen(st::windowFg);
			p.drawText(
				QRect(titleX, rowRect.top() + 4, titleW, 19),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				ElideText(st::semiboldFont, item.title, titleW));

			// Preview / @username subtitle.
			const auto subText = !item.preview.isEmpty()
				? item.preview
				: (!item.username.isEmpty() ? (u"@"_q + item.username) : QString());
			if (!subText.isEmpty()) {
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(
					QRect(contentX, rowRect.top() + 24, contentW, 17),
					(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
					ElideText(st::normalFont, subText, contentW));
			}
		}
	}

private:
	base::weak_ptr<Main::Session> _session;
	std::vector<ModalUnread::BufferItem> _items;
	std::vector<int> _visible;
	QSet<uint64_t> _marked;
	std::map<uint64_t, Ui::PeerUserpicView> _avatars;
	Fn<void(int)> _onClick;
	int _selected = 0;
	int _hovered = -1;
};

class UnreadBufferLayer final : public Ui::LayerWidget {
public:
	explicit UnreadBufferLayer(UnreadBufferConfig config)
	: _config(std::move(config))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _filter(Ui::CreateChild<Ui::InputField>(
		this,
		st::defaultMultiSelectSearchField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Filter unread chats (orderless, e.g. \"dm\", \"gp\", \"bot\")..."_q)))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
		setFocusPolicy(Qt::StrongFocus);
		_close->setClickedCallback([=] { closeLayer(); });

		auto rows = object_ptr<UnreadBufferRowsWidget>(
			_scroll,
			_config.session,
			[=](int row) { openRow(row); });
		_rows = rows.data();
		_scroll->setOwnedWidget(std::move(rows));

		_filter->changes() | rpl::on_next([=] {
			refilter();
		}, lifetime());
		_filter->submits() | rpl::on_next([=](Qt::KeyboardModifiers) {
			openRow(_selected);
		}, lifetime());

		base::install_event_filter(_filter->rawTextEdit(), [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::KeyPress) {
				return base::EventFilterResult::Continue;
			}
			const auto k = static_cast<QKeyEvent*>(e.get());
			const auto layout = _filter->rawTextEdit()->textCursor().block().layout();
			if (layout && !layout->preeditAreaText().isEmpty()) {
				return base::EventFilterResult::Continue;
			}
			const auto mods = k->modifiers() & ~Qt::KeypadModifier;
			const auto key = PhysicalKeyForNav(*k);
			if (key == Qt::Key_Escape || (key == Qt::Key_G && mods == kCtrlMod)) {
				if (!_filter->getLastText().isEmpty()) {
					_filter->setText(QString());
					setFocus();
				} else {
					setFocus();
				}
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Down || ((key == Qt::Key_N || key == Qt::Key_J) && mods == kCtrlMod)) {
				moveSelection(1);
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Up || ((key == Qt::Key_P || key == Qt::Key_K) && mods == kCtrlMod)) {
				moveSelection(-1);
				return base::EventFilterResult::Cancel;
			}
			return base::EventFilterResult::Continue;
		});

		refilter();
	}

protected:
	void doSetInnerFocus() override {
		setFocus();
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 32, 740);
		const auto h = std::min(parentSize.height() - 48, 600);
		setGeometry((parentSize.width() - w) / 2, (parentSize.height() - h) / 2, w, h);
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		constexpr auto kHeaderH = 54;
		constexpr auto kFilterH = 44;
		constexpr auto kFooterH = 32;
		if (IsRtl(this)) {
			_close->move(12, (kHeaderH - _close->height()) / 2);
		} else {
			_close->move(width() - 12 - _close->width(), (kHeaderH - _close->height()) / 2);
		}
		_filter->setGeometry(28, kHeaderH + 6, std::max(120, width() - 56), 32);
		_scroll->setGeometry(0, kHeaderH + kFilterH, width(), std::max(60, height() - kHeaderH - kFilterH - kFooterH));
		if (_rows) {
			_rows->resize(_scroll->width(), _rows->height());
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (_closing) {
			return;
		}
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
		const auto key = PhysicalKeyForNav(*e);

		if (!_pendingConfirmTargets.empty()) {
			if ((key == Qt::Key_Y && !mods) || key == Qt::Key_Return || key == Qt::Key_Enter) {
				auto targets = std::move(_pendingConfirmTargets);
				_pendingConfirmTargets.clear();
				executeMarkRead(std::move(targets));
				return;
			}
			if ((key == Qt::Key_N && !mods)
				|| key == Qt::Key_Escape
				|| (key == Qt::Key_Q && !mods)
				|| (key == Qt::Key_G && mods == kCtrlMod)) {
				_pendingConfirmTargets.clear();
				update();
				return;
			}
			return;
		}

		if (_pendingPrefix == u'*') {
			_pendingPrefix = {};
			if (key == Qt::Key_D && !mods) {
				applyMark(ModalUnread::MarkOp::MarkBucketDM);
				return;
			} else if (key == Qt::Key_G && !mods) {
				applyMark(ModalUnread::MarkOp::MarkBucketGP);
				return;
			} else if (key == Qt::Key_C && !mods) {
				applyMark(ModalUnread::MarkOp::MarkBucketCH);
				return;
			}
			update();
			return;
		}
		if (_pendingPrefix == u'g') {
			_pendingPrefix = {};
			if (key == Qt::Key_G && !mods) {
				jumpSelection(0);
				return;
			} else if (key == Qt::Key_R && !mods) {
				refreshItems();
				return;
			}
			update();
			return;
		}

		if (key == Qt::Key_Escape
			|| (key == Qt::Key_Q && !mods)
			|| (key == Qt::Key_G && mods == kCtrlMod)) {
			closeLayer();
			return;
		} else if (key == Qt::Key_Slash && !mods) {
			_filter->setFocusFast();
			return;
		} else if (key == Qt::Key_J && !mods) {
			moveSelection(1);
			return;
		} else if (key == Qt::Key_K && !mods) {
			moveSelection(-1);
			return;
		} else if (key == Qt::Key_Down || (key == Qt::Key_N && mods == kCtrlMod)) {
			moveSelection(1);
			return;
		} else if (key == Qt::Key_Up || (key == Qt::Key_P && mods == kCtrlMod)) {
			moveSelection(-1);
			return;
		} else if (key == Qt::Key_G && mods == Qt::ShiftModifier) {
			jumpSelection(int(_visible.size()) - 1);
			return;
		} else if (key == Qt::Key_G && !mods) {
			_pendingPrefix = u'g';
			update();
			return;
		} else if (key == Qt::Key_Asterisk) {
			_pendingPrefix = u'*';
			update();
			return;
		} else if (key == Qt::Key_M && !mods) {
			applyMark(ModalUnread::MarkOp::MarkOne);
			moveSelection(1);
			return;
		} else if (key == Qt::Key_U && !mods) {
			applyMark(ModalUnread::MarkOp::UnmarkOne);
			moveSelection(1);
			return;
		} else if (key == Qt::Key_U && mods == Qt::ShiftModifier) {
			applyMark(ModalUnread::MarkOp::UnmarkAll);
			return;
		} else if (key == Qt::Key_T && !mods) {
			applyMark(ModalUnread::MarkOp::ToggleAllVisible);
			return;
		} else if ((key == Qt::Key_R || key == Qt::Key_X) && !mods) {
			markTargetsRead();
			return;
		} else if (key == Qt::Key_Return || key == Qt::Key_Enter || (key == Qt::Key_O && !mods)) {
			openRow(_selected);
			return;
		}
		LayerWidget::keyPressEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		constexpr auto kHeaderH = 54;
		constexpr auto kFilterH = 44;
		constexpr auto kFooterH = 32;
		const auto rtl = IsRtl(this);

		// Header title + count badge.
		auto dm = 0, gp = 0, ch = 0;
		for (const auto &it : _config.items) {
			if (it.bucket == ModalUnread::ChatBucket::DirectMessage) ++dm;
			else if (it.bucket == ModalUnread::ChatBucket::Group) ++gp;
			else ++ch;
		}
		const auto title = u"Unread Unmuted Chats"_q;
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		const auto titleW = MeasureTextWidth(st::keysLegendHeaderStyle.font, title);
		const auto titleX = rtl ? (width() - 22 - titleW) : 22;
		p.drawText(QRect(titleX, 0, titleW, kHeaderH), (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, title);

		const auto summary = u"DM "_q + QString::number(dm)
			+ u" · GP "_q + QString::number(gp)
			+ u" · CH "_q + QString::number(ch)
			+ (_marked.isEmpty() ? QString() : (u"  ·  "_q + QString::number(_marked.size()) + u" marked"_q));
		const auto mono = MonoFont(11, true);
		const auto sumW = MeasureTextWidth(mono, summary) + 14;
		const auto sumX = rtl ? (_close->x() + _close->width() + 14) : (_close->x() - 14 - sumW);
		if ((!rtl && sumX > titleX + titleW + 12) || (rtl && sumX + sumW < titleX - 12)) {
			const auto pill = QRect(sumX, (kHeaderH - 22) / 2, sumW, 22);
			auto fill = st::windowActiveTextFg->c;
			fill.setAlpha(24);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(pill, 6, 6);
			p.setFont(mono);
			p.setPen(st::windowActiveTextFg);
			p.drawText(pill, Qt::AlignCenter, summary);
		}
		p.fillRect(0, kHeaderH - 1, width(), 1, st::boxDividerBg->c);

		// Filter backdrop.
		const auto searchRect = QRect(18, kHeaderH + 6, std::max(120, width() - 36), 32);
		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(BlendColor(st::windowBg->c, st::windowBgOver->c, 0.35));
		p.drawRoundedRect(QRectF(searchRect).adjusted(.5, .5, -.5, -.5), 8, 8);
		p.fillRect(0, kHeaderH + kFilterH - 1, width(), 1, st::boxDividerBg->c);

		// Footer strip (or inline read-receipt confirmation banner for multi-chat mark-read).
		const auto footerRect = QRect(0, height() - kFooterH, width(), kFooterH);
		if (!_pendingConfirmTargets.empty()) {
			auto warnBg = BadgeColor(BadgeKind::DescribeKey);
			warnBg.setAlpha(36);
			p.fillRect(footerRect.adjusted(0, 1, 0, 0), warnBg);
			p.fillRect(0, height() - kFooterH, width(), 1, BadgeColor(BadgeKind::DescribeKey));
			p.setFont(st::semiboldFont);
			p.setPen(BadgeColor(BadgeKind::DescribeKey));
			const auto prompt = u"Mark "_q
				+ QString::number(_pendingConfirmTargets.size())
				+ u" chats as read (sends read receipts)?  y / RET confirm  ·  n / ESC cancel"_q;
			p.drawText(
				footerRect.adjusted(16, 0, -16, 0),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				ElideText(st::semiboldFont, prompt, width() - 32));
		} else {
			p.fillRect(footerRect.adjusted(0, 1, 0, 0), BlendColor(st::windowBg->c, st::windowBgOver->c, 0.45));
			p.fillRect(0, height() - kFooterH, width(), 1, st::boxDividerBg->c);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			const auto hint = (_pendingPrefix == u'*')
				? u"* d mark DMs  ·  * g mark groups  ·  * c mark channels"_q
				: (_pendingPrefix == u'g')
				? u"g g top  ·  g r refresh"_q
				: u"j/k move  ·  RET open  ·  / filter  ·  m/u/U/t/* mark  ·  x read  ·  q close"_q;
			p.drawText(
				footerRect.adjusted(16, 0, -16, 0),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				ElideText(st::normalFont, hint, width() - 32));
		}
	}

private:
	void refilter() {
		_visible = ModalUnread::FilterBufferItems(_config.items, _filter->getLastText());
		_selected = std::clamp(_selected, 0, std::max(0, int(_visible.size()) - 1));
		if (_rows) {
			_rows->setData(_config.items, _visible, _marked, _selected);
		}
		_scroll->scrollToY(0);
		update();
	}

	void moveSelection(int delta) {
		if (_visible.empty()) {
			return;
		}
		const auto n = int(_visible.size());
		_selected = std::clamp(_selected + delta, 0, n - 1);
		syncSelection();
	}

	void jumpSelection(int index) {
		if (_visible.empty()) {
			return;
		}
		_selected = std::clamp(index, 0, int(_visible.size()) - 1);
		syncSelection();
	}

	void syncSelection() {
		if (_rows) {
			_rows->setSelectionAndMarks(_selected, _marked);
		}
		const auto rowTop = 4 + _selected * UnreadBufferRowsWidget::kRowHeight;
		const auto rowBottom = rowTop + UnreadBufferRowsWidget::kRowHeight;
		if (rowTop < _scroll->scrollTop()) {
			_scroll->scrollToY(rowTop);
		} else if (rowBottom > _scroll->scrollTop() + _scroll->height()) {
			_scroll->scrollToY(rowBottom - _scroll->height());
		}
		update();
	}

	void applyMark(ModalUnread::MarkOp op) {
		ModalUnread::ApplyMarkOp(_marked, _config.items, _visible, _selected, op);
		syncSelection();
	}

	void refreshItems() {
		if (_config.onRefresh) {
			_config.items = _config.onRefresh();
			refilter();
		}
	}

	void markTargetsRead() {
		auto targets = std::vector<PeerId>();
		if (!_marked.isEmpty()) {
			for (const auto id : _marked) {
				targets.push_back(PeerId(id));
			}
		} else if (_selected >= 0 && _selected < int(_visible.size())) {
			targets.push_back(PeerId(_config.items[_visible[_selected]].peerId));
		}
		if (targets.empty()) {
			return;
		}
		if (ModalUnread::NeedsMarkReadConfirmation(int(targets.size()))) {
			_pendingConfirmTargets = std::move(targets);
			update();
			return;
		}
		executeMarkRead(std::move(targets));
	}

	void executeMarkRead(std::vector<PeerId> targets) {
		for (const auto id : targets) {
			_marked.remove(id.value);
		}
		if (_config.onMarkRead) {
			_config.onMarkRead(std::move(targets));
		}
		refreshItems();
	}

	void openRow(int row) {
		if (_closing || row < 0 || row >= int(_visible.size())) {
			return;
		}
		_closing = true;
		const auto peerId = PeerId(_config.items[_visible[row]].peerId);
		auto cb = std::move(_config.onOpenChat);
		closeLayer();
		if (cb) {
			cb(peerId);
		}
	}

	UnreadBufferConfig _config;
	std::vector<int> _visible;
	QSet<uint64_t> _marked;
	std::vector<PeerId> _pendingConfirmTargets;
	Ui::IconButton *_close = nullptr;
	Ui::InputField *_filter = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	QPointer<UnreadBufferRowsWidget> _rows;
	int _selected = 0;
	QChar _pendingPrefix;
	bool _closing = false;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

// ---------------------------------------------------------------------------
// TopicPickerLayer (Forum Groups & Threaded Bots)
// ---------------------------------------------------------------------------

class TopicRowsWidget final : public Ui::RpWidget {
public:
	TopicRowsWidget(
		not_null<QWidget*> parent,
		base::weak_ptr<Main::Session> session,
		Fn<void(int row)> onClick,
		Fn<void()> onLoadMoreClick)
	: RpWidget(parent)
	, _session(std::move(session))
	, _onClick(std::move(onClick))
	, _onLoadMoreClick(std::move(onLoadMoreClick)) {
		setMouseTracking(true);
	}

	static constexpr int kRowHeight = 44;
	static constexpr int kMoreRowHeight = 34;

	void setData(
			const std::vector<TopicItem> &items,
			const std::vector<int> &visible,
			int selected) {
		_items = items;
		_visible = visible;
		_selected = selected;
		_hovered = -1;
		updateHeight();
		update();
	}

	void setStatus(bool hasMore, bool loading) {
		if (_hasMore == hasMore && _loading == loading) {
			return;
		}
		_hasMore = hasMore;
		_loading = loading;
		updateHeight();
		update();
	}

	void setSelected(int selected) {
		_selected = selected;
		update();
	}

protected:
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto y = e->pos().y();
		const auto rowsBottom = 4 + int(_visible.size()) * kRowHeight;
		if (_hasMore && !_visible.empty() && y >= rowsBottom && y < rowsBottom + kMoreRowHeight) {
			if (_hovered != -1 || !_moreHovered) {
				_hovered = -1;
				_moreHovered = true;
				setCursor(_loading ? style::cur_default : style::cur_pointer);
				update();
			}
			return;
		}
		const auto row = (y - 4) / kRowHeight;
		const auto next = (row >= 0 && row < int(_visible.size())) ? row : -1;
		if (next != _hovered || _moreHovered) {
			_hovered = next;
			_moreHovered = false;
			setCursor((next >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEventHook(QEvent *) override {
		if (_hovered >= 0 || _moreHovered) {
			_hovered = -1;
			_moreHovered = false;
			update();
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto y = e->pos().y();
		const auto rowsBottom = 4 + int(_visible.size()) * kRowHeight;
		if (_hasMore && !_loading && !_visible.empty()
			&& y >= rowsBottom && y < rowsBottom + kMoreRowHeight
			&& _onLoadMoreClick) {
			_onLoadMoreClick();
			return;
		}
		const auto row = (y - 4) / kRowHeight;
		if (row >= 0 && row < int(_visible.size()) && _onClick) {
			_onClick(row);
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto rtl = IsRtl(this);
		const auto monoFont = MonoFont(10, true);
		const auto timeFont = MonoFont(10, false);

		if (_visible.empty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				rect(),
				Qt::AlignCenter,
				(_loading || _hasMore)
					? u"Loading topics..."_q
					: u"No matching topics."_q);
			return;
		}

		const auto clip = e->rect();
		const auto first = std::max(0, (clip.top() - 4) / kRowHeight);
		const auto till = std::min(int(_visible.size()), (clip.bottom() - 4) / kRowHeight + 2);

		for (auto row = first; row < till; ++row) {
			const auto &item = _items[_visible[row]];
			const auto y = 4 + row * kRowHeight;
			const auto rowRect = QRect(8, y + 1, width() - 16, kRowHeight - 2);
			const auto isActive = (row == _selected);
			const auto isHover = (row == _hovered);

			if (isActive || isHover) {
				auto fill = st::windowBgOver->c;
				if (!isActive) {
					fill.setAlpha(140);
				}
				p.setPen(Qt::NoPen);
				p.setBrush(fill);
				p.drawRoundedRect(rowRect, 7, 7);
				if (isActive) {
					const auto barRect = rtl
						? QRect(rowRect.right() - 2, rowRect.top() + 6, 3, rowRect.height() - 12)
						: QRect(rowRect.left(), rowRect.top() + 6, 3, rowRect.height() - 12);
					p.setBrush(st::windowActiveTextFg->c);
					p.drawRoundedRect(barRect, 1.5, 1.5);
				}
			}

			const auto midY = rowRect.center().y() + 1;
			constexpr auto kIconSize = 24;
			const auto iconX = rtl
				? (rowRect.right() - 12 - kIconSize)
				: (rowRect.left() + 12);
			const auto iconRect = QRect(iconX, midY - kIconSize / 2, kIconSize, kIconSize);
			auto paintedRealIcon = false;
			if (item.iconId != 0) {
				if (const auto s = _session.get()) {
					auto &emoji = _customIcons[item.iconId];
					if (!emoji) {
						emoji = s->data().customEmojiManager().create(
							item.iconId,
							[this] { update(); },
							Data::CustomEmojiManager::SizeTag::Normal);
					}
					if (emoji) {
						const auto ratio = style::DevicePixelRatio();
						const auto frameSize = Data::FrameSizeFromTag(
							Data::CustomEmojiManager::SizeTag::Normal) / std::max(1, ratio);
						const auto pos = QPoint(
							iconRect.x() + (iconRect.width() - frameSize) / 2,
							iconRect.y() + (iconRect.height() - frameSize) / 2);
						emoji->paint(p, {
							.textColor = st::windowFg->c,
							.now = crl::now(),
							.position = pos,
							.paused = false,
						});
						paintedRealIcon = true;
					}
				}
			}
			if (!paintedRealIcon && item.isGeneral) {
				const auto fg = st::windowSubTextFg->c;
				if (_generalIcon.isNull() || _generalIconColor != fg) {
					_generalIcon = Data::ForumTopicGeneralIconFrame(kIconSize, fg);
					_generalIconColor = fg;
				}
				p.drawImage(iconRect.topLeft(), _generalIcon);
				paintedRealIcon = true;
			}
			if (!paintedRealIcon) {
				auto &bubble = _topicBubbles[item.colorId];
				if (bubble.isNull()) {
					bubble = Data::ForumTopicIconBackground(item.colorId, kIconSize);
				}
				if (!bubble.isNull()) {
					p.drawImage(iconRect.topLeft(), bubble);
				} else {
					const auto dotColor = TopicColorFromId(item.colorId);
					auto dotFill = dotColor;
					dotFill.setAlpha(36);
					p.setPen(QPen(dotColor, 1));
					p.setBrush(dotFill);
					p.drawRoundedRect(QRectF(iconRect).adjusted(.5, .5, -.5, -.5), 7, 7);
				}
				const auto first = ModalMx::FirstGrapheme(item.title);
				const auto glyph = first.isEmpty() ? u"T"_q : first.toUpper();
				p.setFont(st::semiboldFont);
				p.setPen(bubble.isNull() ? TopicColorFromId(item.colorId) : Qt::white);
				p.drawText(iconRect.adjusted(0, -1, 0, -1), Qt::AlignCenter, glyph);
			}

			// Far side: timestamp + unread badge.
			auto farReserve = 12;
			const auto timeStr = FormatShortTimestamp(item.lastTimestamp);
			if (!timeStr.isEmpty()) {
				const auto tw = MeasureTextWidth(timeFont, timeStr);
				const auto tx = rtl ? (rowRect.left() + farReserve) : (rowRect.right() - farReserve - tw);
				p.setFont(timeFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(QRect(tx, rowRect.top() + 4, tw, 16), Qt::AlignCenter, timeStr);
				farReserve += tw + 10;
			}
			if (item.unreadCount > 0 || item.mentions > 0) {
				const auto badgeStr = (item.unreadCount > 0)
					? QString::number(item.unreadCount)
					: u"@"_q;
				const auto bw = std::max(22, MeasureTextWidth(monoFont, badgeStr) + 10);
				const auto bx = rtl ? (rowRect.left() + 12) : (rowRect.right() - 12 - bw);
				const auto badgeRect = QRect(bx, rowRect.top() + 21, bw, 18);
				p.setPen(Qt::NoPen);
				p.setBrush(item.muted ? st::windowSubTextFg->c : st::windowActiveTextFg->c);
				p.drawRoundedRect(badgeRect, 9, 9);
				p.setFont(monoFont);
				p.setPen(Qt::white);
				p.drawText(badgeRect, Qt::AlignCenter, badgeStr);
				farReserve = std::max(farReserve, bw + 22);
			}

			const auto lead = 12 + kIconSize + 10;
			const auto contentW = std::max(80, rowRect.width() - lead - farReserve - 8);
			const auto contentX = rtl ? (rowRect.left() + farReserve + 8) : (rowRect.left() + lead);

			auto titleText = item.title;
			if (item.active) titleText += u"  ·  current"_q;
			if (item.pinned) titleText += u"  ·  pinned"_q;
			if (item.closed) titleText += u"  ·  closed"_q;
			p.setFont(st::semiboldFont);
			p.setPen(item.closed ? st::windowSubTextFg : st::windowFg);
			p.drawText(
				QRect(contentX, rowRect.top() + 4, contentW, 18),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				ElideText(st::semiboldFont, titleText, contentW));

			if (!item.preview.isEmpty()) {
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(
					QRect(contentX, rowRect.top() + 23, contentW, 16),
					(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
					ElideText(st::normalFont, item.preview, contentW));
			}
		}

		if (_hasMore) {
			const auto moreY = 4 + int(_visible.size()) * kRowHeight;
			const auto moreRect = QRect(12, moreY + 3, width() - 24, kMoreRowHeight - 6);
			if (_moreHovered && !_loading) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver->c);
				p.drawRoundedRect(moreRect, 6, 6);
			}
			p.setFont(st::semiboldFont);
			p.setPen(_loading ? st::windowSubTextFg : st::windowActiveTextFg);
			p.drawText(
				moreRect,
				Qt::AlignCenter,
				_loading
					? u"Loading more topics..."_q
					: u"Load more topics  (L or scroll)"_q);
		}
	}

private:
	void updateHeight() {
		const auto extra = (_hasMore && !_visible.empty()) ? kMoreRowHeight : 0;
		resize(width(), std::max(72, int(_visible.size()) * kRowHeight + extra + 8));
	}

	base::weak_ptr<Main::Session> _session;
	std::map<DocumentId, std::unique_ptr<Ui::Text::CustomEmoji>> _customIcons;
	std::map<int32, QImage> _topicBubbles;
	QImage _generalIcon;
	QColor _generalIconColor;
	std::vector<TopicItem> _items;
	std::vector<int> _visible;
	Fn<void(int)> _onClick;
	Fn<void()> _onLoadMoreClick;
	int _selected = 0;
	int _hovered = -1;
	bool _hasMore = false;
	bool _loading = false;
	bool _moreHovered = false;
};

class TopicPickerLayer final : public Ui::LayerWidget {
public:
	explicit TopicPickerLayer(TopicPickerConfig config)
	: _config(std::move(config))
	, _unreadOnly(_config.startUnreadOnly)
	, _hasMore(_config.hasMore)
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _filter(Ui::CreateChild<Ui::InputField>(
		this,
		st::defaultMultiSelectSearchField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Filter topics (orderless)..."_q)))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
		setFocusPolicy(Qt::StrongFocus);
		_close->setClickedCallback([=] { closeLayer(); });
		if (const auto s = _config.session.get()) {
			s->downloaderTaskFinished() | rpl::on_next([this] {
				update();
			}, lifetime());
		}

		auto rows = object_ptr<TopicRowsWidget>(
			_scroll,
			_config.session,
			[=](int row) { openRow(row); },
			[=] { requestMore(); });
		_rows = rows.data();
		_scroll->setOwnedWidget(std::move(rows));

		_scroll->scrolls() | rpl::on_next([=] {
			maybeRequestMore();
		}, lifetime());

		_loadTimeout.setSingleShot(true);
		connect(&_loadTimeout, &QTimer::timeout, this, [=] {
			_autoLoadPaused = true;
			refreshFromSource(/*preserveScroll=*/true);
		});
		std::move(_config.pageUpdates) | rpl::on_next([=] {
			refreshFromSource(/*preserveScroll=*/true);
			if (_awaitingPage) {
				_autoLoadPaused = ranges::none_of(_config.items, [&](const auto &item) {
					return !_rootsBeforeLoad.contains(item.rootId.bare);
				});
				finishLoading();
			}
			QTimer::singleShot(0, this, [=] { maybeRequestMore(); });
		}, lifetime());
		if (_config.updates) {
			std::move(_config.updates) | rpl::on_next([=] {
				refreshFromSource(/*preserveScroll=*/true);
			}, lifetime());
		}
		if (_config.destroyed) {
			std::move(_config.destroyed) | rpl::on_next([=] {
				closeLayer();
			}, lifetime());
		}

		_filter->changes() | rpl::on_next([=] {
			_autoLoadPaused = false;
			refilter(/*preserveScroll=*/false, MsgId(0));
			maybeRequestMore();
		}, lifetime());
		_filter->submits() | rpl::on_next([=](Qt::KeyboardModifiers) {
			openRow(_selected);
		}, lifetime());

		base::install_event_filter(_filter->rawTextEdit(), [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::KeyPress) {
				return base::EventFilterResult::Continue;
			}
			const auto k = static_cast<QKeyEvent*>(e.get());
			const auto layout = _filter->rawTextEdit()->textCursor().block().layout();
			if (layout && !layout->preeditAreaText().isEmpty()) {
				return base::EventFilterResult::Continue;
			}
			const auto mods = k->modifiers() & ~Qt::KeypadModifier;
			const auto key = PhysicalKeyForNav(*k);
			if (key == Qt::Key_Escape || (key == Qt::Key_G && mods == kCtrlMod)) {
				if (!_filter->getLastText().isEmpty()) {
					_filter->setText(QString());
				}
				setFocus();
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Down || ((key == Qt::Key_N || key == Qt::Key_J) && mods == kCtrlMod)) {
				moveSelection(1);
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Up || ((key == Qt::Key_P || key == Qt::Key_K) && mods == kCtrlMod)) {
				moveSelection(-1);
				return base::EventFilterResult::Cancel;
			}
			return base::EventFilterResult::Continue;
		});

		refilter(/*preserveScroll=*/false, MsgId(0));
		if (_hasMore && _visible.size() < 20) {
			requestMore();
		}
	}

protected:
	void doSetInnerFocus() override {
		setFocus();
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 32, 660);
		const auto h = std::min(parentSize.height() - 48, 560);
		setGeometry((parentSize.width() - w) / 2, (parentSize.height() - h) / 2, w, h);
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		constexpr auto kHeaderH = 54;
		constexpr auto kFilterH = 44;
		constexpr auto kFooterH = 32;
		if (IsRtl(this)) {
			_close->move(12, (kHeaderH - _close->height()) / 2);
		} else {
			_close->move(width() - 12 - _close->width(), (kHeaderH - _close->height()) / 2);
		}
		_filter->setGeometry(28, kHeaderH + 6, std::max(120, width() - 56), 32);
		_scroll->setGeometry(0, kHeaderH + kFilterH, width(), std::max(60, height() - kHeaderH - kFilterH - kFooterH));
		if (_rows) {
			_rows->resize(_scroll->width(), _rows->height());
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (_closing) {
			return;
		}
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
		const auto key = PhysicalKeyForNav(*e);
		if (_pendingG) {
			_pendingG = false;
			if (key == Qt::Key_G && !mods) {
				moveSelection(-int(_visible.size()));
				return;
			} else if (key == Qt::Key_R && !mods) {
				_autoLoadPaused = true;
				finishLoading();
				refreshFromSource(/*preserveScroll=*/true);
				if (_config.onReload) {
					startLoading();
					_config.onReload();
				}
				return;
			}
		}
		if (key == Qt::Key_Escape
			|| (key == Qt::Key_Q && !mods)
			|| (key == Qt::Key_G && mods == kCtrlMod)) {
			closeLayer();
			return;
		} else if (key == Qt::Key_Slash && !mods) {
			_filter->setFocusFast();
			return;
		} else if (key == Qt::Key_G && !mods) {
			_pendingG = true;
			return;
		} else if (key == Qt::Key_G && mods == Qt::ShiftModifier) {
			moveSelection(int(_visible.size()));
			maybeRequestMore();
			return;
		} else if (key == Qt::Key_L && mods == Qt::ShiftModifier) {
			requestMore();
			return;
		} else if (key == Qt::Key_J && !mods) {
			moveSelection(1);
			return;
		} else if (key == Qt::Key_K && !mods) {
			moveSelection(-1);
			return;
		} else if (key == Qt::Key_Down || (key == Qt::Key_N && mods == kCtrlMod)) {
			moveSelection(1);
			return;
		} else if (key == Qt::Key_Up || (key == Qt::Key_P && mods == kCtrlMod)) {
			moveSelection(-1);
			return;
		} else if (key == Qt::Key_U && !mods) {
			_unreadOnly = !_unreadOnly;
			_autoLoadPaused = false;
			refilter(/*preserveScroll=*/false, MsgId(0));
			maybeRequestMore();
			return;
		} else if (key == Qt::Key_C && !mods && _config.canCreate && _config.onCreateTopic) {
			_closing = true;
			auto cb = std::move(_config.onCreateTopic);
			closeLayer();
			cb();
			return;
		} else if (key == Qt::Key_Return || key == Qt::Key_Enter || (key == Qt::Key_O && !mods)) {
			openRow(_selected);
			return;
		}
		LayerWidget::keyPressEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		constexpr auto kHeaderH = 54;
		constexpr auto kFilterH = 44;
		constexpr auto kFooterH = 32;
		const auto rtl = IsRtl(this);

		// Parent peer avatar in header.
		constexpr auto kAvatarSize = 28;
		auto leadX = 18;
		if (const auto s = _config.session.get(); s && _config.parentPeerId != 0) {
			if (const auto peer = s->data().peerLoaded(_config.parentPeerId)) {
				peer->loadUserpic();
				const auto ax = rtl ? (width() - leadX - kAvatarSize) : leadX;
				peer->paintUserpic(p, _parentAvatar, ax, (kHeaderH - kAvatarSize) / 2, kAvatarSize, true);
				leadX += kAvatarSize + 10;
			}
		}

		const auto badgeText = _unreadOnly
			? u"UNREAD ONLY"_q
			: (_config.isBot ? u"BOT THREADS"_q : u"TOPICS"_q);
		const auto mono = MonoFont(10, true);
		const auto bw = MeasureTextWidth(mono, badgeText) + 12;
		const auto bx = rtl ? (_close->x() + _close->width() + 12) : (_close->x() - 12 - bw);
		const auto bRect = QRect(bx, (kHeaderH - 22) / 2, bw, 22);
		auto bColor = _unreadOnly ? BadgeColor(BadgeKind::Local) : st::windowActiveTextFg->c;
		auto bFill = bColor;
		bFill.setAlpha(28);
		p.setPen(Qt::NoPen);
		p.setBrush(bFill);
		p.drawRoundedRect(bRect, 6, 6);
		p.setFont(mono);
		p.setPen(bColor);
		p.drawText(bRect, Qt::AlignCenter, badgeText);

		const auto titleW = std::max(80, width() - leadX - bw - _close->width() - 36);
		const auto titleX = rtl ? (bx + bw + 10) : leadX;
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(titleX, 0, titleW, kHeaderH),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			ElideText(st::keysLegendHeaderStyle.font, _config.parentTitle, titleW));
		p.fillRect(0, kHeaderH - 1, width(), 1, st::boxDividerBg->c);

		const auto searchRect = QRect(18, kHeaderH + 6, std::max(120, width() - 36), 32);
		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(BlendColor(st::windowBg->c, st::windowBgOver->c, 0.35));
		p.drawRoundedRect(QRectF(searchRect).adjusted(.5, .5, -.5, -.5), 8, 8);
		p.fillRect(0, kHeaderH + kFilterH - 1, width(), 1, st::boxDividerBg->c);

		const auto footerRect = QRect(0, height() - kFooterH, width(), kFooterH);
		p.fillRect(footerRect.adjusted(0, 1, 0, 0), BlendColor(st::windowBg->c, st::windowBgOver->c, 0.45));
		p.fillRect(0, height() - kFooterH, width(), 1, st::boxDividerBg->c);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto hint = [&] {
			auto out = u"j/k move  ·  RET open  ·  / filter  ·  u unread"_q;
			if (_config.canCreate) {
				out += u"  ·  c new"_q;
			}
			if (_hasMore) {
				out += _loadTimeout.isActive() ? u"  ·  loading..."_q : u"  ·  L more"_q;
			}
			out += u"  ·  g r refresh  ·  q close"_q;
			return out;
		}();
		p.drawText(
			footerRect.adjusted(16, 0, -16, 0),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			ElideText(st::normalFont, hint, width() - 32));
	}

private:
	void refreshFromSource(bool preserveScroll) {
		if (!_config.onRefresh) {
			return;
		}
		const auto keepRootId = (_selected >= 0
				&& _selected < int(_visible.size())
				&& _visible[_selected] < int(_config.items.size()))
			? _config.items[_visible[_selected]].rootId
			: MsgId(0);
		auto snap = _config.onRefresh();
		_config.items = std::move(snap.items);
		_hasMore = snap.hasMore;
		refilter(preserveScroll, keepRootId);
	}

	void startLoading() {
		constexpr auto kLoadTimeout = crl::time(10000);
		_autoLoadPaused = false;
		_awaitingPage = true;
		_rootsBeforeLoad.clear();
		for (const auto &item : _config.items) {
			_rootsBeforeLoad.insert(item.rootId.bare);
		}
		_loadTimeout.start(kLoadTimeout);
		_rows->setStatus(true, true);
		update();
	}

	void finishLoading() {
		_awaitingPage = false;
		_loadTimeout.stop();
		_rows->setStatus(_hasMore, false);
		update();
	}

	void requestMore() {
		if (!_hasMore || _loadTimeout.isActive() || !_config.onLoadMore) {
			return;
		}
		startLoading();
		_config.onLoadMore();
	}

	void maybeRequestMore() {
		if (_autoLoadPaused || !_hasMore || _loadTimeout.isActive() || !_config.onLoadMore) {
			return;
		}
		const auto maxTop = _scroll->scrollTopMax();
		const auto nearBottom = (maxTop <= 0)
			|| (_scroll->scrollTop() + 140 >= maxTop);
		const auto nearSelectedEnd = !_visible.empty()
			&& (_selected + 5 >= int(_visible.size()));
		const auto filtering = !_filter->getLastText().trimmed().isEmpty()
			|| _unreadOnly;
		if (nearBottom || nearSelectedEnd || filtering) {
			requestMore();
		}
	}

	void refilter(bool preserveScroll, MsgId keepRootId) {
		const auto tokens = ModalMx::ParseTokens(_filter->getLastText());
		_visible.clear();
		auto restoredSelected = -1;
		for (auto i = 0; i < int(_config.items.size()); ++i) {
			const auto &it = _config.items[i];
			if (_unreadOnly && it.unreadCount <= 0 && it.mentions <= 0) {
				continue;
			}
			if (!tokens.empty()) {
				const auto hay = (it.title + u' ' + it.preview).toCaseFolded();
				const auto init = ModalMx::WordInitials(it.title);
				const auto ok = std::all_of(tokens.begin(), tokens.end(), [&](const ModalMx::OrderlessToken &t) {
					return ModalMx::MatchToken(t, hay, init);
				});
				if (!ok) {
					continue;
				}
			}
			if (keepRootId != 0 && it.rootId == keepRootId) {
				restoredSelected = int(_visible.size());
			}
			_visible.push_back(i);
		}
		if (restoredSelected >= 0) {
			_selected = restoredSelected;
		} else {
			_selected = std::clamp(_selected, 0, std::max(0, int(_visible.size()) - 1));
		}
		if (_rows) {
			_rows->setData(_config.items, _visible, _selected);
			_rows->setStatus(_hasMore, _loadTimeout.isActive());
		}
		if (!preserveScroll) {
			_scroll->scrollToY(0);
		}
		update();
	}

	void moveSelection(int delta) {
		if (_visible.empty()) {
			return;
		}
		const auto n = int(_visible.size());
		_selected = std::clamp(_selected + delta, 0, n - 1);
		if (_rows) {
			_rows->setSelected(_selected);
		}
		const auto rowTop = 4 + _selected * TopicRowsWidget::kRowHeight;
		const auto rowBottom = rowTop + TopicRowsWidget::kRowHeight;
		if (rowTop < _scroll->scrollTop()) {
			_scroll->scrollToY(rowTop);
		} else if (rowBottom > _scroll->scrollTop() + _scroll->height()) {
			_scroll->scrollToY(rowBottom - _scroll->height());
		}
		maybeRequestMore();
	}

	void openRow(int row) {
		if (_closing || row < 0 || row >= int(_visible.size())) {
			return;
		}
		_closing = true;
		const auto rootId = _config.items[_visible[row]].rootId;
		auto cb = std::move(_config.onOpenTopic);
		closeLayer();
		if (cb) {
			cb(rootId);
		}
	}

	TopicPickerConfig _config;
	bool _unreadOnly = false;
	bool _hasMore = false;
	QTimer _loadTimeout;
	QSet<int64> _rootsBeforeLoad;
	bool _awaitingPage = false;
	bool _autoLoadPaused = false;
	bool _pendingG = false;
	std::vector<int> _visible;
	Ui::PeerUserpicView _parentAvatar;
	Ui::IconButton *_close = nullptr;
	Ui::InputField *_filter = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	QPointer<TopicRowsWidget> _rows;
	int _selected = 0;
	bool _closing = false;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

// ---------------------------------------------------------------------------
// NoticeLayer & NoticeHistoryLayer (w-991c evil notices + SPC h e *Messages*)
// ---------------------------------------------------------------------------

[[nodiscard]] QColor SeverityColor(ModalNotices::NoticeSeverity severity) {
	switch (severity) {
	case ModalNotices::NoticeSeverity::Info: return st::windowActiveTextFg->c;
	case ModalNotices::NoticeSeverity::Warning: return BadgeColor(BadgeKind::DescribeKey);
	case ModalNotices::NoticeSeverity::Error: return st::attentionButtonFg->c;
	}
	return st::windowActiveTextFg->c;
}

[[nodiscard]] QString SeverityBadgeText(ModalNotices::NoticeSeverity severity) {
	switch (severity) {
	case ModalNotices::NoticeSeverity::Info: return u"NOTICE"_q;
	case ModalNotices::NoticeSeverity::Warning: return u"WARNING"_q;
	case ModalNotices::NoticeSeverity::Error: return u"ERROR"_q;
	}
	return u"NOTICE"_q;
}

class NoticeBodyWidget final : public Ui::RpWidget {
public:
	NoticeBodyWidget(not_null<QWidget*> parent, ModalNotices::NoticeRecord record)
	: RpWidget(parent)
	, _record(std::move(record)) {
	}

	void relayout(int w) {
		const auto usable = std::max(160, w);
		if (width() == usable && _laidOut) {
			return;
		}
		_laidOut = true;
		const auto h = computeHeight(usable);
		resize(usable, std::max(64, h));
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto rtl = IsRtl(this);
		const auto hAlign = rtl ? Qt::AlignRight : Qt::AlignLeft;
		const auto mono = MonoFont(11, false);
		const auto badgeFont = MonoFont(10, true);
		constexpr auto kPadX = 18;
		const auto contentW = std::max(120, width() - 2 * kPadX);
		auto y = 12;

		if (!_record.summary.isEmpty()) {
			const auto fm = QFontMetrics(st::normalFont);
			const auto flags = hAlign | Qt::TextWordWrap;
			const auto br = fm.boundingRect(QRect(0, 0, contentW, 10000), flags, _record.summary);
			p.setFont(st::normalFont);
			p.setPen(st::windowFg);
			p.drawText(QRect(kPadX, y, contentW, br.height() + 4), flags, _record.summary);
			y += br.height() + 14;
		}

		for (const auto &row : _record.rows) {
			const auto rowH = measureRowHeight(row, contentW);
			const auto card = QRect(kPadX, y, contentW, rowH);
			const auto accent = row.isWarning
				? BadgeColor(BadgeKind::DescribeKey)
				: (row.isInactive ? st::windowSubTextFg->c : st::windowActiveTextFg->c);
			auto bg = BlendColor(st::windowBg->c, st::windowBgOver->c, row.isWarning ? 0.45 : 0.30);
			p.setPen(QPen(st::boxDividerBg->c, 1));
			p.setBrush(bg);
			p.drawRoundedRect(QRectF(card).adjusted(.5, .5, -.5, -.5), 7, 7);

			// Leading accent stripe.
			const auto stripe = rtl
				? QRect(card.right() - 2, card.top() + 5, 3, card.height() - 10)
				: QRect(card.left(), card.top() + 5, 3, card.height() - 10);
			p.setPen(Qt::NoPen);
			p.setBrush(accent);
			p.drawRoundedRect(stripe, 1.5, 1.5);

			auto innerY = card.top() + 8;
			const auto innerX = card.left() + 12;
			const auto innerW = std::max(80, card.width() - 24);

			auto tagW = 0;
			if (!row.tag.isEmpty()) {
				tagW = std::min(160, MeasureTextWidth(badgeFont, row.tag) + 12);
				const auto tx = rtl ? innerX : (innerX + innerW - tagW);
				const auto tagRect = QRect(tx, innerY, tagW, 18);
				auto tFill = accent;
				tFill.setAlpha(28);
				p.setPen(Qt::NoPen);
				p.setBrush(tFill);
				p.drawRoundedRect(tagRect, 5, 5);
				p.setFont(badgeFont);
				p.setPen(accent);
				p.drawText(tagRect, Qt::AlignCenter, ElideText(badgeFont, row.tag, tagW - 8));
			}

			if (!row.label.isEmpty()) {
				const auto lw = std::max(60, innerW - (tagW > 0 ? (tagW + 8) : 0));
				const auto lx = rtl ? (innerX + (tagW > 0 ? (tagW + 8) : 0)) : innerX;
				const auto fm = QFontMetrics(st::semiboldFont);
				const auto br = fm.boundingRect(QRect(0, 0, lw, 4000), hAlign | Qt::TextWordWrap, row.label);
				p.setFont(st::semiboldFont);
				p.setPen(row.isInactive ? st::windowSubTextFg : st::windowFg);
				p.drawText(QRect(lx, innerY, lw, br.height() + 2), hAlign | Qt::TextWordWrap, row.label);
				innerY += std::max(20, br.height() + 4);
			} else if (tagW > 0) {
				innerY += 22;
			}

			if (!row.before.isEmpty() || !row.after.isEmpty()) {
				const auto valText = row.before + u"  ->  "_q + row.after;
				const auto fm = QFontMetrics(mono);
				const auto br = fm.boundingRect(QRect(0, 0, innerW, 4000), hAlign | Qt::TextWordWrap, valText);
				p.setFont(mono);
				p.setPen(row.isInactive ? st::windowSubTextFg : st::windowActiveTextFg);
				p.drawText(QRect(innerX, innerY, innerW, br.height() + 2), hAlign | Qt::TextWordWrap, valText);
				innerY += br.height() + 4;
			}

			if (!row.detail.isEmpty()) {
				const auto fm = QFontMetrics(st::normalFont);
				const auto br = fm.boundingRect(QRect(0, 0, innerW, 4000), hAlign | Qt::TextWordWrap, row.detail);
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(QRect(innerX, innerY, innerW, br.height() + 2), hAlign | Qt::TextWordWrap, row.detail);
			}

			y += rowH + 8;
		}
	}

private:
	[[nodiscard]] int measureRowHeight(const ModalNotices::NoticeRow &row, int contentW) const {
		const auto innerW = std::max(80, contentW - 24);
		const auto badgeFont = MonoFont(10, true);
		const auto mono = MonoFont(11, false);
		auto h = 16;
		const auto tagW = row.tag.isEmpty() ? 0 : std::min(160, MeasureTextWidth(badgeFont, row.tag) + 12);
		if (!row.label.isEmpty()) {
			const auto lw = std::max(60, innerW - (tagW > 0 ? (tagW + 8) : 0));
			const auto br = QFontMetrics(st::semiboldFont).boundingRect(
				QRect(0, 0, lw, 4000), Qt::AlignLeft | Qt::TextWordWrap, row.label);
			h += std::max(20, br.height() + 4);
		} else if (tagW > 0) {
			h += 22;
		}
		if (!row.before.isEmpty() || !row.after.isEmpty()) {
			const auto valText = row.before + u"  ->  "_q + row.after;
			const auto br = QFontMetrics(mono).boundingRect(
				QRect(0, 0, innerW, 4000), Qt::AlignLeft | Qt::TextWordWrap, valText);
			h += br.height() + 4;
		}
		if (!row.detail.isEmpty()) {
			const auto br = QFontMetrics(st::normalFont).boundingRect(
				QRect(0, 0, innerW, 4000), Qt::AlignLeft | Qt::TextWordWrap, row.detail);
			h += br.height() + 4;
		}
		return std::max(38, h);
	}

	[[nodiscard]] int computeHeight(int w) const {
		constexpr auto kPadX = 18;
		const auto contentW = std::max(120, w - 2 * kPadX);
		auto y = 12;
		if (!_record.summary.isEmpty()) {
			const auto br = QFontMetrics(st::normalFont).boundingRect(
				QRect(0, 0, contentW, 10000), Qt::AlignLeft | Qt::TextWordWrap, _record.summary);
			y += br.height() + 14;
		}
		for (const auto &row : _record.rows) {
			y += measureRowHeight(row, contentW) + 8;
		}
		return y + 8;
	}

	ModalNotices::NoticeRecord _record;
	bool _laidOut = false;
};

class NoticeLayer final : public Ui::LayerWidget {
public:
	explicit NoticeLayer(NoticeLayerConfig config)
	: _config(std::move(config))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);
		_close->setClickedCallback([=] { closeLayer(); });

		if (!_config.primaryActionLabel.isEmpty() && _config.onPrimaryAction) {
			_buttons.push_back({ ButtonKind::Primary, _config.primaryActionLabel });
		}
		_buttons.push_back({ ButtonKind::Copy, u"Copy notice"_q });
		_buttons.push_back({ ButtonKind::Close, u"Close"_q });
		// Focus Close on arrival so Return is harmless if the notice arrives mid-keypress.
		_focusedButton = int(_buttons.size()) - 1;
		_openedAtMs = crl::now();

		auto body = object_ptr<NoticeBodyWidget>(_scroll, _config.record);
		_body = body.data();
		_scroll->setOwnedWidget(std::move(body));
	}

protected:
	void doSetInnerFocus() override {
		setFocus();
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 32, 680);
		if (_body) {
			_body->relayout(w);
		}
		constexpr auto kHeaderH = 54;
		constexpr auto kActionsH = 52;
		const auto helpH = _showHelp ? 34 : 0;
		const auto bodyH = _body ? _body->height() : 120;
		const auto maxBodyH = std::max(140, int(parentSize.height() * 0.62) - kHeaderH - kActionsH - helpH);
		const auto h = std::min(parentSize.height() - 48, kHeaderH + std::min(bodyH, maxBodyH) + helpH + kActionsH);
		setGeometry((parentSize.width() - w) / 2, (parentSize.height() - h) / 2, w, h);
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		constexpr auto kHeaderH = 54;
		constexpr auto kActionsH = 52;
		const auto helpH = _showHelp ? 34 : 0;
		if (IsRtl(this)) {
			_close->move(12, (kHeaderH - _close->height()) / 2);
		} else {
			_close->move(width() - 12 - _close->width(), (kHeaderH - _close->height()) / 2);
		}
		_scroll->setGeometry(0, kHeaderH, width(), std::max(60, height() - kHeaderH - kActionsH - helpH));
		if (_body) {
			_body->relayout(_scroll->width());
		}
		layoutButtonRects();
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		auto next = -1;
		for (auto i = 0; i < int(_buttonRects.size()); ++i) {
			if (_buttonRects[i].contains(e->pos())) {
				next = i;
				break;
			}
		}
		if (next != _hoveredButton) {
			_hoveredButton = next;
			setCursor((next >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		for (auto i = 0; i < int(_buttonRects.size()); ++i) {
			if (_buttonRects[i].contains(e->pos())) {
				_focusedButton = i;
				activateButton(i);
				return;
			}
		}
		LayerWidget::mousePressEvent(e);
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (_closing) {
			return;
		}
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
		const auto key = PhysicalKeyForNav(*e);
		const auto isActionKey = (key == Qt::Key_Return
			|| key == Qt::Key_Enter
			|| key == Qt::Key_O
			|| key == Qt::Key_Y);
		// Consume held auto-repeat and the opening gesture (180 ms arrival guard)
		// so a queued notice arriving mid-typing never triggers an action or copies.
		if (isActionKey) {
			const auto elapsed = crl::now() - _openedAtMs;
			if (e->isAutoRepeat() || elapsed < 180) {
				e->accept();
				return;
			}
		}
		if (_pendingG) {
			_pendingG = false;
			if (key == Qt::Key_G && !mods) {
				_scroll->scrollToY(0);
				return;
			}
		}
		if (key == Qt::Key_Escape
			|| (key == Qt::Key_Q && !mods)
			|| (key == Qt::Key_G && mods == kCtrlMod)) {
			closeLayer();
			return;
		} else if (key == Qt::Key_Question || (key == Qt::Key_Slash && mods == Qt::ShiftModifier)) {
			_showHelp = !_showHelp;
			parentResized();
			update();
			return;
		} else if (key == Qt::Key_G && !mods) {
			_pendingG = true;
			return;
		} else if (key == Qt::Key_G && mods == Qt::ShiftModifier) {
			_scroll->scrollToY(_scroll->scrollTopMax());
			return;
		} else if (key == Qt::Key_J && !mods) {
			scrollBy(44);
			return;
		} else if (key == Qt::Key_K && !mods) {
			scrollBy(-44);
			return;
		} else if (key == Qt::Key_Down || (key == Qt::Key_N && mods == kCtrlMod)) {
			scrollBy(44);
			return;
		} else if (key == Qt::Key_Up || (key == Qt::Key_P && mods == kCtrlMod)) {
			scrollBy(-44);
			return;
		} else if (key == Qt::Key_Tab && !mods) {
			moveButton(1);
			return;
		} else if (key == Qt::Key_Backtab || (key == Qt::Key_Tab && mods == Qt::ShiftModifier)) {
			moveButton(-1);
			return;
		} else if ((key == Qt::Key_L && !mods) || key == Qt::Key_Right) {
			moveButton(IsRtl(this) ? -1 : 1);
			return;
		} else if ((key == Qt::Key_H && !mods) || key == Qt::Key_Left) {
			moveButton(IsRtl(this) ? 1 : -1);
			return;
		} else if (key == Qt::Key_O && !mods) {
			for (auto i = 0; i < int(_buttons.size()); ++i) {
				if (_buttons[i].kind == ButtonKind::Primary) {
					_focusedButton = i;
					update();
					return;
				}
			}
			return;
		} else if (key == Qt::Key_Y && !mods) {
			triggerCopy();
			return;
		} else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
			activateButton(_focusedButton);
			return;
		}
		LayerWidget::keyPressEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), st::boxRadius, st::boxRadius);

		constexpr auto kHeaderH = 54;
		constexpr auto kActionsH = 52;
		const auto helpH = _showHelp ? 34 : 0;
		const auto rtl = IsRtl(this);
		const auto mono = MonoFont(10, true);

		// Severity badge.
		const auto sevColor = SeverityColor(_config.record.severity);
		auto sevText = SeverityBadgeText(_config.record.severity);
		if (_config.record.repeatCount > 1) {
			sevText += u" x%1"_q.arg(_config.record.repeatCount);
		}
		const auto bw = MeasureTextWidth(mono, sevText) + 12;
		const auto bx = rtl ? (_close->x() + _close->width() + 10) : (_close->x() - 10 - bw);
		const auto bRect = QRect(bx, (kHeaderH - 22) / 2, bw, 22);
		auto bFill = sevColor;
		bFill.setAlpha(28);
		p.setPen(Qt::NoPen);
		p.setBrush(bFill);
		p.drawRoundedRect(bRect, 6, 6);
		p.setFont(mono);
		p.setPen(sevColor);
		p.drawText(bRect, Qt::AlignCenter, sevText);

		// Title = account + scope.
		const auto titleW = std::max(80, width() - bw - _close->width() - 48);
		const auto titleX = rtl ? (bx + bw + 10) : 18;
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(titleX, 0, titleW, kHeaderH),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			ElideText(st::keysLegendHeaderStyle.font, _config.record.formattedTitle(), titleW));
		p.fillRect(0, kHeaderH - 1, width(), 1, st::boxDividerBg->c);

		// Optional ? help strip.
		if (_showHelp) {
			const auto helpRect = QRect(0, height() - kActionsH - helpH, width(), helpH);
			p.fillRect(helpRect, BlendColor(st::windowBg->c, st::windowBgOver->c, 0.45));
			p.fillRect(0, helpRect.top(), width(), 1, st::boxDividerBg->c);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			const auto helpText = u"j/k/gg/G scroll  ·  TAB/h/l buttons  ·  o select action  ·  RET activate  ·  y copy  ·  q/ESC close"_q;
			p.drawText(
				helpRect.adjusted(16, 0, -16, 0),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				ElideText(st::normalFont, helpText, width() - 32));
		}

		// Footer actions bar.
		const auto actionsTop = height() - kActionsH;
		p.fillRect(0, actionsTop, width(), kActionsH, BlendColor(st::windowBg->c, st::windowBgOver->c, 0.35));
		p.fillRect(0, actionsTop, width(), 1, st::boxDividerBg->c);

		if (!_copiedCue.isEmpty()) {
			p.setFont(st::semiboldFont);
			p.setPen(BadgeColor(BadgeKind::Normal));
			p.drawText(
				QRect(16, actionsTop, 140, kActionsH),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				_copiedCue);
		} else {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			const auto hint = u"? help"_q;
			p.drawText(
				QRect(rtl ? (width() - 100) : 16, actionsTop, 84, kActionsH),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				hint);
		}

		layoutButtonRects();
		for (auto i = 0; i < int(_buttons.size()); ++i) {
			const auto &btn = _buttons[i];
			const auto br = _buttonRects[i];
			const auto isFocused = (i == _focusedButton);
			const auto isHover = (i == _hoveredButton);
			const auto isPrimary = (btn.kind == ButtonKind::Primary);
			if (isPrimary) {
				auto fill = st::windowActiveTextFg->c;
				if (!isFocused && !isHover) {
					fill.setAlpha(220);
				}
				p.setPen(isFocused ? QPen(st::windowFg->c, 2) : Qt::NoPen);
				p.setBrush(fill);
				p.drawRoundedRect(QRectF(br).adjusted(.5, .5, -.5, -.5), 7, 7);
				p.setFont(st::semiboldFont);
				p.setPen(Qt::white);
				p.drawText(br, Qt::AlignCenter, btn.label);
			} else {
				const auto fill = (isFocused || isHover)
					? st::windowBgOver->c
					: st::windowBg->c;
				p.setPen(QPen(isFocused ? st::windowActiveTextFg->c : st::boxDividerBg->c, isFocused ? 2 : 1));
				p.setBrush(fill);
				p.drawRoundedRect(QRectF(br).adjusted(.5, .5, -.5, -.5), 7, 7);
				p.setFont(st::semiboldFont);
				p.setPen(st::windowFg);
				p.drawText(br, Qt::AlignCenter, btn.label);
			}
		}
	}

private:
	enum class ButtonKind { Primary, Copy, Close };
	struct ButtonDef {
		ButtonKind kind = ButtonKind::Close;
		QString label;
	};

	void layoutButtonRects() {
		constexpr auto kActionsH = 52;
		constexpr auto kBtnH = 32;
		constexpr auto kGap = 8;
		const auto y = height() - kActionsH + (kActionsH - kBtnH) / 2;
		const auto rtl = IsRtl(this);
		_buttonRects.clear();
		_buttonRects.resize(_buttons.size());
		if (rtl) {
			auto x = 16;
			for (auto i = int(_buttons.size()) - 1; i >= 0; --i) {
				const auto w = std::max(72, MeasureTextWidth(st::semiboldFont, _buttons[i].label) + 22);
				_buttonRects[i] = QRect(x, y, w, kBtnH);
				x += w + kGap;
			}
		} else {
			auto x = width() - 16;
			for (auto i = int(_buttons.size()) - 1; i >= 0; --i) {
				const auto w = std::max(72, MeasureTextWidth(st::semiboldFont, _buttons[i].label) + 22);
				x -= w;
				_buttonRects[i] = QRect(x, y, w, kBtnH);
				x -= kGap;
			}
		}
	}

	void scrollBy(int dy) {
		const auto next = std::clamp(_scroll->scrollTop() + dy, 0, _scroll->scrollTopMax());
		_scroll->scrollToY(next);
	}

	void moveButton(int delta) {
		if (_buttons.empty()) {
			return;
		}
		const auto n = int(_buttons.size());
		_focusedButton = ((_focusedButton + delta) % n + n) % n;
		update();
	}

	void triggerCopy() {
		if (_config.onCopy) {
			_config.onCopy();
		}
		_copiedCue = u"Copied"_q;
		update();
	}

	void activateButton(int index) {
		if (_closing || index < 0 || index >= int(_buttons.size())) {
			return;
		}
		switch (_buttons[index].kind) {
		case ButtonKind::Primary: {
			_closing = true;
			auto cb = std::move(_config.onPrimaryAction);
			closeLayer();
			if (cb) {
				cb();
			}
		} break;
		case ButtonKind::Copy:
			triggerCopy();
			break;
		case ButtonKind::Close:
			_closing = true;
			closeLayer();
			break;
		}
	}

	NoticeLayerConfig _config;
	Ui::IconButton *_close = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	QPointer<NoticeBodyWidget> _body;
	std::vector<ButtonDef> _buttons;
	std::vector<QRect> _buttonRects;
	int _focusedButton = 0;
	int _hoveredButton = -1;
	bool _pendingG = false;
	bool _showHelp = false;
	bool _closing = false;
	qint64 _openedAtMs = 0;
	QString _copiedCue;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

class NoticeHistoryRowsWidget final : public Ui::RpWidget {
public:
	NoticeHistoryRowsWidget(
		not_null<QWidget*> parent,
		Fn<void(int row)> onClick)
	: RpWidget(parent)
	, _onClick(std::move(onClick)) {
		setMouseTracking(true);
	}

	static constexpr int kRowHeight = 46;

	void setData(
			const std::vector<ModalNotices::NoticeRecord> &items,
			const std::vector<int> &visible,
			int selected) {
		_items = items;
		_visible = visible;
		_selected = selected;
		_hovered = -1;
		resize(width(), std::max(72, int(_visible.size()) * kRowHeight + 8));
		update();
	}

	void setSelected(int selected) {
		_selected = selected;
		update();
	}

protected:
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto row = (e->pos().y() - 4) / kRowHeight;
		const auto next = (row >= 0 && row < int(_visible.size())) ? row : -1;
		if (next != _hovered) {
			_hovered = next;
			setCursor((next >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEventHook(QEvent *) override {
		if (_hovered >= 0) {
			_hovered = -1;
			update();
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto row = (e->pos().y() - 4) / kRowHeight;
		if (row >= 0 && row < int(_visible.size()) && _onClick) {
			_onClick(row);
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto rtl = IsRtl(this);
		const auto timeFont = MonoFont(10, false);
		const auto badgeFont = MonoFont(10, true);

		if (_visible.empty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, u"No notices recorded in this session."_q);
			return;
		}

		const auto clip = e->rect();
		const auto first = std::max(0, (clip.top() - 4) / kRowHeight);
		const auto till = std::min(int(_visible.size()), (clip.bottom() - 4) / kRowHeight + 2);

		for (auto row = first; row < till; ++row) {
			const auto &item = _items[_visible[row]];
			const auto y = 4 + row * kRowHeight;
			const auto rowRect = QRect(8, y + 1, width() - 16, kRowHeight - 2);
			const auto isActive = (row == _selected);
			const auto isHover = (row == _hovered);

			if (isActive || isHover) {
				auto fill = st::windowBgOver->c;
				if (!isActive) {
					fill.setAlpha(140);
				}
				p.setPen(Qt::NoPen);
				p.setBrush(fill);
				p.drawRoundedRect(rowRect, 7, 7);
				if (isActive) {
					const auto barRect = rtl
						? QRect(rowRect.right() - 2, rowRect.top() + 6, 3, rowRect.height() - 12)
						: QRect(rowRect.left(), rowRect.top() + 6, 3, rowRect.height() - 12);
					p.setBrush(st::windowActiveTextFg->c);
					p.drawRoundedRect(barRect, 1.5, 1.5);
				}
			}

			// Severity dot.
			const auto dotColor = SeverityColor(item.severity);
			constexpr auto kDotSize = 8;
			const auto dotX = rtl ? (rowRect.right() - 14 - kDotSize) : (rowRect.left() + 14);
			p.setPen(Qt::NoPen);
			p.setBrush(dotColor);
			p.drawEllipse(QRect(dotX, rowRect.center().y() - kDotSize / 2, kDotSize, kDotSize));

			// Far side: time + optional repeat/action badge.
			auto farReserve = 12;
			const auto timeStr = (item.timestampMs > 0)
				? QDateTime::fromMSecsSinceEpoch(item.timestampMs).toString(u"HH:mm"_q)
				: QString();
			if (!timeStr.isEmpty()) {
				const auto tw = MeasureTextWidth(timeFont, timeStr);
				const auto tx = rtl ? (rowRect.left() + farReserve) : (rowRect.right() - farReserve - tw);
				p.setFont(timeFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(QRect(tx, rowRect.top() + 4, tw, 16), Qt::AlignCenter, timeStr);
				farReserve += tw + 10;
			}
			auto tagStr = QString();
			if (item.repeatCount > 1) {
				tagStr = u"x%1"_q.arg(item.repeatCount);
			} else if (!item.primaryActionLabel.isEmpty()) {
				tagStr = u"ACTION"_q;
			}
			if (!tagStr.isEmpty()) {
				const auto bw = MeasureTextWidth(badgeFont, tagStr) + 10;
				const auto bx = rtl ? (rowRect.left() + 12) : (rowRect.right() - 12 - bw);
				const auto bRect = QRect(bx, rowRect.top() + 22, bw, 17);
				auto bFill = dotColor;
				bFill.setAlpha(28);
				p.setPen(Qt::NoPen);
				p.setBrush(bFill);
				p.drawRoundedRect(bRect, 5, 5);
				p.setFont(badgeFont);
				p.setPen(dotColor);
				p.drawText(bRect, Qt::AlignCenter, tagStr);
				farReserve = std::max(farReserve, bw + 20);
			}

			const auto lead = 14 + kDotSize + 10;
			const auto contentW = std::max(80, rowRect.width() - lead - farReserve - 8);
			const auto contentX = rtl ? (rowRect.left() + farReserve + 8) : (rowRect.left() + lead);

			p.setFont(st::semiboldFont);
			p.setPen(st::windowFg);
			p.drawText(
				QRect(contentX, rowRect.top() + 4, contentW, 18),
				(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
				ElideText(st::semiboldFont, item.formattedTitle(), contentW));

			const auto sub = !item.summary.isEmpty()
				? item.summary
				: (!item.rows.empty() ? item.rows.front().label : QString());
			if (!sub.isEmpty()) {
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(
					QRect(contentX, rowRect.top() + 24, contentW, 16),
					(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
					ElideText(st::normalFont, sub, contentW));
			}
		}
	}

private:
	std::vector<ModalNotices::NoticeRecord> _items;
	std::vector<int> _visible;
	Fn<void(int)> _onClick;
	int _selected = 0;
	int _hovered = -1;
};

class NoticeHistoryLayer final : public Ui::LayerWidget {
public:
	explicit NoticeHistoryLayer(NoticeHistoryConfig config)
	: _config(std::move(config))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _filter(Ui::CreateChild<Ui::InputField>(
		this,
		st::defaultMultiSelectSearchField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Filter notices (orderless)..."_q)))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
		setFocusPolicy(Qt::StrongFocus);
		_close->setClickedCallback([=] { closeLayer(); });

		auto rows = object_ptr<NoticeHistoryRowsWidget>(
			_scroll,
			[=](int row) { inspectRow(row); });
		_rows = rows.data();
		_scroll->setOwnedWidget(std::move(rows));

		_filter->changes() | rpl::on_next([=] {
			refilter();
		}, lifetime());
		_filter->submits() | rpl::on_next([=](Qt::KeyboardModifiers) {
			inspectRow(_selected);
		}, lifetime());

		base::install_event_filter(_filter->rawTextEdit(), [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::KeyPress) {
				return base::EventFilterResult::Continue;
			}
			const auto k = static_cast<QKeyEvent*>(e.get());
			const auto layout = _filter->rawTextEdit()->textCursor().block().layout();
			if (layout && !layout->preeditAreaText().isEmpty()) {
				return base::EventFilterResult::Continue;
			}
			const auto mods = k->modifiers() & ~Qt::KeypadModifier;
			const auto key = PhysicalKeyForNav(*k);
			if (key == Qt::Key_Escape || (key == Qt::Key_G && mods == kCtrlMod)) {
				if (!_filter->getLastText().isEmpty()) {
					_filter->setText(QString());
				}
				setFocus();
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Down || ((key == Qt::Key_N || key == Qt::Key_J) && mods == kCtrlMod)) {
				moveSelection(1);
				return base::EventFilterResult::Cancel;
			} else if (key == Qt::Key_Up || ((key == Qt::Key_P || key == Qt::Key_K) && mods == kCtrlMod)) {
				moveSelection(-1);
				return base::EventFilterResult::Cancel;
			}
			return base::EventFilterResult::Continue;
		});

		refilter();
	}

protected:
	void doSetInnerFocus() override {
		setFocus();
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 32, 680);
		const auto h = std::min(parentSize.height() - 48, 560);
		setGeometry((parentSize.width() - w) / 2, (parentSize.height() - h) / 2, w, h);
	}

	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		constexpr auto kHeaderH = 54;
		constexpr auto kFilterH = 44;
		constexpr auto kFooterH = 32;
		if (IsRtl(this)) {
			_close->move(12, (kHeaderH - _close->height()) / 2);
		} else {
			_close->move(width() - 12 - _close->width(), (kHeaderH - _close->height()) / 2);
		}
		_filter->setGeometry(28, kHeaderH + 6, std::max(120, width() - 56), 32);
		_scroll->setGeometry(0, kHeaderH + kFilterH, width(), std::max(60, height() - kHeaderH - kFilterH - kFooterH));
		if (_rows) {
			_rows->resize(_scroll->width(), _rows->height());
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (_closing) {
			return;
		}
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
		const auto key = PhysicalKeyForNav(*e);
		if (_pendingG) {
			_pendingG = false;
			if (key == Qt::Key_G && !mods) {
				moveSelection(-int(_visible.size()));
				return;
			}
		}
		if (key == Qt::Key_Escape
			|| (key == Qt::Key_Q && !mods)
			|| (key == Qt::Key_G && mods == kCtrlMod)) {
			closeLayer();
			return;
		} else if (key == Qt::Key_Slash && !mods) {
			_filter->setFocusFast();
			return;
		} else if (key == Qt::Key_G && !mods) {
			_pendingG = true;
			return;
		} else if (key == Qt::Key_G && mods == Qt::ShiftModifier) {
			moveSelection(int(_visible.size()));
			return;
		} else if (key == Qt::Key_J && !mods) {
			moveSelection(1);
			return;
		} else if (key == Qt::Key_K && !mods) {
			moveSelection(-1);
			return;
		} else if (key == Qt::Key_Down || (key == Qt::Key_N && mods == kCtrlMod)) {
			moveSelection(1);
			return;
		} else if (key == Qt::Key_Up || (key == Qt::Key_P && mods == kCtrlMod)) {
			moveSelection(-1);
			return;
		} else if (key == Qt::Key_Y && !mods) {
			if (_selected >= 0 && _selected < int(_visible.size()) && _config.onCopy) {
				_config.onCopy(_config.items[_visible[_selected]]);
			}
			return;
		} else if (key == Qt::Key_C && !mods) {
			if (_config.onClear) {
				_config.onClear();
			}
			_config.items.clear();
			refilter();
			return;
		} else if (key == Qt::Key_Return || key == Qt::Key_Enter || (key == Qt::Key_O && !mods)) {
			inspectRow(_selected);
			return;
		}
		LayerWidget::keyPressEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		_roundRect.paint(p, rect());

		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), st::boxRadius, st::boxRadius);

		constexpr auto kHeaderH = 54;
		constexpr auto kFilterH = 44;
		constexpr auto kFooterH = 32;
		const auto rtl = IsRtl(this);

		const auto badgeText = u"%1 NOTICES"_q.arg(_visible.size());
		const auto mono = MonoFont(10, true);
		const auto bw = MeasureTextWidth(mono, badgeText) + 12;
		const auto bx = rtl ? (_close->x() + _close->width() + 10) : (_close->x() - 10 - bw);
		const auto bRect = QRect(bx, (kHeaderH - 22) / 2, bw, 22);
		auto bFill = st::windowActiveTextFg->c;
		bFill.setAlpha(28);
		p.setPen(Qt::NoPen);
		p.setBrush(bFill);
		p.drawRoundedRect(bRect, 6, 6);
		p.setFont(mono);
		p.setPen(st::windowActiveTextFg);
		p.drawText(bRect, Qt::AlignCenter, badgeText);

		const auto title = _config.accountLabel.isEmpty()
			? u"Notices (*Messages*)"_q
			: (_config.accountLabel + u"  ·  Notices (*Messages*)"_q);
		const auto titleW = std::max(80, width() - bw - _close->width() - 48);
		const auto titleX = rtl ? (bx + bw + 10) : 18;
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(titleX, 0, titleW, kHeaderH),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			ElideText(st::keysLegendHeaderStyle.font, title, titleW));
		p.fillRect(0, kHeaderH - 1, width(), 1, st::boxDividerBg->c);

		const auto searchRect = QRect(18, kHeaderH + 6, std::max(120, width() - 36), 32);
		p.setPen(QPen(st::boxDividerBg->c, 1));
		p.setBrush(BlendColor(st::windowBg->c, st::windowBgOver->c, 0.35));
		p.drawRoundedRect(QRectF(searchRect).adjusted(.5, .5, -.5, -.5), 8, 8);
		p.fillRect(0, kHeaderH + kFilterH - 1, width(), 1, st::boxDividerBg->c);

		const auto footerRect = QRect(0, height() - kFooterH, width(), kFooterH);
		p.fillRect(footerRect.adjusted(0, 1, 0, 0), BlendColor(st::windowBg->c, st::windowBgOver->c, 0.45));
		p.fillRect(0, height() - kFooterH, width(), 1, st::boxDividerBg->c);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto hint = u"j/k/gg/G move  ·  RET/o open  ·  / filter  ·  y copy  ·  c clear  ·  q close"_q;
		p.drawText(
			footerRect.adjusted(16, 0, -16, 0),
			(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
			ElideText(st::normalFont, hint, width() - 32));
	}

private:
	void refilter() {
		const auto tokens = ModalMx::ParseTokens(_filter->getLastText());
		_visible.clear();
		for (auto i = 0; i < int(_config.items.size()); ++i) {
			const auto &it = _config.items[i];
			if (!tokens.empty()) {
				const auto hay = it.plainText().toCaseFolded();
				const auto init = ModalMx::WordInitials(it.formattedTitle());
				const auto ok = std::all_of(tokens.begin(), tokens.end(), [&](const ModalMx::OrderlessToken &t) {
					return ModalMx::MatchToken(t, hay, init);
				});
				if (!ok) {
					continue;
				}
			}
			_visible.push_back(i);
		}
		_selected = std::clamp(_selected, 0, std::max(0, int(_visible.size()) - 1));
		if (_rows) {
			_rows->setData(_config.items, _visible, _selected);
		}
		_scroll->scrollToY(0);
		update();
	}

	void moveSelection(int delta) {
		if (_visible.empty()) {
			return;
		}
		const auto n = int(_visible.size());
		_selected = std::clamp(_selected + delta, 0, n - 1);
		if (_rows) {
			_rows->setSelected(_selected);
		}
		const auto rowTop = 4 + _selected * NoticeHistoryRowsWidget::kRowHeight;
		const auto rowBottom = rowTop + NoticeHistoryRowsWidget::kRowHeight;
		if (rowTop < _scroll->scrollTop()) {
			_scroll->scrollToY(rowTop);
		} else if (rowBottom > _scroll->scrollTop() + _scroll->height()) {
			_scroll->scrollToY(rowBottom - _scroll->height());
		}
	}

	void inspectRow(int row) {
		if (_closing || row < 0 || row >= int(_visible.size())) {
			return;
		}
		_closing = true;
		auto rec = _config.items[_visible[row]];
		auto cb = std::move(_config.onInspect);
		closeLayer();
		if (cb) {
			cb(std::move(rec));
		}
	}

	NoticeHistoryConfig _config;
	std::vector<int> _visible;
	Ui::IconButton *_close = nullptr;
	Ui::InputField *_filter = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	QPointer<NoticeHistoryRowsWidget> _rows;
	int _selected = 0;
	bool _pendingG = false;
	bool _closing = false;
	Ui::RoundRect _roundRect{ st::boxRadius, st::windowBg };
};

} // namespace

std::unique_ptr<Ui::LayerWidget> CreateModalHelpLayer(HelpData data) {
	return std::make_unique<ModalHelpLayer>(std::move(data));
}

std::unique_ptr<Ui::LayerWidget> CreateDescribeLayer(DescribeData data) {
	return std::make_unique<DescribeLayer>(std::move(data));
}

std::unique_ptr<Ui::LayerWidget> CreateCommandPaletteLayer(PaletteConfig config) {
	return std::make_unique<CommandPaletteLayer>(std::move(config));
}

std::unique_ptr<Ui::LayerWidget> CreateUnreadBufferLayer(UnreadBufferConfig config) {
	return std::make_unique<UnreadBufferLayer>(std::move(config));
}

std::unique_ptr<Ui::LayerWidget> CreateTopicPickerLayer(TopicPickerConfig config) {
	return std::make_unique<TopicPickerLayer>(std::move(config));
}

std::unique_ptr<Ui::LayerWidget> CreateNoticeLayer(NoticeLayerConfig config) {
	return std::make_unique<NoticeLayer>(std::move(config));
}

std::unique_ptr<Ui::LayerWidget> CreateNoticeHistoryLayer(NoticeHistoryConfig config) {
	return std::make_unique<NoticeHistoryLayer>(std::move(config));
}

} // namespace Purple::ModalUi
