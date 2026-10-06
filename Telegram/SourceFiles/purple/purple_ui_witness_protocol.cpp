#include "purple/purple_ui_witness_protocol.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <limits>

namespace Purple::UiWitness {
namespace {

class JsonScan final {
public:
	explicit JsonScan(QByteArrayView bytes);
	[[nodiscard]] bool complete();

private:
	void spaces();
	[[nodiscard]] bool value(int depth);
	[[nodiscard]] std::optional<QByteArrayView> string();
	[[nodiscard]] bool object(int depth);
	[[nodiscard]] bool array(int depth);
	[[nodiscard]] bool number();

	QByteArrayView _bytes;
	int _offset = 0;
	int _nodes = 0;

};

JsonScan::JsonScan(QByteArrayView bytes) : _bytes(bytes) {
}

void JsonScan::spaces() {
	while (_offset < _bytes.size()
		&& (_bytes[_offset] == ' ' || _bytes[_offset] == '\t'
			|| _bytes[_offset] == '\r' || _bytes[_offset] == '\n')) {
		++_offset;
	}
}

std::optional<QByteArrayView> JsonScan::string() {
	spaces();
	if (_offset >= _bytes.size() || _bytes[_offset++] != '"') {
		return std::nullopt;
	}
	const auto start = _offset;
	while (_offset < _bytes.size() && _bytes[_offset] != '"') {
		const auto ch = uchar(_bytes[_offset]);
		if (ch < 32 || ch > 126 || ch == '\\' || _offset - start >= 128) {
			return std::nullopt;
		}
		++_offset;
	}
	if (_offset >= _bytes.size()) {
		return std::nullopt;
	}
	const auto result = _bytes.sliced(start, _offset - start);
	++_offset;
	return result;
}

bool JsonScan::number() {
	const auto start = _offset;
	if (_offset < _bytes.size() && _bytes[_offset] == '-') {
		++_offset;
	}
	if (_offset >= _bytes.size() || _bytes[_offset] < '0'
		|| _bytes[_offset] > '9') {
		return false;
	}
	const auto zero = _bytes[_offset] == '0';
	++_offset;
	while (_offset < _bytes.size() && _bytes[_offset] >= '0'
		&& _bytes[_offset] <= '9') {
		if (zero || _offset - start >= 9) {
			return false;
		}
		++_offset;
	}
	return true;
}

bool JsonScan::object(int depth) {
	++_offset;
	spaces();
	if (_offset < _bytes.size() && _bytes[_offset] == '}') {
		++_offset;
		return true;
	}
	auto keys = std::array<QByteArrayView, 16>();
	auto count = 0;
	while (count != keys.size()) {
		const auto key = string();
		if (!key || std::find(keys.begin(), keys.begin() + count, *key)
			!= keys.begin() + count) {
			return false;
		}
		keys[count++] = *key;
		spaces();
		if (_offset >= _bytes.size() || _bytes[_offset++] != ':'
			|| !value(depth + 1)) {
			return false;
		}
		spaces();
		if (_offset < _bytes.size() && _bytes[_offset] == '}') {
			++_offset;
			return true;
		}
		if (_offset >= _bytes.size() || _bytes[_offset++] != ',') {
			return false;
		}
	}
	return false;
}

bool JsonScan::array(int depth) {
	++_offset;
	spaces();
	if (_offset < _bytes.size() && _bytes[_offset] == ']') {
		++_offset;
		return true;
	}
	for (auto i = 0; i != kCellLimit; ++i) {
		if (!value(depth + 1)) {
			return false;
		}
		spaces();
		if (_offset < _bytes.size() && _bytes[_offset] == ']') {
			++_offset;
			return true;
		}
		if (_offset >= _bytes.size() || _bytes[_offset++] != ',') {
			return false;
		}
	}
	return false;
}

bool JsonScan::value(int depth) {
	spaces();
	if (depth > 6 || ++_nodes > 128 || _offset >= _bytes.size()) {
		return false;
	}
	switch (_bytes[_offset]) {
	case '{': return object(depth);
	case '[': return array(depth);
	case '"': return string().has_value();
	default: return number();
	}
}

bool JsonScan::complete() {
	const auto valid = value(0);
	spaces();
	return valid && _offset == _bytes.size();
}

std::optional<int> Integer(const QJsonObject &o, const QString &key, int low, int high) {
	const auto v = o.value(key);
	if (!v.isDouble()) {
		return std::nullopt;
	}
	const auto n = v.toDouble();
	if (n < low || n > high || n != int(n)) {
		return std::nullopt;
	}
	return int(n);
}

std::optional<qint64> EpochMilliseconds(const QString &value) {
	if (value.size() != 13 || value.front() < u'1' || value.front() > u'9') {
		return std::nullopt;
	}
	for (const auto ch : value) {
		if (ch < u'0' || ch > u'9') {
			return std::nullopt;
		}
	}
	auto ok = false;
	const auto result = value.toLongLong(&ok, 10);
	return ok && QString::number(result) == value
		? std::optional<qint64>(result)
		: std::nullopt;
}

}

bool IsToken(QByteArrayView value) {
	if (value.size() != 64) {
		return false;
	}
	for (const auto ch : value) {
		if ((ch < 'a' || ch > 'f') && (ch < '0' || ch > '9')) {
			return false;
		}
	}
	return true;
}

std::optional<Request> DecodeRequest(QByteArrayView data) {
	if (data.empty() || data.size() > kRequestLimit || !JsonScan(data).complete()) {
		return std::nullopt;
	}
	auto error = QJsonParseError();
	const auto doc = QJsonDocument::fromJson(data.toByteArray(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) {
		return std::nullopt;
	}
	const auto o = doc.object();
	const auto schema = Integer(o, u"schema"_q, kSchema, kSchema);
	const auto op = Integer(o, u"op"_q, 1, 4);
	if (!schema || !op) {
		return std::nullopt;
	}
	if (*op == int(Operation::LoadedProxyMode)) {
		const auto keys = QStringList{
			u"schema"_q, u"op"_q, u"run"_q, u"nonce"_q, u"peer"_q,
			u"requestSequence"_q, u"requestedAt"_q,
		};
		if (o.size() != keys.size()) {
			return std::nullopt;
		}
		for (const auto &key : keys) {
			if (!o.contains(key)) {
				return std::nullopt;
			}
		}
		const auto sequence = Integer(
			o, u"requestSequence"_q, 1, kProxyModeMaxRequests);
		const auto requestedAtValue = o.value(u"requestedAt"_q);
		const auto requestedAt = requestedAtValue.isString()
			? EpochMilliseconds(requestedAtValue.toString())
			: std::nullopt;
		if (!sequence || !requestedAt) {
			return std::nullopt;
		}
		for (const auto &key : { u"run"_q, u"nonce"_q, u"peer"_q }) {
			if (!o.value(key).isString()
				|| !IsToken(o.value(key).toString().toLatin1())) {
				return std::nullopt;
			}
		}
		auto result = Request{ .operation = Operation::LoadedProxyMode };
		result.run = o.value(u"run"_q).toString().toLatin1();
		result.nonce = o.value(u"nonce"_q).toString().toLatin1();
		result.peer = o.value(u"peer"_q).toString().toLatin1();
		result.requestSequence = *sequence;
		result.requestedAt = *requestedAt;
		return result;
	}
	const auto keys = QStringList{
		u"schema"_q, u"op"_q, u"facet"_q, u"kind"_q, u"slot"_q, u"duration"_q,
		u"run"_q, u"acquisition"_q, u"burst"_q, u"nonce"_q, u"receiver"_q, u"receipt"_q,
	};
	if (o.size() != keys.size()) {
		return std::nullopt;
	}
	for (const auto &key : keys) {
		if (!o.contains(key)) {
			return std::nullopt;
		}
	}
	const auto facet = Integer(o, u"facet"_q, 1, 2);
	const auto kind = Integer(o, u"kind"_q, 1, 2);
	const auto slot = Integer(o, u"slot"_q, 0, 1);
	const auto duration = Integer(o, u"duration"_q, 1, kOperationMs);
	const auto legacyOperation = Integer(o, u"op"_q, 1, 3);
	if (!legacyOperation || !facet || !kind || !slot || !duration) {
		return std::nullopt;
	}
	auto result = Request{
		.operation = Operation(*legacyOperation), .facet = Facet(*facet),
		.kind = ReceiverKind(*kind), .slot = *slot, .duration = *duration,
	};
	for (const auto &key : keys.mid(6)) {
		if (!o.value(key).isString() || !IsToken(o.value(key).toString().toLatin1())) {
			return std::nullopt;
		}
	}
	result.run = o.value(u"run"_q).toString().toLatin1();
	result.acquisition = o.value(u"acquisition"_q).toString().toLatin1();
	result.burst = o.value(u"burst"_q).toString().toLatin1();
	result.nonce = o.value(u"nonce"_q).toString().toLatin1();
	result.receiver = o.value(u"receiver"_q).toString().toLatin1();
	result.receipt = o.value(u"receipt"_q).toString().toLatin1();
	return result;
}

QJsonObject RequestHeader(const Request &r) {
	return {{u"run"_q, QString::fromLatin1(r.run)},
		{u"acquisition"_q, QString::fromLatin1(r.acquisition)},
		{u"burst"_q, QString::fromLatin1(r.burst)},
		{u"nonce"_q, QString::fromLatin1(r.nonce)}};
}

QByteArray Frame(const QJsonObject &object) {
	const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
	if (body.size() > kReplyLimit) {
		return {};
	}
	auto result = QByteArray(4, '\0');
	for (auto i = 0; i != 4; ++i) {
		result[i] = char((quint32(body.size()) >> ((3 - i) * 8)) & 255);
	}
	result.append(body);
	return result;
}

QByteArray AuthoritySeal(const QJsonObject &authority) {
	return QCryptographicHash::hash(
		QByteArray("PURPLE_UI_WITNESS_1\n")
			+ QJsonDocument(authority).toJson(QJsonDocument::Compact),
		QCryptographicHash::Sha256).toHex();
}

QByteArray EqualityToken(QByteArrayView run, quint64 counter) {
	return QCryptographicHash::hash(
		QByteArray("PURPLE_UI_EQUALITY_1\n") + run.toByteArray()
			+ QByteArray::number(counter),
		QCryptographicHash::Sha256).toHex();
}

bool Reader::feed(QByteArrayView bytes) {
	if (_failed || bytes.empty() || bytes.size() > _bytes.size() - _used) {
		_failed = true;
		return false;
	}
	for (const auto ch : bytes) {
		_bytes[_used++] = ch;
		if (_used == 4) {
			auto size = quint32(0);
			for (auto i = 0; i != 4; ++i) {
				size = (size << 8) | uchar(_bytes[i]);
			}
			if (!size || size > kRequestLimit) {
				_failed = true;
				return false;
			}
			_expected = int(size) + 4;
		}
		if (_expected && _used > _expected) {
			_failed = true;
			return false;
		}
	}
	return true;
}

std::optional<Request> Reader::take() {
	if (_failed || !_expected || _used != _expected) {
		return std::nullopt;
	}
	const auto result = DecodeRequest(QByteArrayView(_bytes.data() + 4, _used - 4));
	_failed = !result;
	_used = _expected = 0;
	return result;
}

bool Reader::partial() const { return _used != 0; }
bool Reader::failed() const { return _failed; }

bool Lease::begin(const Request &r, qint64 now) {
	if (r.operation != Operation::Observe || r.duration < 1
		|| r.duration > kOperationMs || r.nonce == _original.nonce || now < 0
		|| now > std::numeric_limits<qint64>::max() - r.duration) {
		return false;
	}
	_original = r;
	_started = now;
	_deadline = now + r.duration;
	_authority = {};
	_active = true;
	_dirty = true;
	return true;
}

void Lease::record(QJsonObject authority) {
	_authority = std::move(authority);
	_dirty = _authority.empty();
}

bool Lease::validate(const Request &r, const QJsonObject &a, qint64 now) {
	if (!current(now) || r.operation != Operation::Validate
		|| r.run != _original.run || r.acquisition != _original.acquisition
		|| r.burst != _original.burst || r.nonce == _original.nonce
		|| r.duration != _original.duration
		|| r.kind != _original.kind || r.slot != _original.slot
		|| r.facet != _original.facet || r.receipt != seal()
		|| a != _authority || QString::fromLatin1(r.receiver)
			!= _authority.value(u"receiver"_q).toString()) {
		invalidate();
		return false;
	}
	return true;
}

void Lease::invalidate() { _dirty = true; }
void Lease::close() { _active = false; _dirty = true; _authority = {}; }
bool Lease::current(qint64 now) const { return _active && !_dirty && now >= _started && now < _deadline; }
qint64 Lease::deadline() const { return _deadline; }
const Request &Lease::original() const { return _original; }
const QJsonObject &Lease::authority() const { return _authority; }
QByteArray Lease::seal() const { return AuthoritySeal(_authority); }

bool Outbox::queue(QJsonObject value, qint64 deadline) {
	if (_failed || !_frame.isEmpty() || _invalidated || _sequence == std::numeric_limits<quint64>::max()) {
		_failed = true;
		return false;
	}
	value.insert(u"sequence"_q, QString::number(++_sequence, 16));
	_frame = Frame(value);
	_offset = 0;
	_deadline = deadline;
	_failed = _frame.isEmpty();
	return !_failed;
}

void Outbox::invalidate(unsigned receivers, quint64 epoch, qint64 deadline) {
	_invalidated |= receivers & 3;
	_epoch = epoch;
	_noticeDeadline = _noticeDeadline ? std::min(_noticeDeadline, deadline) : deadline;
}

QByteArrayView Outbox::pending(qint64 now) {
	if (_frame.isEmpty() && _invalidated) {
		const auto notice = QJsonObject{{u"schema"_q, kSchema},
			{u"type"_q, 3}, {u"receivers"_q, int(_invalidated)},
			{u"epoch"_q, QString::number(_epoch, 16)}};
		_invalidated = 0;
		const auto deadline = _noticeDeadline;
		_noticeDeadline = 0;
		if (!queue(notice, deadline)) {
			return {};
		}
	}
	if (_failed || (!_frame.isEmpty() && now >= _deadline)) {
		_failed = true;
		return {};
	}
	return QByteArrayView(_frame).sliced(_offset);
}

bool Outbox::consumed(int count, qint64 now) {
	if (_failed || now >= _deadline || count < 0
		|| count > _frame.size() - _offset) {
		_failed = true;
		return false;
	}
	_offset += count;
	if (_offset == _frame.size()) {
		_frame.clear();
		_offset = 0;
	}
	return true;
}

bool Outbox::empty() const { return _frame.isEmpty() && !_invalidated; }
bool Outbox::failed() const { return _failed; }
void Outbox::clear() { _frame.clear(); _invalidated = 0; _failed = true; }

}
