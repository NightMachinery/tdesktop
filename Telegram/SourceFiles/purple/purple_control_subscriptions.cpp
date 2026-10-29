#include "purple/purple_control_subscriptions.h"

#include "base/basic_types.h"
#include <QtCore/QJsonDocument>
#include <algorithm>

namespace Purple {

QString ControlTopicName(ControlTopic topic) {
	return topic == ControlTopic::Media ? u"media.state"_q : u"lock.state"_q;
}

ControlResult AuthorizeControlEvent(ControlTopic topic, const ControlPolicy &policy) {
	return AuthorizeControlCommand(
		topic == ControlTopic::Media ? u"media.now"_q : u"lock"_q,
		policy);
}

ControlResult AuthorizeControlTopic(ControlTopic topic, const ControlPolicy &policy) {
	auto result = AuthorizeControlEvent(topic, policy);
	return result.code == u"locked"_q ? ControlResult() : std::move(result);
}

std::optional<std::vector<ControlTopic>> ParseControlTopics(const QJsonObject &arguments) {
	const auto value = arguments.value(u"topics"_q);
	if (arguments.size() != 1 || !value.isArray()) return std::nullopt;
	const auto array = value.toArray();
	if (array.isEmpty() || array.size() > 2) return std::nullopt;
	auto result = std::vector<ControlTopic>();
	for (const auto &entry : array) {
		if (!entry.isString()) return std::nullopt;
		const auto name = entry.toString();
		if (name != u"media.state"_q && name != u"lock.state"_q) return std::nullopt;
		const auto topic = name == u"media.state"_q ? ControlTopic::Media : ControlTopic::Lock;
		if (std::find(result.begin(), result.end(), topic) != result.end()) return std::nullopt;
		result.push_back(topic);
	}
	return result;
}

QByteArray EncodeControlEvent(ControlTopic topic, ControlResult result, bool ended) {
	auto value = QJsonObject{
		{ u"v"_q, 1 }, { u"event"_q, ControlTopicName(topic) },
	};
	if (result.code.isEmpty()) value.insert(u"result"_q, result.value);
	else value.insert(u"error"_q, QJsonObject{
		{ u"code"_q, result.code }, { u"message"_q, result.message },
	});
	if (ended) value.insert(u"ended"_q, true);
	return QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
}

}
