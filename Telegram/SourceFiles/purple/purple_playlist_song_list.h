#pragma once

#include "ui/rp_widget.h"

#include <cstdint>
#include <set>
#include <vector>

namespace Ui { class ScrollArea; }

namespace Purple {

struct PlaylistSongListRow {
	uint64_t documentId = 0;
	QString title;
	QString performer;
	QString duration;
};

class PlaylistSongList final : public Ui::RpWidget {
public:
	enum class Mode {
		Playlist,
		Library,
	};

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
	void setPlayback(uint64_t current, uint64_t checked, QString state, bool unavailable);
	void setVerifiedDownloads(std::set<uint64_t> documents);
	void cancelInteraction();
	void focusSelected();

	void setViewportLimit(int height);
	void setSelectionActions(Fn<void(uint64_t)> toggle, Fn<bool()> playSelection);
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
