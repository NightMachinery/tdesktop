#include "purple/purple_playlist_song_list.h"
#include "purple/purple_music_widgets.h"

#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/effects/animation_value.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/slide_wrap.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QKeySequence>
#include <QtGui/QWindow>
#include <QtCore/QTextBoundaryFinder>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "styles/style_media_player.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_overview.h"
#include "styles/style_purple_music.h"
#include "styles/style_widgets.h"

namespace Purple {
namespace {

constexpr auto kMusicTilesPref = "purple.music.tiles";
constexpr auto kMusicBadgeFrame = crl::time(150);
rpl::event_stream<bool> MusicTilesChanges;

QString FirstGrapheme(const QString &text) {
	const auto trimmed = text.trimmed();
	auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, trimmed);
	return trimmed.left(std::max(0, int(finder.toNextBoundary())));
}

QString PlaybackLabel(MusicRowPlayback state) {
	switch (state) {
	case MusicRowPlayback::Loading: return tr::lng_contacts_loading(tr::now);
	case MusicRowPlayback::Playing: return tr::lng_purple_music_playing(tr::now);
	case MusicRowPlayback::Paused: return tr::lng_purple_music_paused(tr::now);
	case MusicRowPlayback::Idle: return {};
	}
	Unexpected("Music row playback state.");
}

class MusicSelectionBar final : public Ui::RpWidget {
public:
	MusicSelectionBar(
		QWidget *parent,
		MusicSelectionActions actions,
		const MusicShortcutHints &shortcuts);
	void setSummary(const QString &text);

protected:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *) override;

private:
	const not_null<Ui::FlatLabel*> _summary;
	std::vector<not_null<Ui::RpWidget*>> _actions;

};

MusicSelectionBar::MusicSelectionBar(
		QWidget *parent,
		MusicSelectionActions actions,
		const MusicShortcutHints &shortcuts)
: Ui::RpWidget(parent)
, _summary(Ui::CreateChild<Ui::FlatLabel>(this, QString(), st::boxLabel)) {
	const auto add = [&](rpl::producer<QString> text, const MusicKeyHint &keys,
			Fn<void()> callback) {
		const auto action = MakeMusicActionButton(
			this, std::move(text), keys, std::move(callback)).release();
		_actions.push_back(action);
		action->naturalWidthValue() | rpl::skip(1) | rpl::on_next([=] {
			resizeToWidth(width());
		}, action->lifetime());
	};
	add(tr::lng_purple_playlists_play(), shortcuts.play, std::move(actions.play));
	add(rpl::single(u"Queue"_q), shortcuts.queue, std::move(actions.queue));
	add(tr::lng_purple_playlists_add(), shortcuts.add, std::move(actions.add));
	add(tr::lng_context_forward_msg(), shortcuts.forward, std::move(actions.forward));
	add(rpl::single(u"More"_q), shortcuts.more, std::move(actions.more));
	add(rpl::single(u"Clear"_q), shortcuts.clear, std::move(actions.clear));
}

void MusicSelectionBar::setSummary(const QString &text) {
	_summary->setText(text);
	resizeToWidth(width());
}

int MusicSelectionBar::resizeGetHeight(int width) {
	const auto gap = st::purpleMusicActionGap;
	const auto padding = st::purpleMusicBarPadding;
	const auto buttonHeight = _actions.front()->height();
	auto total = 2 * padding + st::purpleMusicTileMin;
	for (const auto button : _actions) total += button->width() + gap;
	const auto wide = width >= total;
	auto buttonsWidth = 0;
	for (auto index = 0; index < int(_actions.size()); ++index) {
		const auto button = _actions[index];
		const auto visible = wide || index == 0 || index >= 4;
		button->setVisible(visible);
		if (visible) buttonsWidth += button->width() + gap;
	}
	const auto sideBySide = width >= buttonsWidth + _summary->naturalWidth() + 2 * padding;
	_summary->resizeToWidth(std::max(1,
		(sideBySide ? width - buttonsWidth : width) - 2 * padding));
	const auto rowHeight = std::max(buttonHeight, _summary->height());
	_summary->moveToLeft(padding,
		padding + (sideBySide ? (rowHeight - _summary->height()) / 2 : 0), width);
	auto right = padding;
	for (auto i = int(_actions.size()); i > 0;) {
		const auto button = _actions[--i];
		if (button->isHidden()) continue;
		button->moveToRight(right, padding + (sideBySide
			? (rowHeight - button->height()) / 2 : _summary->height()), width);
		right += button->width() + gap;
	}
	return 2 * padding + (sideBySide ? rowHeight : buttonHeight + _summary->height());
}

void MusicSelectionBar::paintEvent(QPaintEvent *) {
	auto p = QPainter(this);
	auto quality = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(rect(), st::purpleMusicCoverRadius, st::purpleMusicCoverRadius);
}

} // namespace

bool MusicTiles() {
	return Core::App().settings().readPref<bool>(kMusicTilesPref);
}

rpl::producer<bool> MusicTilesValue() {
	return rpl::single(MusicTiles()) | rpl::then(MusicTilesChanges.events());
}

void ToggleMusicTiles() {
	const auto tiles = !MusicTiles();
	Core::App().settings().writePref<bool>(kMusicTilesPref, tiles);
	Core::App().saveSettingsDelayed();
	MusicTilesChanges.fire_copy(tiles);
}

object_ptr<Ui::RpWidget> MakeMusicSelectionBar(
		QWidget *parent,
		rpl::producer<QString> summary,
		MusicSelectionActions actions,
		const MusicShortcutHints &shortcuts) {
	auto result = object_ptr<Ui::SlideWrap<MusicSelectionBar>>(
		parent, object_ptr<MusicSelectionBar>(parent, std::move(actions), shortcuts));
	const auto wrap = result.data();
	std::move(summary) | rpl::on_next([=](const QString &text) {
		wrap->entity()->setSummary(text);
		wrap->toggle(!text.isEmpty(), anim::type::instant);
	}, wrap->lifetime());
	return result;
}

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
	[[nodiscard]] int reorderDelta(const QKeyEvent &event) const;
	void moveSelected(int delta);
	void navigate(TrackListAction action, int repeat);
	void setRows(std::vector<PlaylistSongListRow> rows, uint64_t cursor);
	void setKeyHandler(Fn<bool(QKeyEvent*)> handler);
	void setContextActions(Fn<void(not_null<Ui::PopupMenu*>, uint64_t)> actions);
	void showActions();
	void setArtworkSession(not_null<Main::Session*> session);
	int setTiles(bool tiles);
	bool cancelRangeMark();
	[[nodiscard]] int contentHeight(int width) const;
	void setPlayback(uint64_t current, uint64_t checked, QString state, bool unavailable);
	void setMusicPlayback(uint64_t current, MusicRowPlayback state, uint64_t loading);
	void setVerifiedDownloads(std::set<uint64_t> documents);
	void cancelInteraction();
	void focusSelected();
	void setSelectionActions(
		Fn<void(uint64_t)> toggle,
		Fn<bool()> playSelection,
		Fn<void(std::vector<uint64_t>)> mark);
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
	struct Cover {
		std::shared_ptr<Data::DocumentMedia> media;
		QImage scaled;
		qint64 revision = 0;
		int pixels = 0;
	};

	[[nodiscard]] int hit(QPoint position) const;
	[[nodiscard]] int columns(int width) const;
	[[nodiscard]] int sleeve(int width) const;
	[[nodiscard]] int rowHeightForWidth(int width) const;
	[[nodiscard]] QRect itemRect(int index) const;
	[[nodiscard]] QRect coverRect(QRect item) const;
	[[nodiscard]] QRect markRect(QRect item) const;
	[[nodiscard]] int previewRow(int slot) const;
	[[nodiscard]] QRect handleRect(int slot) const;
	[[nodiscard]] QString sourceState(int index) const;
	[[nodiscard]] MusicRowPlayback musicPlayback(uint64_t id) const;
	void select(int index, bool scroll = true);
	void markRange(int first, int last);
	void refreshArtwork();
	void updatePlaybackBadge();
	void paintMusicItem(Painter &p, int index) const;
	void paintCover(Painter &p, QRect rect, const PlaylistSongListRow &row) const;
	void updateDrag();
	void autoscroll();
	void submitMove(int from, int to);
	bool showActionsFor(int index, QPoint globalPosition);

	not_null<Ui::ScrollArea*> _scroll;
	std::vector<PlaylistSongListRow> _data;
	Fn<void(uint64_t)> _play;
	Fn<void(std::vector<uint64_t>)> _reorder;
	Fn<void(uint64_t)> _toggle;
	Fn<void(std::vector<uint64_t>)> _mark;
	Fn<bool()> _playSelection;
	Fn<void(uint64_t)> _copyOrgLink;
	Fn<bool(QKeyEvent*)> _keyHandler;
	Fn<void(not_null<Ui::PopupMenu*>, uint64_t)> _contextActions;
	base::unique_qptr<Ui::PopupMenu> _contextMenu;
	std::set<uint64_t> _chosen;
	std::map<uint64_t, Cover> _cover;
	Main::Session *_artworkSession = nullptr;
	Mode _mode = Mode::Playlist;
	base::Timer _autoscroll;
	base::Timer _artworkUpdate;
	base::Timer _playbackBadge;
	QString _checkedState;
	std::set<uint64_t> _verifiedDownloads;
	uint64_t _current = 0;
	uint64_t _checked = 0;
	uint64_t _loading = 0;
	MusicRowPlayback _musicState = MusicRowPlayback::Idle;
	int _selected = -1;
	int _hovered = -1;
	int _pressed = -1;
	int _target = -1;
	int _pressAnchor = -1;
	int _rangeAnchor = -1;
	int _desiredColumn = -1;
	bool _tiles = false;
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
, _autoscroll([=] { autoscroll(); })
, _artworkUpdate([=] { refreshArtwork(); })
, _playbackBadge([=] { updatePlaybackBadge(); }) {
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
	return rowHeightForWidth(width());
}

int PlaylistSongList::Rows::rowHeightForWidth(int width) const {
	if (_mode == Mode::Library) {
		return _tiles
			? sleeve(width) + st::purpleMusicTileGap + st::purpleMusicTrackInset
				+ st::semiboldFont->height * 2 + st::normalFont->height
			: st::purpleMusicTrackHeight;
	}
	const auto &layout = st::mediaPlayerFileLayout;
	return std::max(
		layout.songPadding.top() + layout.songThumbSize + layout.songPadding.bottom(),
		layout.songStatusTop + (_mode == Mode::Playlist ? 2 : 1)
			* st::normalFont->height + layout.songPadding.bottom());
}

int PlaylistSongList::Rows::columns(int width) const {
	return _tiles ? std::max(1, (width + st::purpleMusicTileGap)
		/ (st::purpleMusicTileMin + st::purpleMusicTileGap)) : 1;
}

int PlaylistSongList::Rows::sleeve(int width) const {
	const auto count = columns(width);
	const auto maximum = width <= st::purpleMusicTileNarrowWidth
		? st::purpleMusicTileNarrowMax : st::purpleMusicTileMax;
	return std::max(1, std::min(maximum,
		(width - (count - 1) * st::purpleMusicTileGap) / count));
}

int PlaylistSongList::Rows::contentHeight(int width) const {
	const auto count = columns(width);
	return ((int(_data.size()) + count - 1) / count) * rowHeightForWidth(width);
}

int PlaylistSongList::Rows::resizeGetHeight(int width) {
	if (_artworkSession) _artworkUpdate.callOnce(0);
	return contentHeight(width);
}

QRect PlaylistSongList::Rows::itemRect(int index) const {
	const auto height = rowHeight();
	if (!_tiles) return QRect(0, index * height, width(), height);
	const auto count = columns(width());
	const auto side = sleeve(width());
	const auto gap = st::purpleMusicTileGap;
	const auto leading = count > 1
		? (index % count) * (width() - side) / (count - 1) : 0;
	return style::rtlrect(
		leading,
		(index / count) * height,
		side,
		height - gap,
		width());
}

QRect PlaylistSongList::Rows::coverRect(QRect item) const {
	if (_tiles) {
		const auto inset = std::min(st::purpleMusicTrackInset, (item.width() - 1) / 2);
		const auto side = item.width() - 2 * inset;
		return QRect(item.topLeft() + QPoint(inset, inset), QSize(side, side));
	}
	const auto side = st::purpleMusicCoverSize;
	const auto left = st::purpleMusicTrackInset
		+ st::purpleMusicBadgeSize + st::purpleMusicShortcutGap;
	return style::rtlrect(left,
		item.y() + (item.height() - side) / 2, side, side, width());
}

QRect PlaylistSongList::Rows::markRect(QRect item) const {
	if (!_tiles) {
		const auto side = st::purpleMusicBadgeSize;
		return style::rtlrect(st::purpleMusicTrackInset,
			item.top() + (item.height() - side) / 2, side, side, width());
	}
	const auto cover = coverRect(item);
	const auto inset = std::min(st::purpleMusicShortcutGap, (cover.width() - 1) / 2);
	const auto side = std::min(st::purpleMusicBadgeSize, cover.width() - 2 * inset);
	return QRect(rtl() ? cover.left() + inset : (cover.right() - side + 1 - inset),
		cover.top() + inset, side, side);
}

int PlaylistSongList::Rows::hit(QPoint position) const {
	if (!rect().contains(position)) return -1;
	const auto count = columns(width());
	const auto first = (position.y() / rowHeight()) * count;
	const auto last = std::min(first + count, int(_data.size()));
	for (auto index = first; index < last; ++index) {
		if (itemRect(index).contains(position)) return index;
	}
	return -1;
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

void PlaylistSongList::Rows::setArtworkSession(not_null<Main::Session*> session) {
	_artworkSession = session;
	rpl::merge(
		_scroll->scrollTopChanges() | rpl::to_empty,
		session->downloaderTaskFinished(),
		sizeValue() | rpl::to_empty
	) | rpl::on_next([=] { _artworkUpdate.callOnce(0); }, lifetime());
}

void PlaylistSongList::Rows::refreshArtwork() {
	if (!_artworkSession || Core::App().passcodeLocked() || _data.empty()) return;
	const auto count = columns(width());
	const auto firstRow = std::max(0, _scroll->scrollTop() / rowHeight() - 1);
	const auto lastRow = (_scroll->scrollTop() + _scroll->viewport()->height())
		/ rowHeight() + 2;
	const auto last = std::min(int(_data.size()), lastRow * count);
	auto wanted = std::set<uint64_t>();
	for (auto i = firstRow * count; i < last; ++i) {
		const auto &row = _data[i];
		const auto doc = row.audioDocument;
		if (!doc || !wanted.insert(doc).second) continue;
		const auto document = _artworkSession->data().document(doc);
		auto active = document->activeMediaView();
		if (!active && !document->hasThumbnail()) continue;
		auto &cover = _cover[doc];
		if (!cover.media) {
			cover.media = active ? std::move(active) : document->createMediaView();
			if (!cover.media->thumbnail() && document->hasThumbnail()) {
				cover.media->thumbnailWanted(Data::FileOrigin(row.message));
			}
		}
		const auto thumbnail = cover.media->goodThumbnail()
			? cover.media->goodThumbnail()
			: cover.media->thumbnail() ? cover.media->thumbnail()
			: cover.media->thumbnailInline();
		if (!thumbnail) continue;
		const auto image = thumbnail->original();
		const auto pixels = int(std::ceil(coverRect(itemRect(i)).width() * devicePixelRatioF()));
		if (!image.isNull() && pixels > 0
			&& (cover.revision != image.cacheKey() || cover.pixels != pixels)) {
			cover.revision = image.cacheKey();
			cover.pixels = pixels;
			const auto side = std::min(image.width(), image.height());
			cover.scaled = image.copy((image.width() - side) / 2,
				(image.height() - side) / 2, side, side).scaled(
					pixels, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			update(itemRect(i));
		}
	}
	std::erase_if(_cover, [&](const auto &entry) { return !wanted.contains(entry.first); });
}

void PaintMusicSleeve(
		Painter &p,
		QRect rect,
		const QString &title,
		const QString &artist,
		const QImage &image) {
	auto clip = QPainterPath();
	clip.addRoundedRect(rect, st::purpleMusicCoverRadius, st::purpleMusicCoverRadius);
	p.save();
	p.setClipPath(clip, Qt::IntersectClip);
	if (!image.isNull()) {
		p.drawImage(rect, image);
	} else {
		const auto seed = (artist.isEmpty() ? title : artist)
			.normalized(QString::NormalizationForm_C).toCaseFolded();
		auto hash = uint32_t(2166136261U);
		for (const auto ch : seed) hash = (hash ^ ch.unicode()) * 16777619U;
		const auto hue = int(hash % 360);
		auto gradient = QLinearGradient(rect.topLeft(), rect.bottomRight());
		gradient.setColorAt(0., QColor::fromHsl(hue, 105, 80));
		gradient.setColorAt(1., QColor::fromHsl((hue + 32) % 360, 130, 45));
		p.fillRect(rect, gradient);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 30));
		p.drawEllipse(rect.translated(rect.width() / 2, rect.height() / 3));
		const auto letters = FirstGrapheme(title) + FirstGrapheme(artist);
		auto font = st::semiboldFont->f;
		font.setPixelSize(std::max(st::semiboldFont->height, rect.height() / 3));
		p.setFont(font);
		p.setPen(Qt::white);
		p.drawText(rect, Qt::AlignCenter, letters.isEmpty() ? u"♪"_q : letters);
	}
	p.restore();
}

void PlaylistSongList::Rows::paintCover(
		Painter &p,
		QRect rect,
		const PlaylistSongListRow &row) const {
	const auto found = _cover.find(row.audioDocument);
	PaintMusicSleeve(p, rect, row.title, row.performer,
		found == _cover.end() ? QImage() : found->second.scaled);
}

void PlaylistSongList::Rows::updatePlaybackBadge() {
	if (!isVisible() || anim::Disabled()) return;
	const auto cols = columns(width());
	const auto height = rowHeight();
	const auto top = _scroll->scrollTop();
	const auto bottom = top + _scroll->viewport()->height();
	const auto last = std::min(int(_data.size()), (bottom / height + 1) * cols);
	for (auto i = (top / height) * cols; i < last; ++i) {
		if (musicPlayback(_data[i].documentId) == MusicRowPlayback::Playing) {
			update(coverRect(itemRect(i)));
		}
	}
}

void PlaylistSongList::Rows::paintMusicItem(Painter &p, int index) const {
	const auto &row = _data[index];
	const auto item = itemRect(index);
	const auto art = coverRect(item);
	const auto playback = musicPlayback(row.documentId);
	const auto preview = _rangeAnchor >= 0
		&& index >= std::min(_rangeAnchor, _selected)
		&& index <= std::max(_rangeAnchor, _selected);
	const auto chosen = _chosen.contains(row.documentId) || preview;
	const auto accent = st::windowActiveTextFg->c;
	const auto bg = chosen ? st::windowBgRipple->c
		: index == _hovered ? st::windowBgOver->c : st::windowBg->c;
	const auto radius = st::purpleMusicCoverRadius;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(bg);
	p.drawRoundedRect(item, radius, radius);
	paintCover(p, art, row);
	if (playback != MusicRowPlayback::Idle) {
		const auto inset = std::min(
			_tiles ? st::purpleMusicShortcutGap : st::lineWidth, (art.width() - 1) / 2);
		const auto side = std::min(st::purpleMusicBadgeSize, art.width() - 2 * inset);
		const auto badge = QRect(rtl() ? art.left() + inset : art.right() - side + 1 - inset,
			art.bottom() - side + 1 - inset, side, side);
		p.setBrush(QColor(20, 20, 26, 204));
		p.drawEllipse(badge);
		p.setBrush(Qt::white);
		const auto stroke = std::max(st::lineWidth, side / 9);
		const auto bars = (playback == MusicRowPlayback::Paused) ? 2 : 3;
		const auto playing = (playback == MusicRowPlayback::Playing);
		const auto phase = playing && !anim::Disabled()
			? int((crl::now() / kMusicBadgeFrame + 1) % 3) : 1;
		for (auto i = 0; i != bars; ++i) {
			const auto x = badge.center().x() + (i * 2 - bars + 1) * stroke - stroke / 2;
			if (playback == MusicRowPlayback::Loading) {
				p.drawEllipse(x, badge.center().y() - stroke / 2, stroke, stroke);
			} else {
				const auto bar = (playing && i == phase) ? side / 2 : side / 3;
				p.drawRect(x, badge.bottom() - side / 4 - bar, stroke, bar);
			}
		}
	}
	if (chosen || index == _hovered || (hasFocus() && index == _selected)) {
		const auto mark = markRect(item);
		p.setBrush(chosen ? MusicReadableColor(accent, Qt::white, 3.) : QColor(20, 20, 26, 232));
		p.setPen(QPen(Qt::white, st::lineWidth));
		p.drawEllipse(mark.adjusted(0, 0, -st::lineWidth, -st::lineWidth));
		if (chosen) {
			const auto center = mark.center();
			const auto wing = mark.width() / 4;
			p.drawLine(center - QPoint(wing, 0), center + QPoint(0, wing / 2));
			p.drawLine(center + QPoint(0, wing / 2), center + QPoint(wing, -wing));
		}
	}
	const auto leading = rtl() ? Qt::AlignRight : Qt::AlignLeft;
	const auto trailing = rtl() ? Qt::AlignLeft : Qt::AlignRight;
	const auto gap = st::purpleMusicTrackGap;
	const auto textLeft = (rtl() ? width() - art.left() : art.right() + 1) + gap;
	const auto durationWidth = st::normalFont->width(row.duration) + gap;
	const auto sourceWidth = width() >= 4 * st::purpleMusicTileMin && !row.source.isEmpty()
		? st::purpleMusicTileMin : 0;
	const auto nameTop = item.y() + (item.height()
		- st::semiboldFont->height - st::normalFont->height) / 2;
	const auto textWidth = std::max(0,
		width() - textLeft - durationWidth - (sourceWidth ? (sourceWidth + gap) : 0));
	const auto titleLines = (_tiles && st::semiboldFont->width(row.title) > art.width())
		? 2 : 1;
	const auto title = _tiles
		? QRect(art.left(), art.bottom() + st::purpleMusicTrackInset,
			art.width(), titleLines * st::semiboldFont->height)
		: style::rtlrect(textLeft, nameTop, textWidth, st::semiboldFont->height, width());
	const auto sub = QRect(title.x(), title.bottom() + 1, title.width(), st::normalFont->height);
	p.setFont(st::semiboldFont);
	p.setPen(MusicReadableColor(playback == MusicRowPlayback::Idle
		? st::windowFg->c : accent, bg, 4.5));
	const auto alignment = _tiles ? Qt::Alignment() : Qt::Alignment(leading);
	PaintMusicText(p, title, row.title, titleLines, alignment);
	p.setFont(st::normalFont);
	p.setPen(MusicReadableColor(st::windowSubTextFg->c, bg, 4.5));
	PaintMusicText(p, sub, row.performer, 1, alignment);
	if (!_tiles) {
		const auto duration = style::rtlrect(width() - durationWidth, item.top(),
			durationWidth - st::purpleMusicTrackInset, item.height(), width());
		p.drawText(duration, trailing | Qt::AlignVCenter | Qt::TextForceLeftToRight, row.duration);
		if (sourceWidth) {
			const auto source = style::rtlrect(width() - durationWidth - sourceWidth,
				item.top(), sourceWidth - gap, item.height(), width());
			p.drawText(source,
				leading | Qt::AlignVCenter,
				st::normalFont->elided(row.source, source.width()));
		}
	}
	if (index == _selected && hasFocus()) {
		const auto line = _tiles ? st::purpleMusicTileCursor : st::lineWidth;
		const auto inset = (line + 1) / 2;
		p.setPen(QPen(MusicReadableColor(accent, bg, 3.), line));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(item.adjusted(inset, inset, -inset, -inset), radius, radius);
	}
}

void PlaylistSongList::Rows::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto visible = e->rect().intersected(QRect(
		0, _scroll->scrollTop(), width(), _scroll->viewport()->height()));
	if (visible.isEmpty()) return;
	p.setClipRect(visible);
	p.fillRect(visible, st::windowBg);
	const auto height = rowHeight();
	const auto cols = columns(width());
	const auto first = std::max(0, (visible.top() / height) * cols);
	const auto last = std::min(int(_data.size()), (visible.bottom() / height + 1) * cols);
	if (_mode == Mode::Library) {
		for (auto index = first; index < last; ++index) paintMusicItem(p, index);
		return;
	}
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
	const auto rect = itemRect(index);
	if (scroll) _scroll->scrollToY(rect.top(), rect.bottom() + 1);
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
	_pressAnchor = _selected;
	_desiredColumn = -1;
	setFocus(Qt::MouseFocusReason);
	select(_pressed, false);
	_pressPosition = e->pos();
	_handlePressed = _reorder && handleRect(_pressed).contains(e->pos());
	e->accept();
}

void PlaylistSongList::Rows::mouseMoveEvent(QMouseEvent *e) {
	if (_mode == Mode::Library) {
		const auto hovered = hit(e->pos());
		if (_hovered != hovered) {
			const auto previous = std::exchange(_hovered, hovered);
			if (previous >= 0) update(itemRect(previous));
			if (hovered >= 0) update(itemRect(hovered));
		}
	}
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
	if (!_dragging || QWidget::mouseGrabber() != this
		|| !isEnabled() || !isVisible() || !window()->isActiveWindow()) {
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
	const auto drag = _dragging && QWidget::mouseGrabber() == this;
	const auto play = !_handlePressed && from >= 0 && hit(e->pos()) == from;
	cancelInteraction();
	if (!isEnabled()) return;
	if (drag) {
		submitMove(from, to);
	} else if (play) {
		const auto id = _data[from].documentId;
		if (_toggle && (e->modifiers() & Qt::ShiftModifier) && _pressAnchor >= 0) {
			markRange(_pressAnchor, from);
		} else if (_toggle && ((e->modifiers() & Qt::ControlModifier)
			|| markRect(itemRect(from)).contains(e->pos()))) {
			const auto callback = _toggle;
			callback(id);
		} else {
			const auto callback = _play;
			callback(id);
		}
	}
}

void PlaylistSongList::Rows::mouseDoubleClickEvent(QMouseEvent *e) {
	if (_mode != Mode::Library || e->button() != Qt::LeftButton) {
		RpWidget::mouseDoubleClickEvent(e);
		return;
	}
	e->accept();
	cancelInteraction();
}

void PlaylistSongList::Rows::contextMenuEvent(QContextMenuEvent *e) {
	const auto keyboard = e->reason() == QContextMenuEvent::Keyboard;
	const auto index = keyboard ? _selected : hit(e->pos());
	if (index < 0 || !showActionsFor(index, keyboard
		? mapToGlobal(itemRect(index).bottomLeft())
		: e->globalPos())) {
		RpWidget::contextMenuEvent(e);
		return;
	}
	e->accept();
}

void PlaylistSongList::Rows::showActions() {
	if (_selected >= 0) {
		showActionsFor(_selected, mapToGlobal(itemRect(_selected).bottomLeft()));
	}
}

bool PlaylistSongList::Rows::showActionsFor(int index, QPoint globalPosition) {
	if ((!_copyOrgLink && !_contextActions) || index < 0 || !isEnabled()) return false;
	const auto document = _data[index].documentId;
	setFocus(Qt::OtherFocusReason);
	select(index);
	_contextMenu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	if (_contextActions) _contextActions(_contextMenu.get(), document);
	if (_copyOrgLink) {
		_contextMenu->addAction(tr::lng_purple_music_copy_org_link(tr::now),
			[copy = _copyOrgLink, document] { copy(document); },
			&st::menuIconCopy);
	}
	_contextMenu->popup(globalPosition);
	return true;
}

void PlaylistSongList::Rows::markRange(int first, int last) {
	if (!_toggle || first < 0 || last < 0) return;
	auto keys = std::vector<uint64_t>();
	for (auto i = std::min(first, last), end = std::max(first, last); i <= end; ++i) {
		const auto id = _data[i].documentId;
		if (!_chosen.contains(id)) keys.push_back(id);
	}
	if (const auto mark = _mark) {
		mark(std::move(keys));
	} else {
		const auto toggle = _toggle;
		for (const auto key : keys) toggle(key);
	}
}

bool PlaylistSongList::Rows::cancelRangeMark() {
	if (_rangeAnchor < 0) return false;
	_rangeAnchor = -1;
	update();
	return true;
}

int PlaylistSongList::Rows::reorderDelta(const QKeyEvent &event) const {
	if (!_reorder || _selected < 0
		|| (event.modifiers() & ~Qt::KeypadModifier) != Qt::AltModifier) {
		return 0;
	}
	if (event.key() == Qt::Key_Up) return -1;
	if (event.key() == Qt::Key_Down) return 1;
	return 0;
}

bool PlaylistSongList::ReserveReorderShortcut(
		QObject *receiver,
		QKeyEvent *event) {
	const auto rows = dynamic_cast<Rows*>(QApplication::focusWidget());
	// Cocoa delivers the shortcut override to the window before the widget.
	if (!rows || (receiver != rows && receiver != rows->window()->windowHandle())
		|| !rows->isEnabled()
		|| !rows->isVisible() || !rows->isActiveWindow()
		|| QApplication::activePopupWidget() || !rows->reorderDelta(*event)) {
		return false;
	}
	event->accept();
	return true;
}

void PlaylistSongList::Rows::moveSelected(int delta) {
	cancelInteraction();
	if (_selected < 0) return;
	submitMove(_selected, _selected + delta);
}

void PlaylistSongList::Rows::navigate(TrackListAction action, int repeat) {
	if (!isEnabled() || _data.empty()) return;
	cancelInteraction();
	const auto page = std::max(1, _scroll->viewport()->height() / rowHeight());
	const auto half = std::max(1, _scroll->viewport()->height() / (2 * rowHeight()));
	const auto count = columns(width());
	const auto size = int(_data.size());
	auto next = std::max(0, _selected);
	switch (action) {
	case TrackListAction::Up:
	case TrackListAction::Down:
	case TrackListAction::HalfUp:
	case TrackListAction::HalfDown:
	case TrackListAction::PageUp:
	case TrackListAction::PageDown: {
		const auto step = (action == TrackListAction::Up || action == TrackListAction::Down)
			? 1 : (action == TrackListAction::HalfUp || action == TrackListAction::HalfDown)
			? half : page;
		const auto up = action == TrackListAction::Up
			|| action == TrackListAction::HalfUp || action == TrackListAction::PageUp;
		if (_desiredColumn < 0) _desiredColumn = next % count;
		const auto line = std::clamp(next / count + (up ? -step : step) * repeat,
			0, (size - 1) / count);
		next = line * count + std::min(_desiredColumn, count - 1);
	} break;
	case TrackListAction::Left:
	case TrackListAction::Right: {
		if (!_tiles) return;
		const auto step = ((action == TrackListAction::Left) != rtl()) ? -1 : 1;
		const auto start = next / count * count;
		next = std::clamp(next + step * repeat, start, std::min(size, start + count) - 1);
		_desiredColumn = -1;
	} break;
	case TrackListAction::First: next = 0; _desiredColumn = -1; break;
	case TrackListAction::Last: next = size - 1; _desiredColumn = -1; break;
	case TrackListAction::Mark:
		if (const auto toggle = _toggle) toggle(_data[next].documentId);
		return;
	case TrackListAction::RangeMark:
		if (_rangeAnchor >= 0) markRange(std::exchange(_rangeAnchor, -1), next);
		else _rangeAnchor = next;
		update();
		return;
	case TrackListAction::MarkPage: {
		const auto begin = _scroll->scrollTop() / rowHeight() * count;
		const auto end = (_scroll->scrollTop() + _scroll->viewport()->height() - 1)
			/ rowHeight() * count + count;
		markRange(begin, std::min(size, end) - 1);
	} return;
	case TrackListAction::Play: {
		const auto playSelection = _playSelection;
		if (playSelection && playSelection()) return;
		const auto play = _play;
		play(_data[next].documentId);
	} return;
	}
	select(std::clamp(next, 0, size - 1));
}

void PlaylistSongList::Rows::keyPressEvent(QKeyEvent *e) {
	if (!isEnabled()) return;
	if (const auto handler = _keyHandler; handler && handler(e)) {
		e->accept();
		return;
	}
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
	if (const auto delta = reorderDelta(*e)) {
		e->accept();
		moveSelected(delta);
		return;
	}
	const auto half = (_mode == Mode::Library && e->modifiers() == Qt::AltModifier);
	const auto action = (key == Qt::Key_Up)
		? (half ? TrackListAction::HalfUp : TrackListAction::Up)
		: (key == Qt::Key_Down)
		? (half ? TrackListAction::HalfDown : TrackListAction::Down)
		: (_tiles && key == Qt::Key_Left) ? TrackListAction::Left
		: (_tiles && key == Qt::Key_Right) ? TrackListAction::Right
		: (key == Qt::Key_Home) ? TrackListAction::First
		: (key == Qt::Key_End) ? TrackListAction::Last
		: (key == Qt::Key_PageUp) ? TrackListAction::PageUp
		: (key == Qt::Key_PageDown) ? TrackListAction::PageDown
		: (key == Qt::Key_Return || key == Qt::Key_Enter) ? TrackListAction::Play
		: (key == Qt::Key_Space) ? (_toggle ? TrackListAction::Mark : TrackListAction::Play)
		: std::optional<TrackListAction>();
	if (!action) {
		RpWidget::keyPressEvent(e);
		return;
	}
	e->accept();
	if (e->isAutoRepeat() && (*action == TrackListAction::Mark
		|| (_mode == Mode::Playlist && *action == TrackListAction::Play))) {
		return;
	}
	navigate(*action, 1);
}

void PlaylistSongList::Rows::setRows(
		std::vector<PlaylistSongListRow> rows,
		uint64_t cursor) {
	cancelInteraction();
	_contextMenu.reset();
	_data = std::move(rows);
	_hovered = -1;
	_rangeAnchor = -1;
	_desiredColumn = -1;
	const auto found = std::ranges::find(_data, cursor, &PlaylistSongListRow::documentId);
	_selected = _data.empty() ? -1 : found == _data.end() ? 0 : int(found - _data.begin());
	resizeToWidth(width());
	if (_artworkSession) _artworkUpdate.callOnce(0);
	if (_selected >= 0 && hasFocus()) accessibilityChildFocused(_selected);
	update();
}

int PlaylistSongList::Rows::setTiles(bool tiles) {
	const auto current = _scroll->scrollTop();
	if (_tiles == tiles || _mode != Mode::Library) return current;
	const auto anchor = current / rowHeight() * columns(width());
	_tiles = tiles;
	_desiredColumn = -1;
	resizeToWidth(width());
	update();
	return itemRect(anchor).top();
}

void PlaylistSongList::Rows::setKeyHandler(Fn<bool(QKeyEvent*)> handler) {
	_keyHandler = std::move(handler);
}

void PlaylistSongList::Rows::setContextActions(
		Fn<void(not_null<Ui::PopupMenu*>, uint64_t)> actions) {
	_contextActions = std::move(actions);
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
	if (e->type() == QEvent::Leave && _hovered >= 0) {
		_hovered = -1;
		update();
	}
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
		Fn<bool()> playSelection,
		Fn<void(std::vector<uint64_t>)> mark) {
	_toggle = std::move(toggle);
	_playSelection = std::move(playSelection);
	_mark = std::move(mark);
}

MusicRowPlayback PlaylistSongList::Rows::musicPlayback(uint64_t id) const {
	return _mode != Mode::Library ? MusicRowPlayback::Idle
		: id == _loading ? MusicRowPlayback::Loading
		: id == _current ? _musicState
		: MusicRowPlayback::Idle;
}

void PlaylistSongList::Rows::setMusicPlayback(
		uint64_t current,
		MusicRowPlayback state,
		uint64_t loading) {
	if (_current == current && _musicState == state && _loading == loading) {
		return;
	}
	const auto oldCurrent = std::exchange(_current, current);
	const auto oldLoading = std::exchange(_loading, loading);
	_musicState = state;
	if (current && state == MusicRowPlayback::Playing) {
		_playbackBadge.callEach(kMusicBadgeFrame);
	} else {
		_playbackBadge.cancel();
	}
	for (auto i = 0; i != int(_data.size()); ++i) {
		const auto id = _data[i].documentId;
		if (id == current || id == oldCurrent || id == loading || id == oldLoading) {
			accessibilityChildNameChanged(i);
			update(accessibilityChildRect(i));
		}
	}
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
	if (_mode == Mode::Library) {
		const auto playback = PlaybackLabel(musicPlayback(row.documentId));
		return playback.isEmpty() ? label : label + u", "_q + playback;
	}
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
	state.checkable = bool(_toggle);
	state.checked = state.checkable && state.selected;
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
	return itemRect(slot);
}

bool PlaylistSongList::Rows::accessibilityChildSupportsActions(int index) const {
	return accessibilityChildIdentity(index) != 0;
}

quintptr PlaylistSongList::Rows::accessibilityChildIdentity(int index) const {
	return index >= 0 && index < int(_data.size()) ? quintptr(_data[index].documentId) : 0;
}

int PlaylistSongList::Rows::accessibilityChildIndexByIdentity(quintptr identity) const {
	const auto found = std::ranges::find(_data, uint64_t(identity), &PlaylistSongListRow::documentId);
	return found == _data.end() ? -1 : int(found - _data.begin());
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
		_rows->contentHeight(width));
	_scroll->setGeometry(0, 0, width, height);
	// Qt can defer the viewport resize while the scroll area is hidden.
	_rows->resizeToWidth(width);
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

void PlaylistSongList::navigate(TrackListAction action, int repeat) {
	_rows->navigate(action, repeat);
}

void PlaylistSongList::setRows(
		std::vector<PlaylistSongListRow> rows,
		uint64_t cursor,
		int scrollTop) {
	_rows->setRows(std::move(rows), cursor);
	_restoreScrollTop = scrollTop;
	resizeToWidth(width());
}

void PlaylistSongList::setKeyHandler(Fn<bool(QKeyEvent*)> handler) {
	_rows->setKeyHandler(std::move(handler));
}

void PlaylistSongList::setContextActions(
		Fn<void(not_null<Ui::PopupMenu*>, uint64_t)> actions) {
	_rows->setContextActions(std::move(actions));
}

void PlaylistSongList::showActions() {
	_rows->showActions();
}

void PlaylistSongList::setArtworkSession(not_null<Main::Session*> session) {
	_rows->setArtworkSession(session);
}

void PlaylistSongList::setTiles(bool tiles) {
	_restoreScrollTop = _rows->setTiles(tiles);
	resizeToWidth(width());
}

bool PlaylistSongList::cancelRangeMark() {
	return _rows->cancelRangeMark();
}

void PlaylistSongList::setPlayback(
		uint64_t current, uint64_t checked, QString state, bool unavailable) {
	_rows->setPlayback(current, checked, std::move(state), unavailable);
}

void PlaylistSongList::setMusicPlayback(
		uint64_t current,
		MusicRowPlayback state,
		uint64_t loading) {
	_rows->setMusicPlayback(current, state, loading);
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
		Fn<bool()> playSelection,
		Fn<void(std::vector<uint64_t>)> mark) {
	_rows->setSelectionActions(std::move(toggle), std::move(playSelection), std::move(mark));
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
