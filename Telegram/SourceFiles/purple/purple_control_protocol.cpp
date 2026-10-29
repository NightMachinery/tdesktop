#include "purple/purple_control_protocol.h"

#include "base/basic_types.h"
#include <QtCore/QJsonDocument>
#include <algorithm>

namespace Purple {

namespace {

QByteArray QuoteJsonNumbers(const QByteArray &line) {
	auto result = QByteArray();
	result.reserve(line.size() * 2);
	auto quoted = false;
	auto escaped = false;
	for (auto i = qsizetype(0); i < line.size(); ++i) {
		const auto ch = line[i];
		if (quoted) {
			result += ch;
			if (escaped) escaped = false;
			else if (ch == '\\') escaped = true;
			else if (ch == '"') quoted = false;
		} else if (ch == '"') {
			quoted = true;
			result += ch;
		} else if (ch == '-' || (ch >= '0' && ch <= '9')) {
			result += '"';
			for (; i < line.size(); ++i) {
				const auto digit = line[i];
				if ((digit < '0' || digit > '9') && digit != '-'
					&& digit != '+' && digit != '.' && digit != 'e' && digit != 'E') {
					break;
				}
				result += digit;
			}
			result += '"';
			--i;
		} else {
			result += ch;
		}
	}
	return result;
}

std::optional<qint64> ExactInteger(
		const QJsonValue &value,
		const QJsonValue &raw) {
	if (!value.isDouble() || !raw.isString()) return {};
	const auto token = raw.toString().toLatin1();
	const auto negative = token.startsWith('-');
	auto exponentAt = token.indexOf('e');
	if (exponentAt < 0) exponentAt = token.indexOf('E');
	const auto end = (exponentAt < 0) ? token.size() : exponentAt;
	auto digits = token.mid(negative, end - negative);
	const auto point = digits.indexOf('.');
	auto scale = (point < 0) ? qsizetype(0) : point + 1 - digits.size();
	if (point >= 0) digits.remove(point, 1);
	auto first = qsizetype(0);
	while (first < digits.size() && digits[first] == '0') ++first;
	digits.remove(0, first);
	if (digits.isEmpty()) return qint64(0);
	if (exponentAt >= 0) {
		auto i = exponentAt + 1;
		const auto subtract = token[i] == '-';
		if (token[i] == '-' || token[i] == '+') ++i;
		auto exponent = 0;
		for (; i < token.size(); ++i) {
			exponent = std::min(
				2 * kControlLineBytes,
				exponent * 10 + token[i] - '0');
		}
		scale += subtract ? -exponent : exponent;
	}
	while (scale < 0 && digits.endsWith('0')) {
		digits.chop(1);
		++scale;
	}
	if (scale < 0 || digits.size() + scale > 19) return {};
	digits.append(QByteArray(scale, '0'));
	if (negative) digits.prepend('-');
	auto valid = false;
	const auto integer = digits.toLongLong(&valid, 10);
	return valid ? std::make_optional(integer) : std::nullopt;
}

}

std::optional<qint64> ControlIntegerArgument(
		const ControlRequest &request,
		const QString &name) {
	return ExactInteger(
		request.arguments.value(name),
		request.argumentTokens.value(name));
}

ControlResult ControlError(QString code, QString message) {
	return { {}, std::move(code), std::move(message) };
}

ControlParseResult ParseControlRequest(const QByteArray &line) {
	auto result = ControlParseResult();
	result.error = ControlError(u"bad-request"_q, u"Expected a UTF-8 JSON request object."_q);
	if (line.size() > kControlLineBytes || QString::fromUtf8(line).toUtf8() != line) return result;
	const auto document = QJsonDocument::fromJson(line);
	if (!document.isObject()) return result;
	const auto object = document.object();
	const auto tokenDocument = QJsonDocument::fromJson(QuoteJsonNumbers(line));
	const auto tokens = tokenDocument.object();
	const auto id = ExactInteger(object.value(u"id"_q), tokens.value(u"id"_q));
	if (id) result.id = *id;
	const auto version = ExactInteger(object.value(u"v"_q), tokens.value(u"v"_q));
	if (version && *version != 1) {
		result.error = ControlError(u"unsupported"_q, u"Only protocol version 1 is supported."_q);
		return result;
	}
	const auto command = object.value(u"cmd"_q);
	const auto arguments = object.value(u"args"_q);
	if (!version || !id || !command.isString() || command.toString().isEmpty()
		|| command.toString().size() > 96 || !arguments.isObject()) return result;
	result.request = ControlRequest{
		*id, command.toString(), arguments.toObject(), tokens.value(u"args"_q).toObject(),
	};
	result.error = {};
	return result;
}

QByteArray EncodeControlReply(QJsonValue id, ControlResult result) {
	auto object = QJsonObject{
		{ u"v"_q, 1 },
		{ u"id"_q, id },
		{ u"ok"_q, result.code.isEmpty() },
	};
	if (result.code.isEmpty()) {
		object.insert(u"result"_q, result.value);
	} else {
		object.insert(u"error"_q, QJsonObject{
			{ u"code"_q, result.code },
			{ u"message"_q, result.message },
		});
	}
	auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
	if (bytes.size() > kControlLineBytes) {
		return EncodeControlReply(id, ControlError(u"busy"_q, u"The result exceeds the response limit."_q));
	}
	return bytes + '\n';
}

bool ControlRateLimit::take(qint64 monotonicMilliseconds) {
	if (_updated < 0) _updated = monotonicMilliseconds;
	const auto elapsed = std::max(qint64(0), monotonicMilliseconds - _updated);
	_updated = std::max(_updated, monotonicMilliseconds);
	_tokens = std::min(20., _tokens + double(elapsed) / 100.);
	if (_tokens < 1.) return false;
	_tokens -= 1.;
	return true;
}

}
