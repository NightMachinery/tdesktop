#pragma once

#include "purple/purple_voice_queue_rules.h"
#include "base/weak_ptr.h"
#include "base/random.h"
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace Ports {
using MsgId = std::int64_t;
using PeerId = std::uint64_t;
struct FullMsgId {
	PeerId peer = 0;
	MsgId msg = 0;
	explicit operator bool() const { return peer || msg; }
	bool operator==(const FullMsgId &) const = default;
};
namespace Main {
struct Session : base::has_weak_ptr {};
} // namespace Main
struct PeerData {
	PeerId id = 1;
	bool self = false, user = false, replies = false, codes = false, forum = false,
		 mono = false;
	bool isSelf() const { return self; }
	bool isUser() const { return user; }
	bool isRepliesChat() const { return replies; }
	bool isVerifyCodes() const { return codes; }
	bool isForum() const { return forum; }
	bool amMonoforumAdmin() const { return mono; }
};
struct Owner;
struct History {
	Owner *o = nullptr;
	Main::Session *s = nullptr;
	PeerData *peer = nullptr;
	History *normalized = nullptr;
	Owner &owner() const { return *o; }
	Main::Session &session() const { return *s; }
	History *migrateToOrMe() { return normalized ? normalized : this; }
};
struct DocumentData;
struct ItemMedia {
	DocumentData *doc = nullptr;
	int ttl = 0;
	int ttlSeconds() const { return ttl; }
	DocumentData *document() const { return doc; }
};
struct HistoryItem {
	History *h = nullptr;
	FullMsgId id;
	bool outgoing = false, unread = true;
	MsgId topic = 0;
	PeerId mono = 0;
	ItemMedia *m = nullptr;
	ItemMedia *media() const { return m; }
	FullMsgId fullId() const { return id; }
	History *history() { return h; }
	bool out() const { return outgoing; }
	bool isUnreadMedia() const { return unread; }
	MsgId topicRootId() const { return topic; }
	PeerId sublistPeerId() const { return mono; }
};
struct DocumentData;
struct Shared {};
inline std::function<void()> OnIntent, OnShared, OnPause, OnRead, OnUpdated, OnFinished,
	OnTimer;
inline bool TimerEnds = false;
inline void fire(std::function<void()> &port) {
	auto callback = std::exchange(port, {});
	if (callback)
		callback();
}
struct StreamPort {
	bool available = true;
	int requests = 0;
	std::shared_ptr<Shared> sharedDocument(DocumentData *, FullMsgId) {
		++requests;
		fire(OnShared);
		return available ? std::make_shared<Shared>() : nullptr;
	}
};
struct Owner {
	Main::Session *s = nullptr;
	std::vector<HistoryItem *> items;
	StreamPort streamingPort;
	int reads = 0;
	HistoryItem *message(FullMsgId id) {
		for (auto item : items)
			if (item->id == id)
				return item;
		return nullptr;
	}
	StreamPort &streaming() { return streamingPort; }
	void markMediaRead(DocumentData *) {
		++reads;
		fire(OnRead);
	}
};
struct DocumentData {
	Owner *o = nullptr;
	bool voice = true, round = false, music = false;
	Owner &owner() const { return *o; }
	Main::Session &session() const { return *o->s; }
	bool isAudioFile() const { return music; }
	bool isVoiceMessage() const { return voice; }
	bool isVideoMessage() const { return round; }
};
class AudioMsgId {
  public:
	enum class Type { Unknown, Voice, Song, Video };
	AudioMsgId() = default;
	AudioMsgId(DocumentData *d, FullMsgId id) : d(d), id(id) {}
	Type type() const { return !d ? Type::Unknown : d->music ? Type::Song : Type::Voice; }
	DocumentData *audio() const { return d; }
	FullMsgId contextId() const { return id; }
	explicit operator bool() const { return d; }
	bool operator==(const AudioMsgId &) const = default;
	DocumentData *d = nullptr;
	FullMsgId id;
};
namespace Media::Player {
struct PlaylistContext {
	MsgId topicRootId = 0;
	PeerId monoforumPeerId = 0;
};
enum class State { Playing, Paused, Stopped, Pausing, StoppedAtEnd };
inline bool IsStopped(State state) {
	return state == State::Stopped || state == State::StoppedAtEnd;
}
inline bool IsPaused(State state) { return state == State::Paused; }
struct TrackState {
	AudioMsgId id;
	State state = State::Playing;
};
inline bool IsPausedOrPausing(State state) {
	return IsPaused(state) || state == State::Pausing;
}
inline bool IsStoppedOrStopping(State state) { return IsStopped(state); }
inline std::function<TrackState(AudioMsgId::Type)> GetState;
struct PlayerPort {
	TrackState getState(AudioMsgId::Type type) {
		return GetState ? GetState(type) : TrackState{{}, State::Stopped};
	}
};
inline PlayerPort Player;
inline PlayerPort *instance() { return &Player; }
} // namespace Media::Player
namespace Core {
struct Calls {
	bool currentCall() { return false; }
	bool currentGroupCall() { return false; }
};
struct AppPort {
	void floatPlayerToggleGifsPaused(bool) {}
	Calls &calls() {
		static Calls calls;
		return calls;
	}
};
inline AppPort &App() {
	static AppPort app;
	return app;
}
} // namespace Core
struct Config {
	Purple::Playback playback;
};
} // namespace Ports
