#pragma once

class QObject;

namespace Purple {

enum class HintRole {
	General,
	Navigate,
	Reply,
	Playback,
	PlayPause,
	Menu,
	Close,
	Destructive,
	Count,
};

void SetWindowHintRole(QObject *object, HintRole role);
[[nodiscard]] HintRole TaggedHintRole(const QObject *object, HintRole fallback);

} // namespace Purple
