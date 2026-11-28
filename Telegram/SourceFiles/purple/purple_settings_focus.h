#pragma once

#include <QtCore/QPointer>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QStackedWidget>

#include <algorithm>

namespace Purple {

class SettingsFocus final {
public:
	explicit SettingsFocus(QWidget *widget = nullptr) : _widget(widget) {
		if (const auto line = qobject_cast<QLineEdit*>(widget)) {
			_position = line->cursorPosition();
			if (line->hasSelectedText()) {
				const auto start = line->selectionStart();
				const auto end = line->selectionEnd();
				_anchor = (_position == start) ? end : start;
			}
		} else if (const auto text = qobject_cast<QPlainTextEdit*>(widget)) {
			const auto cursor = text->textCursor();
			_position = cursor.position();
			_anchor = cursor.anchor();
		}
	}

	[[nodiscard]] bool restore(QWidget *within) const {
		const auto widget = _widget.data();
		if (!widget || !widget->isVisible() || !widget->isEnabled()
			|| (widget != within && !within->isAncestorOf(widget))) {
			return false;
		}
		const auto guard = QPointer(widget);
		widget->setFocus(Qt::OtherFocusReason);
		if (!guard) return false;
		if (const auto line = qobject_cast<QLineEdit*>(widget)) {
			if (_anchor >= 0) line->setSelection(_anchor, _position - _anchor);
			else line->setCursorPosition(_position);
		} else if (const auto text = qobject_cast<QPlainTextEdit*>(widget)) {
			auto cursor = text->textCursor();
			const auto limit = text->document()->characterCount() - 1;
			cursor.setPosition(std::clamp(_anchor, 0, limit));
			cursor.setPosition(std::clamp(_position, 0, limit), QTextCursor::KeepAnchor);
			text->setTextCursor(cursor);
		}
		return true;
	}

private:
	QPointer<QWidget> _widget;
	int _position = 0;
	int _anchor = -1;
};

class SettingsProtectedForm final {
public:
	void hide(QStackedWidget *pages, QWidget *form, QWidget *cover, bool retainForm) {
		if (retainForm && pages->currentWidget() == form) {
			_held = true;
			_focus = SettingsFocus(form->focusWidget());
		}
		pages->setCurrentWidget(cover);
	}

	[[nodiscard]] bool held() const { return _held; }

	[[nodiscard]] bool restore(QStackedWidget *pages, QWidget *form, bool validated) {
		if (!_held || !validated) {
			return false;
		}
		_held = false;
		pages->setCurrentWidget(form);
		if (!_focus.restore(form)) form->setFocus();
		_focus = SettingsFocus();
		return true;
	}

private:
	SettingsFocus _focus;
	bool _held = false;
};

} // namespace Purple
