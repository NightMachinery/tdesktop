#pragma once

#include "base/basic_types.h"

#include <QByteArray>
#include <QJsonObject>
#include <array>
#include <optional>

namespace Purple::UiWitness {

inline constexpr auto kRequestLimit = 1024;
inline constexpr auto kReplyLimit = 16384;
inline constexpr auto kCellLimit = 8;
inline constexpr auto kOperationMs = 8000;
inline constexpr auto kValidationMs = 150;
inline constexpr auto kSessionMs = 2700000;
inline constexpr auto kSchema = 1;

enum class Operation { Observe = 1, Validate = 2, Close = 3 };
enum class Facet { Text = 1, Menu = 2 };
enum class ReceiverKind { Main = 1, Secondary = 2 };

struct Request {
	Operation operation = Operation::Close;
	Facet facet = Facet::Text;
	ReceiverKind kind = ReceiverKind::Main;
	int slot = 0;
	int duration = 0;
	QByteArray run;
	QByteArray acquisition;
	QByteArray burst;
	QByteArray nonce;
	QByteArray receiver;
	QByteArray receipt;
};

[[nodiscard]] bool IsToken(QByteArrayView value);
[[nodiscard]] std::optional<Request> DecodeRequest(QByteArrayView data);
[[nodiscard]] QByteArray Frame(const QJsonObject &object);
[[nodiscard]] QByteArray AuthoritySeal(const QJsonObject &authority);
[[nodiscard]] QByteArray EqualityToken(QByteArrayView run, quint64 counter);
[[nodiscard]] QJsonObject RequestHeader(const Request &request);

class Reader final {
public:
	[[nodiscard]] bool feed(QByteArrayView bytes);
	[[nodiscard]] std::optional<Request> take();
	[[nodiscard]] bool partial() const;
	[[nodiscard]] bool failed() const;

private:
	std::array<char, kRequestLimit + 4> _bytes = {};
	int _used = 0;
	int _expected = 0;
	bool _failed = false;

};

class Lease final {
public:
	[[nodiscard]] bool begin(const Request &request, qint64 now);
	[[nodiscard]] bool validate(
		const Request &request,
		const QJsonObject &current,
		qint64 now);
	void record(QJsonObject authority);
	void invalidate();
	void close();
	[[nodiscard]] bool current(qint64 now) const;
	[[nodiscard]] qint64 deadline() const;
	[[nodiscard]] const Request &original() const;
	[[nodiscard]] const QJsonObject &authority() const;
	[[nodiscard]] QByteArray seal() const;

private:
	Request _original;
	QJsonObject _authority;
	qint64 _deadline = 0;
	qint64 _started = 0;
	bool _active = false;
	bool _dirty = true;

};

class Outbox final {
public:
	[[nodiscard]] bool queue(QJsonObject value, qint64 deadline);
	void invalidate(unsigned receivers, quint64 epoch, qint64 deadline);
	[[nodiscard]] QByteArrayView pending(qint64 now);
	[[nodiscard]] bool consumed(int count, qint64 now);
	[[nodiscard]] bool empty() const;
	[[nodiscard]] bool failed() const;
	void clear();

private:
	QByteArray _frame;
	int _offset = 0;
	qint64 _deadline = 0;
	quint64 _sequence = 0;
	unsigned _invalidated = 0;
	quint64 _epoch = 0;
	qint64 _noticeDeadline = 0;
	bool _failed = false;

};

}
