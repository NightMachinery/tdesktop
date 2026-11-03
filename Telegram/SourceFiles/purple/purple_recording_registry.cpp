/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_registry.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <map>
#include <utility>
#include <variant>

namespace Purple::RecordingRegistry {
namespace {

constexpr auto kKinds = std::array{
	std::pair{ Kind::Voice, "voice" },
	std::pair{ Kind::Round, "round" },
};

constexpr auto kStates = std::array{
	std::pair{ State::Recording, "recording" },
	std::pair{ State::Sending, "sending" },
	std::pair{ State::Sent, "sent" },
	std::pair{ State::Unsent, "unsent" },
	std::pair{ State::Discarded, "discarded" },
	std::pair{ State::Recovered, "recovered" },
	std::pair{ State::Unrecoverable, "unrecoverable" },
};

constexpr auto kMaxDepth = 4;
constexpr auto kMaxEntries = 4096;

void Quote(std::string &out, const std::string &value) {
	out += '"';
	for (const auto c : value) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (byte < 0x20) {
			static constexpr auto kHex = "0123456789abcdef";
			out += "\\u00";
			out += kHex[byte >> 4];
			out += kHex[byte & 0x0F];
		} else {
			out += c;
		}
	}
	out += '"';
}

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
	std::variant<std::monostate, bool, std::int64_t, std::uint64_t, std::string, Array, Object> data;
};

class Parser final {
public:
	explicit Parser(const std::string &text) : _text(text) {
	}

	[[nodiscard]] std::optional<Value> document() {
		auto result = value(0);
		skip();
		if (!result || _at != _text.size()) {
			return std::nullopt;
		}
		return result;
	}

private:
	void skip() {
		while (_at < _text.size()
			&& std::strchr(" \t\r\n", _text[_at])) {
			++_at;
		}
	}

	[[nodiscard]] bool take(char c) {
		skip();
		if (_at < _text.size() && _text[_at] == c) {
			++_at;
			return true;
		}
		return false;
	}

	[[nodiscard]] std::optional<std::string> string() {
		if (!take('"')) {
			return std::nullopt;
		}
		auto result = std::string();
		while (_at < _text.size()) {
			const auto c = _text[_at++];
			if (c == '"') {
				return result;
			} else if (c != '\\') {
				result += c;
				continue;
			} else if (_at >= _text.size()) {
				return std::nullopt;
			}
			const auto escaped = _text[_at++];
			if (escaped == '"' || escaped == '\\' || escaped == '/') {
				result += escaped;
			} else if (escaped == 'u' && _at + 4 <= _text.size()) {
				auto code = 0;
				const auto begin = _text.data() + _at;
				const auto parsed = std::from_chars(begin, begin + 4, code, 16);
				if (parsed.ptr != begin + 4 || code > 0x7F) {
					return std::nullopt;
				}
				result += char(code);
				_at += 4;
			} else {
				return std::nullopt;
			}
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<Value> number() {
		const auto begin = _text.data() + _at;
		const auto end = _text.data() + _text.size();
		if (begin < end && *begin == '-') {
			auto result = std::int64_t();
			const auto parsed = std::from_chars(begin, end, result);
			if (parsed.ec != std::errc()) {
				return std::nullopt;
			}
			_at += parsed.ptr - begin;
			return Value{ result };
		}
		auto result = std::uint64_t();
		const auto parsed = std::from_chars(begin, end, result);
		if (parsed.ec != std::errc()) {
			return std::nullopt;
		}
		_at += parsed.ptr - begin;
		return Value{ result };
	}

	[[nodiscard]] std::optional<Value> value(int depth) {
		skip();
		if (_at >= _text.size() || depth > kMaxDepth) {
			return std::nullopt;
		}
		const auto c = _text[_at];
		if (c == '"') {
			auto result = string();
			return result ? std::optional(Value{ std::move(*result) }) : std::nullopt;
		} else if (c == '{') {
			++_at;
			auto result = Object();
			if (take('}')) {
				return Value{ std::move(result) };
			}
			do {
				skip();
				auto key = string();
				if (!key || !take(':')) {
					return std::nullopt;
				}
				auto item = value(depth + 1);
				if (!item) {
					return std::nullopt;
				}
				result[std::move(*key)] = std::move(*item);
			} while (take(','));
			return take('}') ? std::optional(Value{ std::move(result) }) : std::nullopt;
		} else if (c == '[') {
			++_at;
			auto result = Array();
			if (take(']')) {
				return Value{ std::move(result) };
			}
			do {
				auto item = value(depth + 1);
				if (!item) {
					return std::nullopt;
				} else if (result.size() < kMaxEntries) {
					result.push_back(std::move(*item));
				}
			} while (take(','));
			return take(']') ? std::optional(Value{ std::move(result) }) : std::nullopt;
		} else if (_text.compare(_at, 4, "true") == 0) {
			_at += 4;
			return Value{ true };
		} else if (_text.compare(_at, 5, "false") == 0) {
			_at += 5;
			return Value{ false };
		}
		return number();
	}

	const std::string &_text;
	std::size_t _at = 0;

};

[[nodiscard]] const Value *Field(const Object &object, const char *name) {
	const auto i = object.find(name);
	return (i == object.end()) ? nullptr : &i->second;
}

[[nodiscard]] std::optional<std::int64_t> Signed(const Value *value) {
	if (!value) {
		return std::int64_t(0);
	} else if (const auto v = std::get_if<std::int64_t>(&value->data)) {
		return *v;
	} else if (const auto u = std::get_if<std::uint64_t>(&value->data)) {
		return (*u <= std::uint64_t(INT64_MAX))
			? std::optional(std::int64_t(*u))
			: std::nullopt;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> Unsigned(const Value *value) {
	if (!value) {
		return std::uint64_t(0);
	} else if (const auto u = std::get_if<std::uint64_t>(&value->data)) {
		return *u;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<bool> Bool(const Value *value) {
	if (!value) {
		return false;
	} else if (const auto b = std::get_if<bool>(&value->data)) {
		return *b;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> String(const Value *value) {
	if (!value) {
		return std::string();
	} else if (const auto s = std::get_if<std::string>(&value->data)) {
		return *s;
	}
	return std::nullopt;
}

template <typename Enum, std::size_t Size>
[[nodiscard]] std::optional<Enum> Named(
		const std::array<std::pair<Enum, const char*>, Size> &names,
		const std::string &value) {
	for (const auto &[e, name] : names) {
		if (value == name) {
			return e;
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<Entry> ReadEntry(const Value &value) {
	const auto object = std::get_if<Object>(&value.data);
	if (!object) {
		return std::nullopt;
	}
	auto result = Entry();
	const auto id = String(Field(*object, "id"));
	const auto account = Signed(Field(*object, "account"));
	const auto user = Unsigned(Field(*object, "user"));
	const auto peer = Unsigned(Field(*object, "peer"));
	const auto topic = Signed(Field(*object, "topic"));
	const auto reply = Signed(Field(*object, "reply"));
	const auto kind = String(Field(*object, "kind"));
	const auto started = Signed(Field(*object, "started"));
	const auto duration = Signed(Field(*object, "duration_ms"));
	const auto binned = Signed(Field(*object, "binned"));
	const auto viewOnce = Bool(Field(*object, "view_once"));
	const auto partial = Bool(Field(*object, "partial"));
	const auto fromListen = Bool(Field(*object, "from_listen"));
	const auto media = String(Field(*object, "media"));
	const auto journal = String(Field(*object, "journal"));
	const auto state = String(Field(*object, "state"));
	if (!id || id->empty() || !account || !user || !peer || !topic || !reply
		|| !kind || !started || !duration || !binned || !viewOnce
		|| !partial || !fromListen || !media || !journal || !state
		|| !ValidFileName(*media)
		|| (!journal->empty() && !ValidFileName(*journal))
		|| *account < -1 || *account > 1000) {
		return std::nullopt;
	}
	const auto kindValue = Named(kKinds, *kind);
	const auto stateValue = Named(kStates, *state);
	if (!kindValue || !stateValue) {
		return std::nullopt;
	}
	result.id = *id;
	result.account = int(*account);
	result.user = *user;
	result.peer = *peer;
	result.topic = *topic;
	result.reply = *reply;
	result.kind = *kindValue;
	result.started = *started;
	result.durationMs = *duration;
	result.binned = *binned;
	result.viewOnce = *viewOnce;
	result.partial = *partial;
	result.fromListen = *fromListen;
	result.media = *media;
	result.journal = *journal;
	result.state = *stateValue;
	return result;
}

} // namespace

Entry *Manifest::find(const std::string &id) {
	const auto i = std::find_if(entries.begin(), entries.end(), [&](
			const Entry &entry) {
		return entry.id == id;
	});
	return (i == entries.end()) ? nullptr : &*i;
}

const Entry *Manifest::find(const std::string &id) const {
	return const_cast<Manifest*>(this)->find(id);
}

bool Manifest::remove(const std::string &id) {
	const auto before = entries.size();
	entries.erase(std::remove_if(entries.begin(), entries.end(), [&](
			const Entry &entry) {
		return entry.id == id;
	}), entries.end());
	return entries.size() != before;
}

const char *KindName(Kind kind) {
	for (const auto &[value, name] : kKinds) {
		if (value == kind) {
			return name;
		}
	}
	return "voice";
}

const char *StateName(State state) {
	for (const auto &[value, name] : kStates) {
		if (value == state) {
			return name;
		}
	}
	return "recording";
}

bool ValidFileName(const std::string &name) {
	return !name.empty()
		&& name.size() <= 64
		&& name.front() != '.'
		&& std::all_of(name.begin(), name.end(), [](char c) {
			return (c >= 'a' && c <= 'z')
				|| (c >= '0' && c <= '9')
				|| c == '.'
				|| c == '-';
		});
}

std::string Encode(const Manifest &manifest) {
	auto out = std::string("{\"version\":");
	out += std::to_string(kVersion);
	out += ",\"entries\":[";
	auto first = true;
	for (const auto &entry : manifest.entries) {
		if (!std::exchange(first, false)) {
			out += ',';
		}
		const auto number = [&](const char *name, auto value) {
			out += '"';
			out += name;
			out += "\":";
			out += std::to_string(value);
			out += ',';
		};
		const auto text = [&](const char *name, const std::string &value) {
			out += '"';
			out += name;
			out += "\":";
			Quote(out, value);
			out += ',';
		};
		const auto flag = [&](const char *name, bool value) {
			out += '"';
			out += name;
			out += "\":";
			out += value ? "true" : "false";
			out += ',';
		};
		out += '{';
		text("id", entry.id);
		number("account", entry.account);
		number("user", entry.user);
		number("peer", entry.peer);
		number("topic", entry.topic);
		number("reply", entry.reply);
		text("kind", KindName(entry.kind));
		number("started", entry.started);
		number("duration_ms", entry.durationMs);
		number("binned", entry.binned);
		flag("view_once", entry.viewOnce);
		flag("partial", entry.partial);
		flag("from_listen", entry.fromListen);
		text("media", entry.media);
		text("journal", entry.journal);
		out += "\"state\":";
		Quote(out, StateName(entry.state));
		out += '}';
	}
	out += "]}\n";
	return out;
}

Manifest Decode(const std::string &text, ReadStatus *status) {
	const auto set = [&](ReadStatus value) {
		if (status) {
			*status = value;
		}
		return Manifest();
	};
	if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
		return set(ReadStatus::Empty);
	}
	const auto document = Parser(text).document();
	const auto object = document
		? std::get_if<Object>(&document->data)
		: nullptr;
	if (!object) {
		return set(ReadStatus::Damaged);
	}
	const auto version = Signed(Field(*object, "version"));
	if (!version || *version != kVersion) {
		return set(version ? ReadStatus::UnknownVersion : ReadStatus::Damaged);
	}
	const auto entries = Field(*object, "entries");
	const auto list = entries ? std::get_if<Array>(&entries->data) : nullptr;
	if (!list) {
		return set(ReadStatus::Damaged);
	}
	auto result = Manifest();
	auto damaged = false;
	for (const auto &item : *list) {
		if (auto entry = ReadEntry(item); entry && !result.find(entry->id)) {
			result.entries.push_back(std::move(*entry));
		} else {
			damaged = true;
		}
	}
	if (status) {
		*status = damaged ? ReadStatus::Damaged : ReadStatus::Ok;
	}
	return result;
}

std::optional<StopKind> StopKindFromOrdinal(int ordinal) {
	switch (ordinal) {
	case 0: return StopKind::Cancel;
	case 1: return StopKind::Send;
	case 2: return StopKind::Listen;
	}
	return std::nullopt;
}

StopDecision DecideStop(StopKind kind, bool keepDiscarded) {
	switch (kind) {
	case StopKind::Send: return { .keepFiles = true, .state = State::Sending };
	case StopKind::Listen: return { .keepFiles = true, .state = State::Recording };
	case StopKind::Cancel: break;
	}
	return keepDiscarded
		? StopDecision{ .keepFiles = true, .state = State::Discarded }
		: StopDecision{ .keepFiles = false, .state = State::Discarded };
}

bool InBin(State state) {
	return state == State::Unsent
		|| state == State::Discarded
		|| state == State::Recovered
		|| state == State::Unrecoverable;
}

bool NeedsRecovery(State state) {
	return state == State::Recording || state == State::Sending;
}

Mark MarkOf(const Entry &entry) {
	switch (entry.state) {
	case State::Recovered:
		return entry.partial ? Mark::Partial : Mark::Recovered;
	case State::Unsent: return Mark::NotSent;
	case State::Discarded: return Mark::Discarded;
	case State::Unrecoverable: return Mark::Unrecoverable;
	case State::Recording:
	case State::Sending:
	case State::Sent: break;
	}
	return Mark::None;
}

std::optional<std::size_t> MatchSent(
		const Manifest &manifest,
		const SentFacts &facts) {
	auto result = std::optional<std::size_t>();
	for (auto i = std::size_t(); i != manifest.entries.size(); ++i) {
		const auto &entry = manifest.entries[i];
		const auto waiting = (entry.state == State::Sending)
			|| (entry.state == State::Discarded
				&& entry.fromListen
				&& entry.binned <= facts.now
				&& facts.now - entry.binned <= kListenSendWindowSeconds);
		if (!waiting
			|| entry.account != facts.account
			|| entry.peer != facts.peer
			|| entry.kind != facts.kind) {
			continue;
		} else if (!result
			|| entry.started < manifest.entries[*result].started) {
			result = i;
		}
	}
	return result;
}

std::vector<std::string> Expired(
		const Manifest &manifest,
		const std::function<bool(std::int64_t binned)> &expired) {
	auto result = std::vector<std::string>();
	for (const auto &entry : manifest.entries) {
		if (entry.state == State::Sent
			|| (InBin(entry.state) && expired && expired(entry.binned))) {
			result.push_back(entry.id);
		}
	}
	return result;
}

std::vector<std::string> OfAccount(const Manifest &manifest, int account) {
	auto result = std::vector<std::string>();
	for (const auto &entry : manifest.entries) {
		if (account >= 0 && entry.account == account) {
			result.push_back(entry.id);
		}
	}
	return result;
}

std::vector<std::string> AdoptOrphans(
		Manifest &manifest,
		const std::vector<std::string> &names,
		std::int64_t now) {
	auto result = std::vector<std::string>();
	const auto has = [&](const std::string &name) {
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	const auto named = [&](const std::string &name) {
		return std::any_of(
			manifest.entries.begin(),
			manifest.entries.end(),
			[&](const Entry &entry) {
				return entry.media == name || entry.journal == name;
			});
	};
	for (const auto &name : names) {
		const auto dot = name.find('.');
		const auto suffix = (dot == std::string::npos)
			? std::string()
			: name.substr(dot);
		const auto round = (suffix == ".mp4.prc");
		if ((!round && suffix != ".ogg.prc")
			|| !ValidFileName(name)
			|| named(name)) {
			continue;
		}
		const auto id = name.substr(0, dot);
		if (id.empty() || manifest.find(id)) {
			continue;
		}
		const auto journal = id + ".pjr.prc";
		manifest.entries.push_back({
			.id = id,
			.kind = round ? Kind::Round : Kind::Voice,
			.started = now,
			.binned = now,
			.media = name,
			.journal = (round && has(journal)) ? journal : std::string(),
			.state = State::Recording,
		});
		result.push_back(id);
	}
	return result;
}

std::vector<const Entry*> BinNewestFirst(const Manifest &manifest) {
	auto result = std::vector<const Entry*>();
	for (const auto &entry : manifest.entries) {
		if (InBin(entry.state)) {
			result.push_back(&entry);
		}
	}
	std::stable_sort(result.begin(), result.end(), [](
			const Entry *a,
			const Entry *b) {
		return a->started > b->started;
	});
	return result;
}

} // namespace Purple::RecordingRegistry
