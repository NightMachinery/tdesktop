#include "purple/purple_keys_ui.h"

#include "base/event_filter.h"
#include "ui/controls/keys_legend.h"
#include "ui/painter.h"
#include "ui/round_rect.h"
#include "ui/style/style_core.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/scroll_area.h"

#include "styles/style_boxes.h"
#include "styles/style_keys_legend.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtGui/QFontDatabase>
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

[[nodiscard]] bool IsRtl(const QWidget *widget) {
	return style::RightToLeft()
		|| (widget && widget->layoutDirection() == Qt::RightToLeft);
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
		const auto elided = QFontMetrics(baseFont).elidedText(
			label,
			Qt::ElideRight,
			rect.width());
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
		const auto fm = QFontMetrics(font);
		const auto avail = maxX - x;
		const auto piece = fm.elidedText(text, Qt::ElideRight, avail);
		const auto w = fm.horizontalAdvance(piece);
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
		p.drawText(cueRect, Qt::AlignVCenter | Qt::AlignLeft, cue);
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
	const auto st = CapStyle();
	const auto sepFm = QFontMetrics(st::normalFont);
	auto maxCap = st.capMinWidth;
	const auto scan = [&](const std::vector<WhichKeyEntry> &list) {
		for (const auto &entry : list) {
			maxCap = std::max(maxCap, MeasureWhichKeyCapsWidth(entry.keys, st, sepFm));
		}
	};
	scan(_state.groups);
	scan(_state.leaves);
	_capColumnWidth = std::clamp(maxCap, 28, 96);
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
			const auto capX = rtl
				? (colX + _columnWidth - capsW)
				: (colX + (_capColumnWidth - capsW));
			PaintWhichKeyCaps(p, entry.keys, capX, midY, st, sepFm);

			const auto arrowX = rtl
				? (colX + _columnWidth - _capColumnWidth - 18)
				: (colX + _capColumnWidth + 6);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(QRect(arrowX, rowY, 14, kRowH), Qt::AlignCenter, arrowGlyph);

			const auto labelW = std::max(24, _columnWidth - _capColumnWidth - 24);
			const auto labelX = rtl ? colX : (colX + _capColumnWidth + 24);
			const auto text = entry.isGroup ? (u"+"_q + entry.label) : entry.label;
			const auto &font = entry.isGroup ? st::semiboldFont : st::normalFont;
			p.setFont(font);
			p.setPen(entry.isGroup ? st::windowActiveTextFg : st::windowFg);
			const auto elided = QFontMetrics(font).elidedText(text, Qt::ElideRight, labelW);
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
				const auto elided = QFontMetrics(st::normalFont).elidedText(
					row.label,
					Qt::ElideRight,
					textW);
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
		const auto monoFm = QFontMetrics(monoFont);

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
					: std::min(190, monoFm.horizontalAdvance(idText));
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
					QFontMetrics(st::normalFont).elidedText(row.label, Qt::ElideRight, labelW));

				if (!idText.isEmpty() && idW > 0) {
					p.setFont(monoFont);
					p.setPen(st::windowSubTextFg);
					p.drawText(
						QRect(rtl ? kPadX : (width() - kPadX - idW), y, idW, kRowH),
						(rtl ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
						monoFm.elidedText(idText, Qt::ElideRight, idW));
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
				QFontMetrics(mono).elidedText(rightText, Qt::ElideMiddle, avail));
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
	ActiveBindingsList *_bindingsWidget = nullptr;
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
		const auto parentSize = parentWidget()->size();
		const auto w = std::min(parentSize.width() - 48, 540);
		auto h = 18 + 38; // top padding + header
		if (_data.isKeyMode && !_data.queriedChord.isEmpty()) {
			h += 34;
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
		auto y = 18;

		// Header.
		p.setFont(st::keysLegendHeaderStyle.font);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(rtl ? (kPadX + 40) : kPadX, y, width() - 2 * kPadX - 40, 28),
			hAlign | Qt::AlignVCenter,
			_data.isKeyMode ? u"Describe Key"_q : u"Describe Command"_q);
		y += 38;
		p.fillRect(0, y - 6, width(), 1, st::boxDividerBg->c);

		// Key chord header in Describe Key mode.
		if (_data.isKeyMode && !_data.queriedChord.isEmpty()) {
			const auto cw = MeasureChordsWidth({ _data.queriedChord }, st, sepFm);
			PaintChords(p, { _data.queriedChord }, rtl ? (width() - kPadX - cw) : kPadX, y + 12, st);
			y += 32;
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
		const std::vector<PaletteItem> &items,
		Fn<void(int rowIndex)> onClick)
	: RpWidget(parent)
	, _items(items)
	, _onClick(std::move(onClick)) {
		setMouseTracking(true);
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
		const auto monoFm = QFontMetrics(monoFont);
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
				const auto rw = std::min(200, monoFm.horizontalAdvance(reason) + 14);
				const auto pill = rtl
					? QRect(rowRect.left() + 10, midY - 10, rw, 20)
					: QRect(rowRect.right() - 10 - rw, midY - 10, rw, 20);
				p.setPen(QPen(st::boxDividerBg->c, 1));
				p.setBrush(st::windowBg->c);
				p.drawRoundedRect(QRectF(pill).adjusted(.5, .5, -.5, -.5), 4, 4);
				p.setFont(monoFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(pill, Qt::AlignCenter, monoFm.elidedText(reason, Qt::ElideRight, rw - 8));
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

			const auto textW = std::max(80, rowRect.width() - sideReserved - 13);
			const auto leftX = rtl
				? (rowRect.left() + sideReserved)
				: (rowRect.left() + 13);
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
	const std::vector<PaletteItem> &_items;
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
		if (rowIndex < 0 || rowIndex >= int(_filtered.size())) {
			return;
		}
		const auto &item = _config.items[_filtered[rowIndex]];
		if (!_config.describeMode && !item.available) {
			return;
		}
		const auto actionId = item.actionId;
		const auto query = _input->getLastText();
		auto cb = std::move(_config.onPick);
		_config.onCancel = nullptr;
		closeLayer();
		if (cb) {
			cb(actionId, query);
		}
	}

	void cancel() {
		auto cb = std::move(_config.onCancel);
		_config.onPick = nullptr;
		closeLayer();
		if (cb) {
			cb();
		}
	}

	PaletteConfig _config;
	std::vector<ModalMx::Candidate> _candidates;
	std::vector<int> _filtered;
	Ui::InputField *_input = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	PaletteRowsWidget *_rows = nullptr;
	int _selected = 0;
	int _historyPos = -1;
	QString _draft;
	bool _steppingHistory = false;
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

} // namespace Purple::ModalUi
