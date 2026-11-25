#pragma once

class QWidget;
namespace Data { class Thread; }
namespace Window { class SessionController; }

namespace Purple {

enum class MusicTrackAction;
enum class TrackListAction;

class ChatMusicKeyTarget {
public:
	virtual ~ChatMusicKeyTarget() = default;

	virtual void navigate(TrackListAction action, int count) = 0;
	virtual void act(MusicTrackAction action) = 0;
	virtual void focusFilter() = 0;
};

[[nodiscard]] ChatMusicKeyTarget *FindChatMusicKeyTarget(
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> focus);

void ShowChatMusicBox(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread);

} // namespace Purple
