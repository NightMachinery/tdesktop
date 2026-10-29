#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <functional>
#include <optional>

namespace Purple {

inline constexpr auto kControlLineBytes = 64 * 1024;
inline constexpr auto kControlClients = 16;
inline constexpr auto kControlClientPending = 8;
inline constexpr auto kControlPending = 64;

struct ControlRequest {
	qint64 id = 0;
	QString command;
	QJsonObject arguments;
	QJsonObject argumentTokens;
	std::function<bool()> active;
};

enum class ControlMediaKind { Unspecified, Empty, Music, Voice };

struct ControlResult {
	QJsonObject value;
	QString code;
	QString message;
	ControlMediaKind mediaKind = ControlMediaKind::Unspecified;
};

struct ControlParseResult {
	std::optional<ControlRequest> request;
	QJsonValue id = QJsonValue::Null;
	ControlResult error;
};

using ControlReply = std::function<void(ControlResult)>;

[[nodiscard]] std::optional<qint64> ControlIntegerArgument(
	const ControlRequest &request,
	const QString &name);
[[nodiscard]] std::optional<quint64> ControlUnsignedArgument(
	const ControlRequest &request,
	const QString &name);
[[nodiscard]] ControlParseResult ParseControlRequest(const QByteArray &line);
[[nodiscard]] ControlResult ControlError(QString code, QString message);
[[nodiscard]] QByteArray EncodeControlReply(QJsonValue id, ControlResult result);

class ControlRateLimit final {
public:
	[[nodiscard]] bool take(qint64 monotonicMilliseconds);

private:
	double _tokens = 20.;
	qint64 _updated = -1;

};

}
