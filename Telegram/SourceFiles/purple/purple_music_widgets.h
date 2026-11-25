#pragma once

#include "ui/rp_widget.h"

class QPainter;
namespace Ui { class IconButton; class InputField; }

namespace Purple {

struct MusicShortcutHints {
	QStringList previous;
	QStringList togglePlayback;
	QStringList next;
	QStringList view;
	QStringList playlists;
	QStringList play;
	QStringList queue;
	QStringList add;
	QStringList forward;
	QStringList more;
	QStringList clear;
	QStringList filter;
};

[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicKeyHint(
	QWidget *parent,
	const QStringList &keys);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicActionButton(
	QWidget *parent,
	rpl::producer<QString> label,
	const QStringList &keys,
	Fn<void()> callback);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicHeader(
	QWidget *parent,
	const QStringList &playlistKeys,
	Fn<void()> back,
	Fn<void()> playlists,
	Fn<void(not_null<Ui::IconButton*>)> more);
[[nodiscard]] object_ptr<Ui::InputField> MakeMusicFilter(
	QWidget *parent,
	const QStringList &keys);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicViewControl(
	QWidget *parent,
	rpl::producer<bool> tiles,
	Fn<void(bool)> select,
	const QStringList &keys);
void PaintMusicText(QPainter &p, const QRect &rect, const QString &text, int lines = 1);

} // namespace Purple
