/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>

class PeerData;

namespace Ui {
class RpWidget;
} // namespace Ui

// Purple: what a spent budget does in the chat pane. A hard budget puts this
// over the history and the composer; a soft one says so once and gets out of
// the way. Neither touches messages or notifications - the cap is a screen and
// not a mute, and the session keeps running behind it, counted as reading, so
// time spent sitting on the cover still shows in the total.
namespace Purple {

class ScreenTimeCover final {
public:
	explicit ScreenTimeCover(not_null<Ui::RpWidget*> parent);
	~ScreenTimeCover();

	// The chat the pane is showing, or null for none. Cheap to call on every
	// chat change; the ledger is only re-derived when the answer could differ.
	void setPeer(PeerData *peer);

	// Where the cover goes: everything below the top bar, so the chat stays
	// nameable and closeable while its time is up.
	void setGeometry(QRect geometry);

private:
	class Widget;

	std::unique_ptr<Widget> _widget;

};

} // namespace Purple
