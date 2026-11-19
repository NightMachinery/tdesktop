#include "purple/purple_playlist_song_list.h"

#include "base/timer.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <utility>

#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_overview.h"
#include "styles/style_widgets.h"

namespace Purple {

class PlaylistSongList::Rows final : public Ui::RpWidget {
public:
	Rows(
		QWidget *parent,
		not_null<Ui::ScrollArea*> scroll,
		std::vector<PlaylistSongListRow> rows,
		uint64_t selected,
		Fn<void(uint64_t)> play,
		Fn<void(std::vector<uint64_t>)> reorder,
		Mode mode);
	[[nodiscard]] uint64_t selectedDocument() const;
	void moveSelected(int delta);
	void setPlayback(uint64_t current, uint64_t checked, QString state, bool unavailable);
	void setVerifiedDownloads(std::set<uint64_t> documents);
	void cancelInteraction();
	void focusSelected();
	void setSelectionActions(Fn<void(uint64_t)> toggle, Fn<bool()> playSelection);
	void setCopyOrgLinkAction(Fn<void(uint64_t)> copy);
	void setChosenSongs(std::set<uint64_t> songs);
	[[nodiscard]] int rowHeight() const;
	QAccessible::Role accessibilityRole() override;
	Qt::FocusPolicy accessibilityFocusPolicy() override;
	bool accessibilitySelectionList() const override;
	int accessibilityChildCount() const override;
	QAccessible::Role accessibilityChildRole() const override;
	QString accessibilityChildName(int index) const override;
	QAccessible::State accessibilityChildState(int index) const override;
	QRect accessibilityChildRect(int index) const override;
	bool accessibilityChildSupportsActions(int index) const override;
	quintptr accessibilityChildIdentity(int index) const override;
	int accessibilityChildIndexByIdentity(quintptr identity) const override;
	void accessibilityChildSetFocus(quintptr identity) override;
	void accessibilityChildActivate(quintptr identity) override;

protected:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	bool eventHook(QEvent *e) override;

private:
	[[nodiscard]] int hit(QPoint position) const;
	[[nodiscard]] int previewRow(int slot) const;
	[[nodiscard]] QRect handleRect(int slot) const;
	[[nodiscard]] QString sourceState(int index) const;
	void select(int index, bool scroll = true);
	void updateDrag();
	void autoscroll();
	void submitMove(int from, int to);

	not_null<Ui::ScrollArea*> _scroll;
	std::vector<PlaylistSongListRow> _data;
	Fn<void(uint64_t)> _play;
	Fn<void(std::vector<uint64_t>)> _reorder;
	Fn<void(uint64_t)> _toggle;
	Fn<bool()> _playSelection;
	Fn<void(uint64_t)> _copyOrgLink;
	base::unique_qptr<Ui::PopupMenu> _contextMenu;
	std::set<uint64_t> _chosen;
	Mode _mode = Mode::Playlist;
	base::Timer _autoscroll;
	QString _checkedState;
	std::set<uint64_t> _verifiedDownloads;
	uint64_t _current = 0;
	uint64_t _checked = 0;
	int _selected = -1;
	int _pressed = -1;
	int _target = -1;
	bool _handlePressed = false;
	bool _dragging = false;
	bool _unavailable = false;
	QPoint _pressPosition;

};

PlaylistSongList::Rows::Rows(
		QWidget *parent,
		not_null<Ui::ScrollArea*> scroll,
		std::vector<PlaylistSongListRow> rows,
		uint64_t selected,
		Fn<void(uint64_t)> play,
		Fn<void(std::vector<uint64_t>)> reorder,
		Mode mode)
: RpWidget(parent)
, _scroll(scroll)
, _data(std::move(rows))
, _play(std::move(play))
, _reorder(std::move(reorder))
, _mode(mode)
, _autoscroll([=] { autoscroll(); }) {
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	setAccessibleName(_mode == Mode::Library
		? tr::lng_purple_music_title(tr::now)
		: tr::lng_purple_playlists_title(tr::now));
	if (_mode == Mode::Playlist) {
		setAccessibleDescription(tr::lng_purple_playlists_list_help(tr::now));
	}
	for (auto i = 0; i < int(_data.size()); ++i) {
		if (_data[i].documentId == selected) _selected = i;
	}
	if (_selected < 0 && !_data.empty()) _selected = 0;
}

int PlaylistSongList::Rows::rowHeight() const {
	const auto &layout = st::mediaPlayerFileLayout;
	return std::max(
		layout.songPadding.top() + layout.songThumbSize + layout.songPadding.bottom(),
		layout.songStatusTop + (_mode == Mode::Playlist ? 2 : 1)
			* st::normalFont->height + layout.songPadding.bottom());
}

int PlaylistSongList::Rows::resizeGetHeight(int) {
	return int(_data.size()) * rowHeight();
}

int PlaylistSongList::Rows::hit(QPoint position) const {
	if (!rect().contains(position)) return -1;
	const auto index = position.y() / rowHeight();
	return index < int(_data.size()) ? index : -1;
}

int PlaylistSongList::Rows::previewRow(int slot) const {
	if (!_dragging) return slot;
	if (slot == _target) return _pressed;
	if (_pressed < _target && slot >= _pressed && slot < _target) return slot + 1;
	if (_pressed > _target && slot > _target && slot <= _pressed) return slot - 1;
	return slot;
}

QRect PlaylistSongList::Rows::handleRect(int slot) const {
	const auto &layout = st::mediaPlayerFileLayout;
	const auto size = layout.songThumbSize;
	return style::rtlrect(
		width() - layout.songPadding.right() - size,
		slot * rowHeight(),
		size,
		rowHeight(),
		width());
}

QString PlaylistSongList::Rows::sourceState(int index) const {
	auto result = _data[index].documentId == _checked && !_checkedState.isEmpty()
		? _checkedState : tr::lng_purple_playlists_not_checked(tr::now);
	if (_verifiedDownloads.contains(_data[index].documentId)) {
		result += u" · "_q + tr::lng_purple_playlist_download_verified(tr::now);
	}
	return result;
}

void PlaylistSongList::Rows::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto visible = e->rect().intersected(QRect(
		0, _scroll->scrollTop(), width(), _scroll->viewport()->height()));
	if (visible.isEmpty()) return;
	p.setClipRect(visible);
	p.fillRect(visible, st::windowBg);
	const auto height = rowHeight();
	const auto first = std::max(0, visible.top() / height);
	const auto last = std::min(int(_data.size()), visible.bottom() / height + 1);
	const auto &layout = st::mediaPlayerFileLayout;
	const auto left = layout.songPadding.left() + layout.songThumbSize + layout.songPadding.right();
	const auto available = std::max(0, width() - left
		- (_mode == Mode::Playlist ? layout.songThumbSize : 0)
		- layout.songPadding.right());
	for (auto slot = first; slot < last; ++slot) {
		const auto index = previewRow(slot);
		const auto &row = _data[index];
		const auto top = slot * height;
		const auto selected = index == _selected;
		if (selected) p.fillRect(0, top, width(), height, st::windowBgOver);
		const auto thumb = style::rtlrect(
			layout.songPadding.left(),
			top + layout.songPadding.top(),
			layout.songThumbSize,
			layout.songThumbSize,
			width());
		p.setPen(Qt::NoPen);
		p.setBrush(row.documentId == _current || _chosen.contains(row.documentId)
			? st::mediaPlayerActiveFg : layout.songIconBg);
		p.drawEllipse(thumb);
		layout.songPlay.paintInCenter(p, thumb);
		p.setFont(st::semiboldFont);
		p.setPen(row.documentId == _checked && _unavailable
			? st::windowSubTextFg : st::windowFg);
		p.drawTextLeft(
			left,
			top + layout.songNameTop,
			width(),
			st::semiboldFont->elided(row.title, available));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto details = row.performer.isEmpty()
			? row.duration : row.performer + u" · "_q + row.duration;
		p.drawTextLeft(
			left,
			top + layout.songStatusTop,
			width(),
			st::normalFont->elided(details, available));
		if (_mode == Mode::Playlist) {
			const auto state = row.documentId == _current
				? tr::lng_purple_playlists_current_row(tr::now, lt_status, sourceState(index))
				: sourceState(index);
			p.drawTextLeft(
				left,
				top + layout.songStatusTop + st::normalFont->height,
				width(),
				st::normalFont->elided(state, available));
		}
		if (_reorder) st::menuIconReorder.paintInCenter(p, handleRect(slot));
		if (_dragging && slot == _target) {
			p.fillRect(0, top, width(), st::lineWidth, st::mediaPlayerActiveFg);
		}
	}
}

void PlaylistSongList::Rows::select(int index, bool scroll) {
	if (index < 0 || index >= int(_data.size())) return;
	const auto previous = _selected;
	_selected = index;
	if (scroll) _scroll->scrollToY(index * rowHeight(), (index + 1) * rowHeight());
	update();
	if (previous != index) {
		if (previous >= 0) accessibilityChildSelectionChanged(previous);
		accessibilityChildSelectionChanged(index);
		if (hasFocus()) accessibilityChildFocused(index);
	}
}

void PlaylistSongList::Rows::mousePressEvent(QMouseEvent *e) {
	if (!isEnabled() || e->button() != Qt::LeftButton) return;
	cancelInteraction();
	_pressed = hit(e->pos());
	if (_pressed < 0) return;
	setFocus(Qt::MouseFocusReason);
	select(_pressed, false);
	_pressPosition = e->pos();
	_handlePressed = _reorder && handleRect(_pressed).contains(e->pos());
	e->accept();
}

void PlaylistSongList::Rows::mouseMoveEvent(QMouseEvent *e) {
	if (_pressed < 0 || !_handlePressed || !isEnabled()) return;
	if (!_dragging
		&& (e->pos() - _pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
		_dragging = true;
		_target = _pressed;
		grabMouse();
		_autoscroll.callEach(16);
	}
	if (_dragging) updateDrag();
}

void PlaylistSongList::Rows::updateDrag() {
	if (!_dragging || _data.empty()) return;
	const auto position = mapFromGlobal(QCursor::pos());
	const auto target = std::clamp(
		position.y() / rowHeight(),
		0,
		int(_data.size()) - 1);
	if (_target != target) {
		_target = target;
		update();
	}
}

void PlaylistSongList::Rows::autoscroll() {
	if (!_dragging || !isEnabled() || !isVisible() || !window()->isActiveWindow()) {
		cancelInteraction();
		return;
	}
	const auto viewport = _scroll->viewport();
	const auto y = viewport->mapFromGlobal(QCursor::pos()).y();
	const auto edge = rowHeight();
	const auto delta = y < edge ? -std::max(1, edge / 6)
		: y >= viewport->height() - edge ? std::max(1, edge / 6) : 0;
	if (delta) _scroll->scrollToY(_scroll->scrollTop() + delta);
	updateDrag();
}

void PlaylistSongList::Rows::cancelInteraction() {
	_autoscroll.cancel();
	_dragging = false;
	_pressed = -1;
	_target = -1;
	_handlePressed = false;
	if (QWidget::mouseGrabber() == this) releaseMouse();
	update();
}

void PlaylistSongList::Rows::submitMove(int from, int to) {
	if (!_reorder || !isEnabled() || from < 0 || to < 0 || from == to
		|| from >= int(_data.size()) || to >= int(_data.size())) return;
	auto ids = std::vector<uint64_t>();
	ids.reserve(_data.size());
	for (const auto &row : _data) ids.push_back(row.documentId);
	const auto id = ids[from];
	ids.erase(ids.begin() + from);
	ids.insert(ids.begin() + to, id);
	const auto callback = _reorder;
	callback(std::move(ids));
}

void PlaylistSongList::Rows::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) return;
	const auto from = _pressed;
	const auto to = _target;
	const auto drag = _dragging;
	const auto play = !_handlePressed && from >= 0 && hit(e->pos()) == from;
	cancelInteraction();
	if (!isEnabled()) return;
	if (drag) {
		submitMove(from, to);
	} else if (play) {
		const auto id = _data[from].documentId;
		if (_toggle) {
			if (e->modifiers() & Qt::ControlModifier) {
				const auto callback = _toggle;
				callback(id);
			}
		} else {
			const auto callback = _play;
			callback(id);
		}
	}
}

void PlaylistSongList::Rows::mouseDoubleClickEvent(QMouseEvent *e) {
	const auto index = hit(e->pos());
	if (!_toggle || !isEnabled() || e->button() != Qt::LeftButton || index < 0
		|| (e->modifiers() & Qt::ControlModifier)) {
		RpWidget::mouseDoubleClickEvent(e);
		return;
	}
	e->accept();
	cancelInteraction();
	const auto callback = _play;
	callback(_data[index].documentId);
}

void PlaylistSongList::Rows::contextMenuEvent(QContextMenuEvent *e) {
	const auto keyboard = e->reason() == QContextMenuEvent::Keyboard;
	const auto index = keyboard ? _selected : hit(e->pos());
	if (!_copyOrgLink || index < 0 || !isEnabled()) {
		RpWidget::contextMenuEvent(e);
		return;
	}
	const auto document = _data[index].documentId;
	setFocus(Qt::OtherFocusReason);
	select(index);
	_contextMenu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	_contextMenu->addAction(tr::lng_purple_music_copy_org_link(tr::now),
		[copy = _copyOrgLink, document] { copy(document); },
		&st::menuIconCopy);
	_contextMenu->popup(keyboard
		? mapToGlobal(QPoint(0, (index + 1) * rowHeight()))
		: e->globalPos());
	e->accept();
}

void PlaylistSongList::Rows::moveSelected(int delta) {
	cancelInteraction();
	if (_selected < 0) return;
	submitMove(_selected, _selected + delta);
}

void PlaylistSongList::Rows::keyPressEvent(QKeyEvent *e) {
	if (!isEnabled()) return;
	if (_copyOrgLink && _selected >= 0 && e->matches(QKeySequence::Copy)) {
		e->accept();
		if (!e->isAutoRepeat()) {
			const auto copy = _copyOrgLink;
			copy(_data[_selected].documentId);
		}
		return;
	}
	const auto key = e->key();
	if (key == Qt::Key_Escape && _pressed >= 0) {
		cancelInteraction();
		e->accept();
		return;
	}
	if (_reorder && (e->modifiers() & Qt::AltModifier)
		&& (key == Qt::Key_Up || key == Qt::Key_Down)) {
		e->accept();
		moveSelected(key == Qt::Key_Up ? -1 : 1);
		return;
	}
	const auto size = int(_data.size());
	if (!size) return;
	const auto page = std::max(1, _scroll->viewport()->height() / rowHeight());
	auto next = _selected < 0 ? 0 : _selected;
	if (key == Qt::Key_Up) --next;
	else if (key == Qt::Key_Down) ++next;
	else if (key == Qt::Key_Home) next = 0;
	else if (key == Qt::Key_End) next = size - 1;
	else if (key == Qt::Key_PageUp) next -= page;
	else if (key == Qt::Key_PageDown) next += page;
	else if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
		e->accept();
		cancelInteraction();
		if (_toggle && key == Qt::Key_Space) {
			if (e->isAutoRepeat()) return;
			const auto toggle = _toggle;
			toggle(_data[next].documentId);
		} else {
			const auto playSelection = _playSelection;
			if (playSelection && playSelection()) return;
			const auto callback = _play;
			callback(_data[next].documentId);
		}
		return;
	} else {
		RpWidget::keyPressEvent(e);
		return;
	}
	e->accept();
	cancelInteraction();
	select(std::clamp(next, 0, size - 1));
}

void PlaylistSongList::Rows::focusInEvent(QFocusEvent *e) {
	RpWidget::focusInEvent(e);
	if (_selected >= 0) accessibilityChildFocused(_selected);
	update();
}

void PlaylistSongList::Rows::focusOutEvent(QFocusEvent *e) {
	cancelInteraction();
	RpWidget::focusOutEvent(e);
}

bool PlaylistSongList::Rows::eventHook(QEvent *e) {
	if (e->type() == QEvent::Hide || e->type() == QEvent::WindowDeactivate
		|| e->type() == QEvent::UngrabMouse
		|| (e->type() == QEvent::EnabledChange && !isEnabled())) {
		cancelInteraction();
	}
	return RpWidget::eventHook(e);
}

void PlaylistSongList::Rows::focusSelected() {
	cancelInteraction();
	if (_selected >= 0) select(_selected);
	setFocus();
}

uint64_t PlaylistSongList::Rows::selectedDocument() const {
	return _selected >= 0 ? _data[_selected].documentId : 0;
}

void PlaylistSongList::Rows::setPlayback(
		uint64_t current, uint64_t checked, QString state, bool unavailable) {
	if (_current == current && _checked == checked
		&& _checkedState == state && _unavailable == unavailable) return;
	const auto oldCurrent = _current;
	const auto oldChecked = _checked;
	_current = current;
	_checked = checked;
	_checkedState = std::move(state);
	_unavailable = unavailable;
	for (auto i = 0; i < int(_data.size()); ++i) {
		const auto id = _data[i].documentId;
		if (id == oldCurrent || id == oldChecked || id == current || id == checked) {
			accessibilityChildNameChanged(i);
		}
	}
	update();
}

void PlaylistSongList::Rows::setSelectionActions(
		Fn<void(uint64_t)> toggle,
		Fn<bool()> playSelection) {
	_toggle = std::move(toggle);
	_playSelection = std::move(playSelection);
}

void PlaylistSongList::Rows::setCopyOrgLinkAction(Fn<void(uint64_t)> copy) {
	_copyOrgLink = std::move(copy);
}

void PlaylistSongList::Rows::setChosenSongs(std::set<uint64_t> songs) {
	if (_chosen == songs) return;
	const auto previous = std::exchange(_chosen, std::move(songs));
	for (auto index = 0; index != int(_data.size()); ++index) {
		const auto id = _data[index].documentId;
		if (previous.contains(id) != _chosen.contains(id)) {
			accessibilityChildSelectionChanged(index);
		}
	}
	update();
}

void PlaylistSongList::Rows::setVerifiedDownloads(std::set<uint64_t> documents) {
	if (_verifiedDownloads == documents) return;
	const auto previous = std::exchange(_verifiedDownloads, std::move(documents));
	for (auto i = 0; i < int(_data.size()); ++i) {
		const auto id = _data[i].documentId;
		if (previous.contains(id) != _verifiedDownloads.contains(id)) {
			accessibilityChildNameChanged(i);
		}
	}
	update();
}

QAccessible::Role PlaylistSongList::Rows::accessibilityRole() {
	return QAccessible::List;
}

Qt::FocusPolicy PlaylistSongList::Rows::accessibilityFocusPolicy() {
	return Qt::TabFocus;
}

bool PlaylistSongList::Rows::accessibilitySelectionList() const {
	return true;
}

int PlaylistSongList::Rows::accessibilityChildCount() const {
	return int(_data.size());
}

QAccessible::Role PlaylistSongList::Rows::accessibilityChildRole() const {
	return QAccessible::ListItem;
}

QString PlaylistSongList::Rows::accessibilityChildName(int index) const {
	if (index < 0 || index >= int(_data.size())) return {};
	const auto &row = _data[index];
	auto label = row.title + u", "_q + row.performer + u", "_q + row.duration;
	if (_mode == Mode::Library) return label;
	return label + u", "_q
		+ (row.documentId == _current ? tr::lng_purple_playlists_current_row(tr::now, lt_status, sourceState(index)) : sourceState(index));
}

QAccessible::State PlaylistSongList::Rows::accessibilityChildState(int index) const {
	auto state = QAccessible::State();
	state.selectable = true;
	state.selected = _toggle && index >= 0 && index < int(_data.size())
		? _chosen.contains(_data[index].documentId)
		: index == _selected;
	state.multiSelectable = bool(_toggle);
	state.focusable = true;
	state.focused = hasFocus() && index == _selected;
	state.disabled = !isEnabled();
	const auto visible = accessibilityChildRect(index).translated(mapToGlobal(QPoint()));
	const auto viewport = QRect(_scroll->viewport()->mapToGlobal(QPoint()), _scroll->viewport()->size());
	state.offscreen = !visible.intersects(viewport);
	return state;
}

QRect PlaylistSongList::Rows::accessibilityChildRect(int index) const {
	if (index < 0 || index >= int(_data.size())) return {};
	auto slot = index;
	if (_dragging) {
		if (index == _pressed) slot = _target;
		else if (_pressed < _target && index > _pressed && index <= _target) --slot;
		else if (_pressed > _target && index >= _target && index < _pressed) ++slot;
	}
	return QRect(0, slot * rowHeight(), width(), rowHeight());
}

bool PlaylistSongList::Rows::accessibilityChildSupportsActions(int index) const {
	return accessibilityChildIdentity(index) != 0;
}

quintptr PlaylistSongList::Rows::accessibilityChildIdentity(int index) const {
	return index >= 0 && index < int(_data.size()) ? quintptr(index + 1) : 0;
}

int PlaylistSongList::Rows::accessibilityChildIndexByIdentity(quintptr identity) const {
	return identity && identity <= _data.size() ? int(identity - 1) : -1;
}

void PlaylistSongList::Rows::accessibilityChildSetFocus(quintptr identity) {
	crl::on_main(this, [=] {
		const auto index = accessibilityChildIndexByIdentity(identity);
		if (!isEnabled() || index < 0) return;
		cancelInteraction();
		select(index);
		setFocus();
	});
}

void PlaylistSongList::Rows::accessibilityChildActivate(quintptr identity) {
	crl::on_main(this, [=] {
		const auto index = accessibilityChildIndexByIdentity(identity);
		if (!isEnabled() || index < 0) return;
		cancelInteraction();
		select(index);
		const auto callback = _play;
		callback(_data[index].documentId);
	});
}

PlaylistSongList::PlaylistSongList(
		QWidget *parent,
		std::vector<PlaylistSongListRow> rows,
		uint64_t selected,
		int scrollTop,
		Fn<void(uint64_t)> play,
		Fn<void(std::vector<uint64_t>)> reorder,
		Mode mode)
: RpWidget(parent)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::mediaPlayerScroll))
, _rows(_scroll->setOwnedWidget(object_ptr<Rows>(
	_scroll,
	_scroll,
	std::move(rows),
	selected,
	std::move(play),
	std::move(reorder),
	mode)).data())
, _restoreScrollTop(scrollTop) {
	_scroll->show();
}

PlaylistSongList::~PlaylistSongList() {
	_rows->cancelInteraction();
}

int PlaylistSongList::resizeGetHeight(int width) {
	const auto height = std::min(
		_viewportLimit > 0 ? _viewportLimit : st::mediaPlayerListHeightMax,
		_rows->rowHeight() * _rows->accessibilityChildCount());
	_scroll->setGeometry(0, 0, width, height);
	_rows->resizeToWidth(_scroll->viewport()->width());
	if (_restoreScrollTop >= 0) {
		_scroll->scrollToY(_restoreScrollTop);
		_restoreScrollTop = -1;
	}
	return height;
}

uint64_t PlaylistSongList::selectedDocument() const {
	return _rows->selectedDocument();
}

int PlaylistSongList::scrollTop() const {
	return _scroll->scrollTop();
}

rpl::producer<> PlaylistSongList::navigationEvents() const {
	return rpl::merge(
		_scroll->scrollTopChanges() | rpl::to_empty,
		_rows->events() | rpl::filter([](not_null<QEvent*> event) {
			return event->type() == QEvent::FocusIn;
		}) | rpl::to_empty);
}

void PlaylistSongList::moveSelected(int delta) {
	_rows->moveSelected(delta);
}

void PlaylistSongList::setPlayback(
		uint64_t current, uint64_t checked, QString state, bool unavailable) {
	_rows->setPlayback(current, checked, std::move(state), unavailable);
}

void PlaylistSongList::setVerifiedDownloads(std::set<uint64_t> documents) {
	_rows->setVerifiedDownloads(std::move(documents));
}

void PlaylistSongList::cancelInteraction() {
	_rows->cancelInteraction();
}

void PlaylistSongList::focusSelected() {
	_rows->focusSelected();
}

void PlaylistSongList::setSelectionActions(
		Fn<void(uint64_t)> toggle,
		Fn<bool()> playSelection) {
	_rows->setSelectionActions(std::move(toggle), std::move(playSelection));
}

void PlaylistSongList::setCopyOrgLinkAction(Fn<void(uint64_t)> copy) {
	_rows->setCopyOrgLinkAction(std::move(copy));
}

void PlaylistSongList::setChosenSongs(std::set<uint64_t> songs) {
	_rows->setChosenSongs(std::move(songs));
}

void PlaylistSongList::setViewportLimit(int height) {
	if (_viewportLimit == height) return;
	_viewportLimit = height;
	resizeToWidth(width());
}

} // namespace Purple
