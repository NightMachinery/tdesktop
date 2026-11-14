#include "purple/hooks/music_listening.h"

#include "base/algorithm.h"

#include <crl/crl_time.h>
#include <rpl/event_stream.h>

#include <QtCore/QCoreApplication>

#include <iostream>
#include <vector>

namespace {

rpl::event_stream<bool> PrivacyChanges;
bool PrivacyEnabled = true;
crl::time Now = 0;

} // namespace

namespace Purple {

rpl::producer<bool> PrivateMusicListeningValue() {
	return rpl::single(PrivacyEnabled) | rpl::then(PrivacyChanges.events());
}

} // namespace Purple

using FullMsgId = int;

namespace Data {
struct FileOriginMessage { explicit FileOriginMessage(FullMsgId) {} };
struct FileOrigin { explicit FileOrigin(FileOriginMessage) {} };
} // namespace Data

namespace MTP {
struct Error {
	int code() const { return 400; }
	QString type() const { return u"FILE_REFERENCE_EXPIRED"_q; }
};
} // namespace MTP

struct ListenReport { int document = 0; int duration = 0; };
ListenReport MTPmessages_ReportMusicListen(int document, int duration) {
	return { document, duration };
}
int MTP_int(int value) { return value; }

struct Api {
	struct Request {
		Api *api = nullptr;
		ListenReport report;
		Fn<void(const MTP::Error&)> failure;
		Request &fail(Fn<void(const MTP::Error&)> callback) {
			failure = std::move(callback);
			return *this;
		}
		void send() {
			api->sent.push_back(report);
			api->failures.push_back(failure);
		}
	};
	std::vector<ListenReport> sent;
	std::vector<Fn<void(const MTP::Error&)>> failures;
	std::vector<Fn<void(const QByteArray&)>> refreshes;
	Request request(ListenReport report) { return { this, report, {} }; }
	void refreshFileReference(
			Data::FileOrigin,
			Fn<void(const QByteArray&)> callback) {
		refreshes.push_back(std::move(callback));
	}
};

struct Session {
	Api requests;
	Api &api() { return requests; }
};

class DocumentData {
public:
	Session owner;
	QByteArray reference = "before";
	bool song = true;
	bool isSong() const { return song; }
	QByteArray fileReference() const { return reference; }
	int mtpInput() const { return 1; }
	Session &session() { return owner; }
};

namespace Media::Player {

enum class State { Playing, Paused, Stopped };
bool IsStopped(State state) { return state == State::Stopped; }
bool IsPausedOrPausing(State state) { return state == State::Paused; }
crl::time ClockNow() { return Now; }

struct AudioId {
	DocumentData *document = nullptr;
	DocumentData *audio() const { return document; }
	FullMsgId contextId() const { return 1; }
};

struct TrackState {
	AudioId id;
	State state = State::Playing;
};

struct Timer {
	explicit Timer(Fn<void()> callback) : callback(std::move(callback)) {}
	Fn<void()> callback;
	void cancel() {}
	void callOnce(crl::time) {}
};

class MusicListenTracker {
public:
	MusicListenTracker();
	void update(const TrackState &state);
	void finalize();
	void report();
	void pauseTimedOut();

private:
	DocumentData *_document = nullptr;
	FullMsgId _contextId = 0;
	crl::time _listenedMs = 0;
	crl::time _playStartedAt = 0;
	bool _playing = false;
	Timer _pauseTimer;
	Purple::MusicListeningPrivacy _privacy;
};

} // namespace Media::Player

#include "music-listening-tracker.inc"

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::cerr << name << '\n';
		}
	};
	const auto privacy = [](bool value) {
		PrivacyEnabled = value;
		PrivacyChanges.fire_copy(value);
	};
	using namespace Media::Player;
	{
		DocumentData song;
		MusicListenTracker tracker;
		tracker.update({ { &song }, State::Playing });
		Now += 20000;
		tracker.finalize();
		expect(song.owner.requests.sent.empty(), "default private listening sends nothing");
		tracker.update({ { &song }, State::Playing });
		Now += 20000;
		privacy(false);
		tracker.update({ { &song }, State::Playing });
		Now += 5000;
		tracker.finalize();
		expect(song.owner.requests.sent.size() == 1, "opting out permits reports");
		expect(song.owner.requests.sent.back().duration == 5, "private time is never carried into opt-out report");
		tracker.update({ { &song }, State::Playing });
		Now += 9000;
		privacy(true);
		expect(song.owner.requests.sent.size() == 1, "turning privacy on discards pending interval");
		Now += 9000;
		privacy(false);
		tracker.update({ { &song }, State::Playing });
		Now += 4000;
		tracker.update({ { &song }, State::Stopped });
		expect(song.owner.requests.sent.back().duration == 4, "later opt-out starts fresh");
		tracker.update({ { &song }, State::Playing });
		Now += 5000;
		tracker.update({ { &song }, State::Paused });
		privacy(true);
		privacy(false);
		tracker.pauseTimedOut();
		expect(song.owner.requests.sent.size() == 2, "paused private interval cannot escape via timer");
		tracker.update({ { &song }, State::Playing });
		Now += 6000;
		tracker.finalize();
		auto &api = song.owner.requests;
		api.failures.back()(MTP::Error());
		expect(api.refreshes.size() == 1, "file-reference failure can wait for refresh");
		privacy(true);
		privacy(false);
		song.reference = "after";
		api.refreshes.back()(song.reference);
		expect(api.sent.size() == 3, "revoked report never retries after privacy was toggled");
		tracker.update({ { &song }, State::Playing });
		Now += 6000;
		tracker.finalize();
		api.failures.back()(MTP::Error());
		song.reference = "after-again";
		api.refreshes.back()(song.reference);
		expect(api.sent.size() == 5, "unrevoked report keeps ordinary file-reference retry");
		tracker.update({ { &song }, State::Playing });
		Now += 2000;
		tracker.finalize();
		expect(api.sent.size() == 5, "short intervals retain stock minimum duration");
	}
	{
		DocumentData song;
		{
			MusicListenTracker tracker;
			tracker.update({ { &song }, State::Playing });
			Now += 5000;
			tracker.finalize();
			song.owner.requests.failures.back()(MTP::Error());
		}
		song.reference = "after-destruction";
		song.owner.requests.refreshes.back()(song.reference);
		expect(song.owner.requests.sent.size() == 1, "destroyed tracker revokes pending retries");
	}
	{
		rpl::event_stream<bool> changes;
		auto discards = 0;
		Purple::MusicListeningPrivacy policy(
			rpl::single(false) | rpl::then(changes.events()),
			[&] { ++discards; });
		auto ticket = policy.reportAllowed();
		expect(ticket(), "opt-out ticket allowed");
		changes.fire_copy(false);
		expect(ticket() && discards == 1, "unchanged setting does not reset tracking");
		changes.fire_copy(true);
		expect(!ticket() && policy.suppressed(), "enabling privacy revokes ticket");
		changes.fire_copy(false);
		expect(!ticket() && policy.reportAllowed()(), "old ticket cannot revive after second transition");
	}
	std::cout << "MUSIC_LISTENING_CHECKS=" << checks << " FAILURES=" << failures << '\n';
	return failures ? 1 : 0;
}
