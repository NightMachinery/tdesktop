#pragma once

#include "ui/rp_widget.h"

namespace Main { class Session; }

namespace Purple {

struct MusicShortcutHints;

struct MusicNowPlayingState {
	QString title;
	QString artist;
	QString album;
	QImage cover;
	qint64 elapsedSeconds = 0;
	qint64 durationSeconds = 0;
	double progress = 0.;
	bool active = false;
	bool playing = false;
	bool loading = false;
	bool seeking = false;
	bool previousAvailable = false;
	bool nextAvailable = false;
};

struct MusicTransportActions {
	Fn<void()> previous;
	Fn<void()> toggle;
	Fn<void()> next;
	Fn<void()> startSeeking;
	Fn<void(double)> finishSeeking;
};

[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicNowPlaying(
	QWidget *parent,
	not_null<Main::Session*> session,
	bool transport,
	const MusicShortcutHints &keys);
[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicNowPlaying(
	QWidget *parent,
	rpl::producer<MusicNowPlayingState> state,
	MusicTransportActions actions,
	const MusicShortcutHints &keys);

} // namespace Purple
