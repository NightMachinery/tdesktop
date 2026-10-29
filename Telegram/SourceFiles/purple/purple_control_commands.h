#pragma once

#include "purple/purple_control_protocol.h"
#include "purple/purple_control_media.h"
#include <QtCore/QJsonArray>
#include <array>
#include <string_view>
#include <vector>

namespace Purple {

inline constexpr auto kControlGroupCount = 5;

enum class ControlLockUnit { Window, MediaControl, MusicInfo, VoiceInfo, Emoji };
inline constexpr auto kControlLockUnitCount = 5;
inline constexpr auto kControlLockDefaults = std::array{ false, true, true, false, true };
inline constexpr auto kControlLockKeys = std::array<std::string_view, kControlLockUnitCount>{
	"automation.locked.window", "automation.locked.media.control",
	"automation.locked.media.info.music", "automation.locked.media.info.voice",
	"automation.locked.emoji",
};

struct ControlPolicy {
	quint64 generation = 0;
	bool enabled = false;
	bool locked = false;
	std::array<bool, kControlGroupCount> groups = { true, true, true, true, true };
	std::array<bool, kControlLockUnitCount> allowLocked = kControlLockDefaults;
};

struct ControlWindow {
	QString title;
	bool primary = false;
	bool active = false;
};

struct ControlWindowAdapter {
	std::function<std::vector<ControlWindow>()> list;
	std::function<bool(int)> activate;
};

using ControlEmojiAdapter = std::function<void(
	quint64, int, std::function<bool()>, ControlReply)>;

class ControlConsent final {
public:
	[[nodiscard]] quint64 begin();
	[[nodiscard]] bool advance(quint64 token);
	[[nodiscard]] bool confirm(quint64 token);
	void cancel(quint64 token);
	void invalidate();

private:
	quint64 _token = 0;
	int _stage = 0;

};

[[nodiscard]] QString ControlGroupName(int index);
[[nodiscard]] QJsonArray ControlCommandNames();
[[nodiscard]] bool ControlAllowedWhileLocked(
	const ControlPolicy &policy,
	ControlLockUnit unit);
[[nodiscard]] ControlResult AuthorizeControlCommand(
	const QString &command,
	const ControlPolicy &policy,
	ControlMediaKind mediaKind = ControlMediaKind::Unspecified,
	bool final = false);
void DispatchControlRequest(
	const ControlRequest &request,
	const ControlPolicy &policy,
	const QString &build,
	const ControlWindowAdapter &windows,
	ControlReply reply,
	const ControlEmojiAdapter &emoji = {},
	const ControlMediaAdapter &media = {});

}
