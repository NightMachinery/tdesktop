#include "purple/purple_sync_diag.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <set>

namespace Purple {
namespace {

constexpr auto kMaxCount = int64_t(99999);
constexpr auto kMaxSeconds = int64_t(9999999);
const auto kInvalid = QStringLiteral("invalid");

const std::vector<QString> &Words() {
	static const auto result = std::vector<QString>{
		u"diag"_q, u"repeated"_q, u"area"_q, u"event"_q, u"count"_q,
		u"sync_chat"_q, u"lookup_start"_q, u"lookup_result"_q, u"request"_q,
		u"wait_honored"_q, u"auto_paused"_q, u"auto_resumed"_q,
		u"problem_shown"_q, u"migration"_q, u"trigger"_q, u"startup"_q,
		u"screen"_q, u"manual"_q, u"auto"_q, u"post"_q, u"lists"_q,
		u"main"_q, u"archive"_q, u"loaded"_q, u"partial"_q, u"requested"_q,
		u"result"_q, u"found"_q, u"created"_q, u"absent_waiting"_q,
		u"ineligible"_q, u"bound"_q, u"rebound"_q, u"reason"_q,
		u"method"_q, u"outcome"_q, u"ok"_q, u"flood"_q, u"network"_q,
		u"timeout"_q, u"error"_q, u"seconds"_q, u"code"_q, u"rpc"_q,
		u"channels.getfullchannel"_q,
		u"channels.createchannel"_q, u"folders.editpeerfolders"_q,
		u"account.updatenotifysettings"_q, u"messages.gethistory"_q,
		u"messages.getmessages"_q, u"channels.getmessages"_q,
		u"messages.sendmedia"_q, u"messages.editmessage"_q,
		u"messages.deletemessages"_q, u"channels.deletemessages"_q,
		u"upload.savefilepart"_q, u"upload.savebigfilepart"_q,
		u"upload.getfile"_q,
		u"chat_gone"_q, u"chat_left"_q, u"not_megagroup"_q,
		u"not_creator"_q, u"extra_member"_q, u"extra_admin"_q,
		u"public_username"_q, u"restricted"_q, u"cannot_post"_q,
		u"foreign_record"_q, u"forwarded_record"_q, u"via_bot_record"_q,
		u"create_limit"_q, u"create_uncertain"_q, u"magic_ineligible"_q,
		u"space_mismatch"_q, u"lists_loading"_q, u"binding_unreadable"_q,
		u"drop_pending"_q, u"move"_q, u"inventory"_q, u"publish"_q,
		u"joined"_q, u"wait"_q, u"problem"_q, u"step"_q,
		u"user_check"_q, u"user_resume"_q, u"chat_changed"_q,
		u"user_action"_q, u"choose"_q, u"default"_q, u"join"_q,
		u"listed"_q, u"value"_q, u"on"_q, u"off"_q,
		u"action"_q, u"slot"_q, u"lists_unreadable"_q, u"native_unavailable"_q,
		u"rejected"_q, u"wait_recorded"_q, u"retry"_q, u"request_refused"_q,
		u"review_refused"_q, u"unreadable"_q, u"ambiguous"_q, u"newer_schema"_q,
		u"heads_unclear"_q, u"own_records"_q, u"unknown"_q,
		u"first_use"_q, u"ask"_q, u"join_silently"_q, u"choice"_q, u"synced"_q,
		u"this_device"_q, u"none"_q, u"carry"_q, u"apply"_q, u"mark"_q,
	};
	return result;
}

const std::set<QString> &WordSet() {
	static const auto result = std::set<QString>(Words().begin(), Words().end());
	return result;
}

QString Clamped(int64_t value, int64_t limit) {
	return (value < 0)
		? u"neg"_q
		: (value > limit)
		? QString::number(limit) + '+'
		: QString::number(value);
}

bool Ended(int64_t start, int64_t now) {
	return now < start || now - start >= SyncDiagRepeats::kWindow;
}

std::mutex &GlobalMutex() {
	static auto result = std::mutex();
	return result;
}

SyncDiagLog::Sink &GlobalSink() {
	static auto result = SyncDiagLog::Sink();
	return result;
}

SyncDiagLog &Global() {
	static auto result = SyncDiagLog([](const QString &line) {
		auto sink = SyncDiagLog::Sink();
		{
			const auto lock = std::lock_guard(GlobalMutex());
			sink = GlobalSink();
		}
		if (sink) {
			sink(line);
		}
	}, [] {
		return int64_t(QDateTime::currentMSecsSinceEpoch());
	});
	return result;
}

} // namespace

const std::vector<QString> &SyncDiagVocabulary() {
	return Words();
}

QString SyncDiagToken(const QString &value) {
	return WordSet().contains(value) ? value : kInvalid;
}

bool SyncDiagWordShape(const QString &value) {
	if (value.isEmpty() || value.size() > 48) {
		return false;
	} else if (value[0] < 'a' || value[0] > 'z') {
		return false;
	}
	auto digits = 0;
	for (const auto ch : value) {
		const auto digit = (ch >= '0' && ch <= '9');
		if (!(ch >= 'a' && ch <= 'z') && !digit && ch != '_' && ch != '.') {
			return false;
		}
		digits = digit ? (digits + 1) : 0;
		if (digits >= 3) {
			return false;
		}
	}
	return true;
}

SyncDiagField SyncDiagCode(const QString &key, const QString &value) {
	return { SyncDiagToken(key), SyncDiagToken(value) };
}

SyncDiagField SyncDiagReason(const QString &code) {
	return { u"reason"_q, SyncDiagToken(code) };
}

SyncDiagField SyncDiagCount(const QString &key, int64_t value) {
	return { SyncDiagToken(key), Clamped(value, kMaxCount) };
}

SyncDiagField SyncDiagSeconds(int64_t value) {
	return { u"seconds"_q, Clamped(value, kMaxSeconds) };
}

SyncDiagField SyncDiagFlag(const QString &key, bool value) {
	return { SyncDiagToken(key), value ? u"1"_q : u"0"_q };
}

QString SyncDiagLine(
		const QString &area,
		const QString &event,
		const std::vector<SyncDiagField> &fields) {
	auto result = SyncDiagToken(area) + ' ' + SyncDiagToken(event);
	for (const auto &field : fields) {
		result += ' ' + field.key + '=' + field.value;
	}
	return (result.size() > kSyncDiagLineLimit)
		? result.left(kSyncDiagLineLimit)
		: result;
}

QString SyncDiagSummaryLine(const SyncDiagRepeats::Summary &summary) {
	return SyncDiagLine(u"diag"_q, u"repeated"_q, {
		SyncDiagCode(u"area"_q, summary.area),
		SyncDiagCode(u"event"_q, summary.event),
		SyncDiagCount(u"count"_q, summary.dropped),
	});
}

SyncDiagRepeats::Decision SyncDiagRepeats::offer(
		int64_t now,
		const QString &body,
		const QString &area,
		const QString &event) {
	auto result = Decision();
	const auto summarize = [&](Window &window) {
		if (window.dropped > 0) {
			result.summaries.push_back({
				window.area,
				window.event,
				window.dropped,
			});
		}
		window.dropped = 0;
	};
	for (auto &window : _windows) {
		if (window.dropped > 0 && Ended(window.start, now)) {
			summarize(window);
		}
	}
	auto i = _index.find(body);
	if (i == end(_index)) {
		_windows.push_front({ body, area, event, now, 1, 0 });
		_index.emplace(body, _windows.begin());
		result.write = true;
	} else {
		_windows.splice(_windows.begin(), _windows, i->second);
		auto &window = _windows.front();
		if (Ended(window.start, now)) {
			window.start = now;
			window.written = 1;
			result.write = true;
		} else if (window.written < kBurst) {
			++window.written;
			result.write = true;
		} else {
			++window.dropped;
		}
	}
	while (_windows.size() > size_t(kKeys)) {
		summarize(_windows.back());
		_index.erase(_windows.back().body);
		_windows.pop_back();
	}
	return result;
}

std::vector<SyncDiagRepeats::Summary> SyncDiagRepeats::flush() {
	auto result = std::vector<Summary>();
	for (auto i = _windows.rbegin(); i != _windows.rend(); ++i) {
		if (i->dropped > 0) {
			result.push_back({ i->area, i->event, i->dropped });
		}
		i->dropped = 0;
	}
	return result;
}

SyncDiagLog::SyncDiagLog(Sink sink, Clock clock)
: _sink(std::move(sink))
, _clock(std::move(clock)) {
}

void SyncDiagLog::emit(const std::vector<SyncDiagRepeats::Summary> &list) {
	for (const auto &summary : list) {
		_sink(SyncDiagSummaryLine(summary));
	}
}

void SyncDiagLog::write(
		const QString &area,
		const QString &event,
		const std::vector<SyncDiagField> &fields) {
	const auto line = SyncDiagLine(area, event, fields);
	const auto lock = std::lock_guard(_mutex);
	const auto decision = _repeats.offer(
		_clock(),
		line,
		SyncDiagToken(area),
		SyncDiagToken(event));
	emit(decision.summaries);
	if (decision.write) {
		_sink(line);
	}
}

void SyncDiagLog::flush() {
	const auto lock = std::lock_guard(_mutex);
	emit(_repeats.flush());
}

void SetSyncDiagSink(SyncDiagLog::Sink sink) {
	const auto lock = std::lock_guard(GlobalMutex());
	GlobalSink() = std::move(sink);
}

void SyncDiag(
		const QString &area,
		const QString &event,
		const std::vector<SyncDiagField> &fields) {
	Global().write(area, event, fields);
}

} // namespace Purple
