#pragma once

#include "base/object_ptr.h"
#include "purple/purple_playlist_song_list.h"
#include "ui/rp_widget.h"

#include <optional>

class QPainter;
namespace Ui {
class FlatLabel;
class GenericBox;
class IconButton;
class InputField;
class RoundButton;
} // namespace Ui

namespace Purple {

using MusicKeyHint = rpl::producer<QStringList>;

struct MusicShortcutHints {
	MusicKeyHint previous;
	MusicKeyHint togglePlayback;
	MusicKeyHint next;
	MusicKeyHint view;
	MusicKeyHint playlists;
	MusicKeyHint play;
	MusicKeyHint queue;
	MusicKeyHint add;
	MusicKeyHint forward;
	MusicKeyHint more;
	MusicKeyHint clear;
	MusicKeyHint filter;
};

struct ChatMusicLayout {
	Ui::InputField *filter = nullptr;
	Ui::FlatLabel *status = nullptr;
	PlaylistSongList *tracks = nullptr;
	Ui::RoundButton *loadMore = nullptr;
};

struct ChatMusicLayoutArgs {
	object_ptr<Ui::RpWidget> player;
	rpl::producer<bool> tiles;
	rpl::producer<QString> marks;
	MusicSelectionActions selection;
	Fn<void(bool)> selectView;
	Fn<void(uint64_t)> play;
	Fn<void()> loadMore;
};

[[nodiscard]] int ChatMusicWidth(int windowWidth);
[[nodiscard]] ChatMusicLayout AddChatMusicLayout(
	not_null<Ui::GenericBox*> box,
	ChatMusicLayoutArgs args,
	const MusicShortcutHints &keys);

[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicKeyHint(
	QWidget *parent,
	const MusicKeyHint &keys,
	QWidget *accessibleOwner = nullptr);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicActionButton(
	QWidget *parent,
	rpl::producer<QString> label,
	const MusicKeyHint &keys,
	Fn<void()> callback);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicHeader(
	QWidget *parent,
	const MusicKeyHint &playlistKeys,
	Fn<void()> back,
	Fn<void()> playlists,
	Fn<void(not_null<Ui::IconButton*>, std::optional<QString>)> more);
[[nodiscard]] object_ptr<Ui::InputField> MakeMusicFilter(
	QWidget *parent,
	const MusicKeyHint &keys);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicViewControl(
	QWidget *parent,
	rpl::producer<bool> tiles,
	Fn<void(bool)> select,
	const MusicKeyHint &keys);
[[nodiscard]] QColor MusicReadableColor(QColor foreground, QColor background, double minimum);
void PaintMusicText(
	QPainter &p,
	const QRect &rect,
	const QString &text,
	int lines = 1,
	Qt::Alignment horizontal = {});

} // namespace Purple
