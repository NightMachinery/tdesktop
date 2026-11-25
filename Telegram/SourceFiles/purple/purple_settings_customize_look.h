#pragma once

#include "purple/purple_keys_colors.h"

#include <QtGui/QPainter>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>

#include <utility>
#include <vector>

namespace Purple::CustomizeLook {

inline QColor Tone(const QWidget *widget, const char *name, QPalette::ColorRole fallback) {
	for (auto at = widget; at; at = at->parentWidget()) {
		const auto color = at->property(name);
		if (color.isValid()) return color.value<QColor>();
	}
	return widget->palette().color(fallback);
}

inline void Apply(QWidget *root, QColor bg, QColor surface, QColor ink, QColor muted,
		QColor accent, QColor divider) {
	using ModalUi::BlendColor;
	using ModalUi::ReadableText;
	const auto quiet = ReadableText(muted, bg);
	const auto focus = ReadableText(accent, bg);
	const auto select = BlendColor(bg, focus, .18);
	const auto selectedInk = ReadableText(ink, select);
	const auto controlBorder = BlendColor(bg, ink, .28);
	const auto unit = root->fontMetrics().height();
	const auto radius = std::max(3, unit / 3);
	root->setProperty("customizeBackground", bg);
	root->setProperty("customizeInk", ink);
	root->setProperty("customizeSurface", surface);
	root->setProperty("customizeAccent", focus);
	root->setProperty("customizeSelectionInk", selectedInk);
	auto palette = root->palette();
	palette.setColor(QPalette::Window, bg);
	palette.setColor(QPalette::Base, surface);
	palette.setColor(QPalette::AlternateBase, surface);
	palette.setColor(QPalette::Button, surface);
	palette.setColor(QPalette::ButtonText, ink);
	palette.setColor(QPalette::Text, ink);
	palette.setColor(QPalette::WindowText, ink);
	palette.setColor(QPalette::Mid, divider);
	palette.setColor(QPalette::Highlight, select);
	palette.setColor(QPalette::HighlightedText, selectedInk);
	palette.setColor(QPalette::Link, focus);
	palette.setColor(QPalette::PlaceholderText, quiet);
	palette.setColor(QPalette::Disabled, QPalette::Text, quiet);
	root->setPalette(palette);
	root->setStyleSheet(QStringLiteral(
		"QLabel, QCheckBox, QRadioButton { color: %2; }"
		"QLabel[secondary=\"true\"] { color: %1; }"
		"QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QTreeWidget, QListWidget {"
		" color: %2; background: %3; border: 1px solid %4; border-radius: %5px;"
		" selection-background-color: %6; selection-color: %7; }"
		"QLineEdit, QPlainTextEdit, QTextEdit, QComboBox { padding: %8px; background: %11; }"
		"QComboBox QAbstractItemView { color: %2; background: %3; selection-background-color: %6; selection-color: %7; }"
		"QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus,"
		" QTreeWidget:focus, QListWidget:focus { border-color: %9; }"
		"QTreeWidget::item, QListWidget::item { padding: %10px; }"
		"QTreeWidget::item:hover:!selected, QListWidget::item:hover:!selected { background: %11; }"
		"QAbstractScrollArea { background: %3; }"
		"QScrollBar:vertical { width: %12px; background: %3; margin: 0; }"
		"QScrollBar:horizontal { height: %12px; background: %3; margin: 0; }"
		"QScrollBar::handle { background: %4; border-radius: %5px; min-height: %13px; min-width: %13px; }"
		"QScrollBar::handle:hover { background: %9; }"
		"QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
		"QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")
		.arg(quiet.name(), ink.name(), bg.name(), controlBorder.name())
		.arg(radius)
		.arg(select.name(), selectedInk.name())
		.arg(unit / 3)
		.arg(focus.name())
		.arg(unit / 4)
		.arg(surface.name())
		.arg(std::max(5, unit / 2))
		.arg(2 * unit));
	root->setAutoFillBackground(true);
}

class Action final : public QPushButton {
public:
	Action(const QString &text, QWidget *parent, QString shortcut = {})
	: QPushButton(text, parent), _shortcut(std::move(shortcut)) {
		setAutoDefault(false);
		setDefault(false);
		setAccessibleName(text);
		if (!_shortcut.isEmpty()) setAccessibleDescription(_shortcut);
		setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	}

	QSize sizeHint() const override {
		const auto fm = fontMetrics();
		const auto pad = fm.height() / 2;
		return QSize(fm.horizontalAdvance(text()) + 2 * pad
			+ (_shortcut.isEmpty() ? 0 : fm.horizontalAdvance(_shortcut) + 2 * pad),
			fm.height() + pad + 2);
	}

	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override {
		using ModalUi::BlendColor;
		using ModalUi::ReadableText;
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto bg = Tone(this, "customizeBackground", QPalette::Window);
		const auto ink = Tone(this, "customizeInk", QPalette::WindowText);
		const auto accent = Tone(this, "customizeAccent", QPalette::Link);
		const auto active = isDown() || underMouse();
		const auto surface = BlendColor(bg, accent, active && isEnabled() ? .21 : .09);
		const auto border = hasFocus() ? accent : BlendColor(bg, ink, .20);
		const auto bounds = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
		const auto fm = fontMetrics();
		const auto pad = fm.height() / 2;
		const auto radius = fm.height() / 3.;
		p.setPen(QPen(border, hasFocus() ? 2. : 1.));
		p.setBrush(surface);
		p.drawRoundedRect(bounds, radius, radius);
		p.setFont(font());
		const auto fg = ReadableText(isEnabled() ? ink : BlendColor(ink, bg, .45), surface);
		p.setPen(fg);
		const auto capW = _shortcut.isEmpty() ? 0 : fm.horizontalAdvance(_shortcut) + pad;
		const auto labelW = fm.horizontalAdvance(text());
		const auto gap = capW ? pad : 0;
		const auto start = (width() - labelW - gap - capW) / 2;
		p.drawText(QRect(start, 0, labelW, height()), Qt::AlignCenter, text());
		if (capW) {
			const auto cap = QRect(start + labelW + gap, (height() - fm.height()) / 2, capW, fm.height());
			const auto capBg = BlendColor(surface, ink, .12);
			p.setPen(Qt::NoPen);
			p.setBrush(capBg);
			p.drawRoundedRect(cap, radius / 2, radius / 2);
			p.setPen(ReadableText(fg, capBg));
			p.drawText(cap, Qt::AlignCenter, _shortcut);
		}
	}

private:
	QString _shortcut;
};

class Actions final : public QWidget {
public:
	explicit Actions(QWidget *parent) : QWidget(parent) {
		auto policy = QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}

	void append(QPushButton *action) {
		action->setParent(this);
		_actions.push_back(action);
		action->show();
		updateGeometry();
		place(width(), true);
	}

	QSize minimumSizeHint() const override {
		auto size = QSize(0, 0);
		for (const auto button : _actions) size = size.expandedTo(button->sizeHint());
		return size;
	}

	QSize sizeHint() const override {
		auto size = minimumSizeHint();
		auto width = 0;
		for (const auto button : _actions) width += button->sizeHint().width() + gap();
		size.setWidth(std::max(0, width - gap()));
		return size;
	}

	int heightForWidth(int width) const override { return place(width, false); }

protected:
	void resizeEvent(QResizeEvent *event) override {
		QWidget::resizeEvent(event);
		place(width(), true);
	}

private:
	int gap() const { return std::max(3, fontMetrics().height() / 3); }

	int place(int width, bool apply) const {
		auto x = 0;
		auto y = 0;
		auto line = 0;
		for (const auto button : _actions) {
			const auto size = button->sizeHint();
			if (x && x + size.width() > width) {
				y += line + gap();
				x = line = 0;
			}
			if (apply) button->setGeometry(QRect(QPoint(x, y), size));
			x += size.width() + gap();
			line = std::max(line, size.height());
		}
		return y + line;
	}

	std::vector<QPushButton*> _actions;
};

class Chip final : public QLabel {
public:
	explicit Chip(QWidget *parent) : QLabel(parent) {
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		setMinimumWidth(0);
		setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		setTextFormat(Qt::PlainText);
	}

	void setDetails(QString title, QString path = {}) {
		_title = std::move(title);
		_path = std::move(path);
		const auto details = _path.isEmpty() ? _title : _title + QStringLiteral(" · ") + _path;
		setToolTip(details);
		setAccessibleName(_title);
		setAccessibleDescription(details);
		refresh();
	}

protected:
	void resizeEvent(QResizeEvent *event) override {
		QLabel::resizeEvent(event);
		refresh();
	}

	void paintEvent(QPaintEvent *event) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(ModalUi::BlendColor(
			Tone(this, "customizeBackground", QPalette::Window),
			Tone(this, "customizeAccent", QPalette::Link), .09));
		p.drawRoundedRect(rect(), fontMetrics().height() / 3., fontMetrics().height() / 3.);
		p.end();
		QLabel::paintEvent(event);
	}

private:
	void refresh() {
		const auto padding = fontMetrics().height() / 2;
		setIndent(padding);
		const auto fm = fontMetrics();
		const auto available = std::max(0, width() - 2 * padding);
		if (_path.isEmpty()) {
			setText(fm.elidedText(_title, Qt::ElideRight, available));
		} else {
			const auto prefix = fm.elidedText(_title, Qt::ElideRight, available / 2) + QStringLiteral(" · ");
			setText(prefix + fm.elidedText(_path, Qt::ElideMiddle,
				std::max(0, available - fm.horizontalAdvance(prefix))));
		}
	}
	QString _title;
	QString _path;
};

class Tree final : public QTreeWidget {
public:
	explicit Tree(QWidget *parent) : QTreeWidget(parent) {
		setAlternatingRowColors(false);
		setIndentation(fontMetrics().height());
	}

protected:
	void drawBranches(QPainter *p, const QRect &rect, const QModelIndex &index) const override {
		if (!model()->hasChildren(index)) {
			return;
		}
		const auto step = std::max(2, fontMetrics().height() / 5);
		const auto x = rect.right() - indentation() / 2;
		const auto y = rect.center().y();
		const auto color = selectionModel()->isSelected(index)
			? Tone(this, "customizeSelectionInk", QPalette::HighlightedText)
			: Tone(this, "customizeAccent", QPalette::Link);
		p->save();
		p->setRenderHint(QPainter::Antialiasing);
		p->setPen(QPen(color, std::max(1., step / 2.), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		const auto points = isExpanded(index)
			? QPolygon{ QPoint(x - step, y - step / 2), QPoint(x, y + step / 2), QPoint(x + step, y - step / 2) }
			: QPolygon{ QPoint(x - step / 2, y - step), QPoint(x + step / 2, y), QPoint(x - step / 2, y + step) };
		p->drawPolyline(points);
		p->restore();
	}
};

} // namespace Purple::CustomizeLook
