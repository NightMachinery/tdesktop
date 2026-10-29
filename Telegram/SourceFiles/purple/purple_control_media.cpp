#include "purple/purple_control_media.h"

#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <algorithm>
#include <limits>

namespace Purple {

qint64 ControlMediaMilliseconds(qint64 samples, int frequency) {
	if (samples <= 0 || frequency <= 0) return 0;
	const auto seconds = samples / frequency;
	const auto tail = (samples % frequency) * 1000 / frequency;
	const auto maximum = std::numeric_limits<qint64>::max();
	return (seconds > (maximum - tail) / 1000) ? maximum : seconds * 1000 + tail;
}

ControlResult ControlMediaInfo(
		const ControlMediaSnapshot &snapshot,
		const QString &verifiedPath) {
	const auto path = (!verifiedPath.isEmpty() && snapshot.path == verifiedPath)
		? QJsonValue(verifiedPath.left(4096)) : QJsonValue(QJsonValue::Null);
	auto file = QJsonObject{
		{ u"name"_q, snapshot.name.left(1024) },
		{ u"path"_q, path },
	};
	if (snapshot.hidden) {
		return { QJsonObject{ { u"hidden"_q, true }, { u"file"_q, file } } };
	}
	file.insert(u"mime"_q, snapshot.mime.left(256));
	const auto state = snapshot.state == ControlMediaState::Playing ? u"playing"_q
		: snapshot.state == ControlMediaState::Paused ? u"paused"_q : u"stopped"_q;
	auto result = QJsonObject{
		{ u"type"_q, snapshot.type ? QJsonValue(*snapshot.type == ControlMediaType::Music
			? u"music"_q : u"voice"_q) : QJsonValue(QJsonValue::Null) },
		{ u"state"_q, snapshot.type ? state : u"stopped"_q },
		{ u"position_ms"_q, std::max(qint64(0), snapshot.position) },
		{ u"duration_ms"_q, std::max(qint64(0), snapshot.duration) },
		{ u"file"_q, file },
	};
	if (snapshot.type == ControlMediaType::Music) {
		result.insert(u"music"_q, QJsonObject{
			{ u"title"_q, snapshot.title.left(1024) },
			{ u"performer"_q, snapshot.performer.left(1024) },
		});
	} else if (snapshot.type == ControlMediaType::Voice) {
		result.insert(u"voice"_q, QJsonObject{
			{ u"sender"_q, snapshot.sender.left(1024) },
			{ u"chat"_q, snapshot.chat.left(1024) },
			{ u"date"_q, snapshot.date },
		});
	}
	return { result };
}

ControlResult ControlLock(bool hasPasscode, const std::function<bool()> &lock) {
	if (!hasPasscode) {
		return ControlError(u"no-passcode"_q, u"Set a local passcode in Purple Settings first."_q);
	}
	if (!lock || !lock()) {
		return ControlError(u"busy"_q, u"Purple could not lock; retry."_q);
	}
	return { QJsonObject{ { u"locked"_q, true } } };
}

QString ExistingControlMediaPath(const QString &path) {
	const auto app = QCoreApplication::instance();
	if ((app && QThread::currentThread() == app->thread()) || path.size() > 4096
		|| !QDir::isAbsolutePath(path)) return {};
	const auto info = QFileInfo(path);
	return (info.isFile() && !info.isSymLink()) ? path : QString();
}

}
