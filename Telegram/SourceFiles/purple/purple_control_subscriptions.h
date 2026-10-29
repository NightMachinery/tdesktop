#pragma once

#include "purple/purple_control_commands.h"

namespace Purple {

enum class ControlTopic { Media, Lock };

using ControlEventHandler = std::function<void(
	ControlTopic, const ControlPolicy &, std::function<bool()>, ControlReply)>;

[[nodiscard]] QString ControlTopicName(ControlTopic topic);
[[nodiscard]] ControlResult AuthorizeControlTopic(ControlTopic topic, const ControlPolicy &policy);
[[nodiscard]] ControlResult AuthorizeControlEvent(ControlTopic topic, const ControlPolicy &policy);
[[nodiscard]] std::optional<std::vector<ControlTopic>> ParseControlTopics(const QJsonObject &arguments);
[[nodiscard]] QByteArray EncodeControlEvent(ControlTopic topic, ControlResult result, bool ended);

}
