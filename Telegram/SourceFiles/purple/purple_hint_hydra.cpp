#include "purple/purple_hint_hydra.h"
#include "purple/purple_hint_mnemonics.h"
#include "purple/purple_hint_paint.h"
#include "purple/purple_keys.h"

#include "lang/lang_keys.h"
#include "ui/layers/layer_widget.h"
#include "ui/painter.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"

#include "styles/palette.h"
#include "styles/style_boxes.h"
#include "styles/style_keys_legend.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyle>

#include <algorithm>

namespace Purple {
namespace {

using Kind = WindowHintTarget::Kind;

char KnownKey(Kind kind, const QString &text) {
	if (kind == Kind::Folder) {
		if (text == tr::lng_filters_context_edit(tr::now)) return 'e';
		if (text == tr::lng_context_mark_read_all(tr::now)) return 'r';
		if (text == tr::lng_filters_context_remove(tr::now)) return 'd';
	} else if (kind == Kind::Chat) {
		if (text == tr::lng_context_mark_read(tr::now) || text == tr::lng_context_mark_unread(tr::now)) return 'r';
		if (text == tr::lng_context_mute(tr::now) || text == tr::lng_context_unmute(tr::now)) return 'm';
		if (text == tr::lng_filters_menu_add(tr::now)) return 'f';
		if (text == tr::lng_context_new_window(tr::now)) return 'w';
		if (text == tr::lng_context_view_profile(tr::now) || text == tr::lng_context_view_thread(tr::now)) return 'i';
		if (text == tr::lng_archived_add(tr::now) || text == tr::lng_archived_remove(tr::now)) return 'a';
		if (text == tr::lng_profile_delete_conversation(tr::now) || text == tr::lng_profile_leave_channel(tr::now)
			|| text == tr::lng_profile_leave_group(tr::now) || text == tr::lng_profile_delete_channel(tr::now)
			|| text == tr::lng_profile_delete_group(tr::now)) return 'd';
	} else if (kind == Kind::Message) {
		if (text == tr::lng_context_copy_text(tr::now) || text == tr::lng_context_copy_selected(tr::now)) return 'y';
		if (text == tr::lng_context_reply_msg(tr::now) || text == tr::lng_context_quote_and_reply(tr::now)) return 'r';
		if (text == tr::lng_context_edit_msg(tr::now)) return 'e';
		if (text == tr::lng_context_forward_msg(tr::now)) return 'f';
		if (text == tr::lng_context_select_msg(tr::now) || text == tr::lng_context_deselect_msg(tr::now)) return 's';
		if (text == tr::lng_context_translate(tr::now)) return 't';
		if (text == tr::lng_context_copy_message_link(tr::now) || text == tr::lng_context_copy_post_link(tr::now)) return 'l';
		if (text == tr::lng_context_copy_link(tr::now)) return 'u';
	}
	if (text == tr::lng_context_delete_msg(tr::now)) return 'd';
	if (text == tr::lng_context_pin_msg(tr::now) || text == tr::lng_context_unpin_msg(tr::now)
		|| text == tr::lng_context_pin_to_top(tr::now) || text == tr::lng_context_unpin_from_top(tr::now)) {
		return 'p';
	}
	return 0;
}

const style::icon &MenuIcon(Kind kind, char key) {
	switch (key) {
	case 'y': return st::menuIconCopy;
	case 'r': return (kind == Kind::Message) ? st::menuIconReply : st::menuIconSelect;
	case 'e': return st::menuIconEdit;
	case 'd': return st::menuIconDeleteAttention;
	case 'f': return st::menuIconForward;
	case 's': return st::menuIconSelect;
	case 'p': return st::menuIconPin;
	case 't': return st::menuIconTranslate;
	case 'a': return st::menuIconArchive;
	case 'l':
	case 'u': return st::menuIconLink;
	case 'm': return (kind == Kind::Chat) ? st::menuIconMute : st::menuIconInfo;
	case 'R': return st::menuIconReactions;
	default: return st::menuIconInfo;
	}
}

QString ShortcutName(QString sequence) {
	return sequence.replace(u" "_q, u"SPC "_q);
}

class HintHydra final : public Ui::LayerWidget {
public:
	HintHydra(WindowHintTarget target, WindowHintTarget::Menu menu, Fn<bool()> allowed, Fn<void()> stale)
	: _target(std::move(target))
	, _menu(std::move(menu))
	, _allowed(std::move(allowed))
	, _stale(std::move(stale))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
		setFocusPolicy(Qt::StrongFocus);
		setAccessibleName(u"Hint actions"_q);
		auto rows = object_ptr<Rows>(_scroll, this);
		_rows = rows.data();
		_scroll->setOwnedWidget(std::move(rows));
		load(_menu.get());
	}

	void parentResized() override {
		if (!parentWidget()) {
			return;
		}
		const auto parent = parentWidget()->rect().marginsRemoved(st::boxPadding);
		const auto w = std::min(parent.width(), st::boxWideWidth);
		const auto h = std::min(parent.height(), rowsHeight() + headerHeight() + st::boxPadding.bottom());
		setGeometry(QRect(parent.center() - QPoint(w / 2, h / 2), QSize(w, h)));
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		LayerWidget::resizeEvent(e);
		_scroll->setGeometry(0, headerHeight(), width(), std::max(0, height() - headerHeight() - st::boxPadding.bottom()));
		_rows->resize(_scroll->width(), rowsHeight());
	}

	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		p.fillRect(rect(), st::windowBg->c);
		auto title = _target.name.isEmpty() ? u"Hint actions"_q : _target.name;
		if (!_path.empty()) {
			title += u" / "_q + _path.back().title;
		}
		const auto interior = rect().marginsRemoved(st::boxPadding);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg->c);
		p.drawText(QRect(interior.topLeft(), QSize(interior.width(), st::semiboldFont->height)),
			Qt::AlignVCenter | Qt::AlignLeading, p.fontMetrics().elidedText(title, Qt::ElideRight, interior.width()));
		const auto hint = _describing
			? u"j/k select · RET activate · Backspace up · Esc close"_q
			: _prefix.isEmpty() ? u"RET default · j/k move · ? help · Esc close"_q : ShortcutName(_prefix);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg->c);
		p.drawText(QRect(interior.left(), headerHeight() - st::normalFont->height - st::keysLegendCapSkip,
			interior.width(), st::normalFont->height), Qt::AlignVCenter | Qt::AlignLeading,
			p.fontMetrics().elidedText(hint, Qt::ElideRight, interior.width()));
	}

	void keyPressEvent(QKeyEvent *e) override {
		const auto key = ModalPhysicalKey(*e);
		const auto mods = e->modifiers() & ~Qt::KeypadModifier;
#ifdef Q_OS_MAC
		const auto ctrl = Qt::MetaModifier;
#else
		const auto ctrl = Qt::ControlModifier;
#endif
		if (!AllowModalLayerKey(*e, this)) {
			e->accept();
			return;
		}
		e->accept();
		if (!live()) {
			return;
		}
		if (key == Qt::Key_Escape || (key == Qt::Key_G && mods == ctrl)) {
			closeLayer();
		} else if (key == Qt::Key_Backspace) {
			if (!_prefix.isEmpty()) {
				_prefix.chop(1);
				update();
			} else if (!_path.empty()) {
				const auto parent = _path.back().parent;
				_path.pop_back();
				load(parent);
			}
		} else if (key == Qt::Key_Question) {
			_describing = !_describing;
			update();
		} else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
			if (!_moved && _path.empty() && _default >= 0) {
				choose(_default);
			} else if (!_actions.empty() && (_moved || !_path.empty())) {
				choose(_selected);
			}
		} else if ((key == Qt::Key_J || key == Qt::Key_K) && !mods && _prefix.isEmpty()) {
			move(key == Qt::Key_J ? 1 : -1);
		} else if ((key == Qt::Key_Down || key == Qt::Key_Up) && !mods) {
			move(key == Qt::Key_Down ? 1 : -1);
		} else if ((key == Qt::Key_Space || (key >= Qt::Key_A && key <= Qt::Key_Z))
			&& (!mods || mods == Qt::ShiftModifier)) {
			const auto letter = (key == Qt::Key_Space) ? ' '
				: ((mods == Qt::ShiftModifier ? 'A' : 'a') + key - Qt::Key_A);
			_prefix += QChar(ushort(letter));
			const auto found = ranges::find_if(_actions, [&](const auto &action) {
				return action.key.startsWith(_prefix);
			});
			if (found == end(_actions)) {
				_prefix.clear();
			} else if (found->key == _prefix) {
				choose(int(found - begin(_actions)));
				return;
			}
			update();
		}
	}

private:
	struct Action {
		QString title;
		QString key;
		char known = 0;
		QPointer<QAction> native;
		QPointer<Ui::PopupMenu> child;
		Fn<void()> callback;
	};
	struct Branch {
		QPointer<Ui::PopupMenu> parent;
		QString title;
	};
	class Rows final : public Ui::RpWidget {
	public:
		Rows(QWidget *parent, HintHydra *owner) : RpWidget(parent), _owner(owner) {
		}
	protected:
		void paintEvent(QPaintEvent *e) override { _owner->paintRows(this, e->rect()); }
		void mouseReleaseEvent(QMouseEvent *e) override {
			if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && _owner->live()) {
				_owner->choose(e->pos().y() / _owner->rowHeight());
			}
		}
	private:
		const not_null<HintHydra*> _owner;
	};

	int headerHeight() const { return st::boxPadding.top() + st::semiboldFont->height + rowHeight(); }
	int rowHeight() const { return std::max(st::keysLegendCapHeight, st::menuIconCopy.height()) + st::keysLegendCapSkip; }
	int rowsHeight() const { return int(_actions.size()) * rowHeight(); }

	void load(Ui::PopupMenu *menu) {
		_current = menu;
		_actions.clear();
		_prefix.clear();
		_default = -1;
		_moved = false;
		if (_path.empty() && _target.activate && _target.kind != Kind::Message) {
			_default = 0;
			_actions.push_back({
				.title = (_target.kind == Kind::Control) ? u"Activate"_q : u"Open"_q,
				.known = 'o',
				.callback = _target.activate,
			});
		}
		if (menu) {
			for (const auto action : menu->actions()) {
				const auto item = menu->menu()->itemForAction(action);
				if (action->isSeparator() || action->text().isEmpty() || !action->isVisible()
					|| !action->isEnabled() || (item && !item->isEnabled())) {
					continue;
				}
				const auto title = action->text();
				const auto known = KnownKey(_target.kind, title);
				if (_path.empty() && _target.reply && known == 'r' && _default < 0) {
					_default = int(_actions.size());
				}
				_actions.push_back({
					.title = title,
					.known = known,
					.native = action.get(),
					.child = action->menu() ? menu->ensureSubmenu(action, menu->st()).get() : nullptr,
				});
			}
		}
		if (_path.empty() && _target.fullMenu) {
			if (_target.canReact) {
				_actions.push_back({ .title = u"React (native picker)"_q, .known = 'R', .callback = _target.fullMenu });
			}
			_actions.push_back({ .title = u"Full native menu"_q, .known = 'm', .callback = _target.fullMenu });
		}
		auto requests = std::vector<HintMnemonic>();
		for (const auto &action : _actions) {
			requests.push_back({ action.known, action.title.toCaseFolded().toStdString() });
		}
		const auto assigned = HintMnemonics(requests);
		for (auto i = std::size_t(0); i < assigned.size(); ++i) {
			_actions[i].key = QString::fromStdString(assigned[i]);
		}
		_selected = std::max(_default, 0);
		_rows->resize(_scroll->width(), rowsHeight());
		_scroll->scrollToY(0);
		_rows->update();
		parentResized();
		update();
	}

	bool live() {
		if (_allowed() && _target.widget && _target.current()
			&& WindowHintClip(_target.widget).contains(_target.bounds.center())) {
			return true;
		}
		const auto stale = _stale;
		closeLayer();
		stale();
		return false;
	}

	void choose(int index) {
		if (index < 0 || index >= int(_actions.size())) {
			return;
		}
		const auto action = _actions[index];
		if (action.child) {
			_path.push_back({ _current, action.title });
			load(action.child);
			return;
		}
		const auto trigger = [&] {
			if (action.native && action.native->isEnabled()) {
				action.native->trigger();
			} else if (action.callback) {
				action.callback();
			}
		};
		if (action.known == 's' && _target.kind == Kind::Message && _target.nativeMenu) {
			const auto guard = QPointer(this);
			trigger();
			if (guard && live()) {
				_menu = _target.nativeMenu();
				_path.clear();
				load(_menu.get());
			}
		} else {
			const auto allowed = _allowed;
			const auto current = _target.current;
			const auto menu = std::move(_menu);
			closeLayer();
			if (allowed() && current()) {
				trigger();
			}
		}
	}

	void move(int delta) {
		if (!_actions.empty()) {
			_moved = true;
			_selected = std::clamp(_selected + delta, 0, int(_actions.size()) - 1);
			_scroll->scrollToY(_selected * rowHeight(), (_selected + 1) * rowHeight());
			_rows->update();
		}
	}

	void paintRows(QWidget *widget, QRect clip) const {
		auto p = QPainter(widget);
		p.fillRect(clip, st::windowBg->c);
		p.setFont(st::normalFont);
		const auto height = rowHeight();
		const auto fm = p.fontMetrics();
		const auto pad = st::boxPadding.left();
		const auto cap = fm.horizontalAdvance(u"SPC XX"_q) + st::keysLegendCapSkip * 2;
		for (auto i = std::max(clip.top() / height, 0); i < int(_actions.size()) && i * height <= clip.bottom(); ++i) {
			const auto &action = _actions[i];
			const auto row = QRect(0, i * height, widget->width(), height);
			if (i == _selected) {
				p.fillRect(row, st::windowBgOver->c);
			}
			const auto mirrored = widget->layoutDirection() == Qt::RightToLeft;
			const auto orient = [&](QRect rect) {
				return QStyle::visualRect(widget->layoutDirection(), row, rect);
			};
			const auto capRect = orient(QRect(pad, row.y() + (height - st::keysLegendCapHeight) / 2,
				cap, st::keysLegendCapHeight));
			PaintHydraCap(p, capRect, ShortcutName(action.key), st::keysLegendCapBg->c,
				st::keysLegendCapBorder->c, st::keysLegendCapFg->c, st::keysLegendCapRadius);
			const auto &icon = MenuIcon(_target.kind, action.known);
			const auto iconX = pad + cap + st::keysLegendCapSkip;
			const auto iconRect = orient(QRect(iconX, row.y() + (height - icon.height()) / 2, icon.width(), icon.height()));
			if (action.native && !action.native->icon().isNull()) {
				action.native->icon().paint(&p, iconRect);
			} else {
				icon.paint(p, iconRect.x(), iconRect.y(), widget->width());
			}
			const auto textX = iconX + icon.width() + st::keysLegendCapSkip;
			const auto textRect = orient(QRect(textX, row.y(), std::max(0, row.width() - textX - pad), height));
			auto title = action.title;
			if (action.child) {
				title += mirrored ? u"  ‹"_q : u"  ›"_q;
			}
			p.setPen(st::windowFg->c);
			p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeading, fm.elidedText(title, Qt::ElideRight, textRect.width()));
		}
	}

	WindowHintTarget _target;
	WindowHintTarget::Menu _menu;
	Fn<bool()> _allowed;
	Fn<void()> _stale;
	const not_null<Ui::ScrollArea*> _scroll;
	Rows *_rows = nullptr;
	QPointer<Ui::PopupMenu> _current;
	std::vector<Action> _actions;
	std::vector<Branch> _path;
	QString _prefix;
	int _default = -1;
	int _selected = 0;
	bool _moved = false;
	bool _describing = false;
};

} // namespace

std::unique_ptr<Ui::LayerWidget> CreateHintHydra(
		WindowHintTarget target,
		WindowHintTarget::Menu menu,
		Fn<bool()> allowed,
		Fn<void()> stale) {
	return std::make_unique<HintHydra>(std::move(target), std::move(menu), std::move(allowed), std::move(stale));
}

} // namespace Purple
