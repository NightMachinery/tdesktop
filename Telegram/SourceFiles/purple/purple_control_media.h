#pragma once

#include "purple/purple_control_protocol.h"

namespace Purple {

enum class ControlMediaType { Music, Voice };
enum class ControlMediaAction { Toggle, Play, Pause };
enum class ControlMediaState { Playing, Paused, Stopped };

struct ControlMediaSnapshot {
	std::optional<ControlMediaType> type;
	ControlMediaState state = ControlMediaState::Stopped;
	qint64 position = 0;
	qint64 duration = 0;
	QString name;
	QString path;
	QString mime;
	QString title;
	QString performer;
	QString sender;
	QString chat;
	qint64 date = 0;
	bool hidden = false;
};

struct ControlMediaAdapter {
	std::function<ControlResult(ControlMediaAction, std::optional<ControlMediaType>)> control;
	std::function<void(std::optional<ControlMediaType>, std::function<bool()>, ControlReply)> now;
	std::function<ControlResult()> lock;
};

[[nodiscard]] qint64 ControlMediaMilliseconds(qint64 samples, int frequency);
[[nodiscard]] ControlResult ControlMediaInfo(
	const ControlMediaSnapshot &snapshot,
	const QString &verifiedPath);
[[nodiscard]] ControlResult ControlLock(
	bool hasPasscode,
	const std::function<bool()> &lock);
[[nodiscard]] QString ExistingControlMediaPath(const QString &path);
[[nodiscard]] ControlMediaAdapter AppControlMediaAdapter();

}
