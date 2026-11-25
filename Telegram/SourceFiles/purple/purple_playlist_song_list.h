#pragma once

#include "data/data_msg_id.h"
#include "ui/rp_widget.h"

#include <cstdint>
#include <set>
#include <vector>

class Painter;
namespace Ui { class PopupMenu; class ScrollArea; }
namespace Main { class Session; }

namespace Purple {

enum class TrackListAction {
	Up,
	Down,
	Left,
	Right,
	HalfUp,
	HalfDown,
	PageUp,
	PageDown,
	First,
	Last,
	Play,
	Mark,
	RangeMark,
	MarkPage,
};

enum class MusicRowPlayback {
	Idle,
	Loading,
	Playing,
	Paused,
};

struct PlaylistSongListRow {
	uint64_t documentId = 0;
	QString title;
	QString performer;
	QString duration;
	FullMsgId message;
	uint64_t audioDocument = 0;
	QString source;
};

[[nodiscard]] bool MusicTiles();
[[nodiscard]] rpl::producer<bool> MusicTilesValue();
void ToggleMusicTiles();
void PaintMusicSleeve(
	Painter &painter,
	QRect rect,
	const QString &title,
	const QString &artist,
	const QImage &image);

struct MusicSelectionActions {
	Fn<void()> play;
	Fn<void()> queue;
	Fn<void()> add;
	Fn<void()> forward;
	Fn<void()> more;
	Fn<void()> clear;
};

[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicSelectionBar(
	QWidget *parent,
	rpl::producer<QString> summary,
	MusicSelectionActions actions);

class PlaylistSongList final : public Ui::RpWidget {
public:
	enum class Mode {
		Playlist,
		Library,
	};

	[[nodiscard]] static bool ReserveReorderShortcut(
		QObject *receiver,
		QKeyEvent *event);

	PlaylistSongList(
		QWidget *parent,
		std::vector<PlaylistSongListRow> rows,
		uint64_t selected,
		int scrollTop,
		Fn<void(uint64_t)> play,
		Fn<void(std::vector<uint64_t>)> reorder,
		Mode mode = Mode::Playlist);
	~PlaylistSongList();
	[[nodiscard]] uint64_t selectedDocument() const;
	[[nodiscard]] int scrollTop() const;
	[[nodiscard]] rpl::producer<> navigationEvents() const;
	void moveSelected(int delta);
	void navigate(TrackListAction action, int repeat = 1);
	void setRows(std::vector<PlaylistSongListRow> rows, uint64_t cursor, int scrollTop);
	void setKeyHandler(Fn<bool(QKeyEvent*)> handler);
	void setContextActions(Fn<void(not_null<Ui::PopupMenu*>, uint64_t)> actions);
	void setArtworkSession(not_null<Main::Session*> session);
	void setTiles(bool tiles);
	bool cancelRangeMark();
	void showActions();
	void setPlayback(uint64_t current, uint64_t checked, QString state, bool unavailable);
	void setMusicPlayback(uint64_t current, MusicRowPlayback state, uint64_t loading = 0);
	void setVerifiedDownloads(std::set<uint64_t> documents);
	void cancelInteraction();
	void focusSelected();

	void setViewportLimit(int height);
	void setSelectionActions(
		Fn<void(uint64_t)> toggle,
		Fn<bool()> playSelection,
		Fn<void(std::vector<uint64_t>)> mark = {});
	void setCopyOrgLinkAction(Fn<void(uint64_t)> copy);
	void setChosenSongs(std::set<uint64_t> songs);

protected:
	int resizeGetHeight(int width) override;

private:
	class Rows;
	not_null<Ui::ScrollArea*> _scroll;
	not_null<Rows*> _rows;
	int _restoreScrollTop = 0;
	int _viewportLimit = 0;

};

} // namespace Purple
