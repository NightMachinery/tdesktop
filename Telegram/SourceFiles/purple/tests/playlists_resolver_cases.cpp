#include "purple/purple_playlists_resolve_queue.h"

#include <QtCore/QCoreApplication>

#include <iostream>
#include <tuple>

namespace {

using namespace Purple;
int Checks = 0;
int Failures = 0;

void Expect(bool condition, const char *message) {
	++Checks;
	if (!condition) {
		++Failures;
		std::cerr << message << '\n';
	}
}

using Key = std::tuple<int, uint64_t, int32_t>;

Key For(const PlaylistSourceRef &source) {
	return { int(source.peer.kind), source.peer.id, source.msg };
}

PlaylistResolveSong Song(uint64_t documentId = 7, int32_t messageId = 1) {
	return { documentId, { .peer = { PeerTokenKind::Self, 0 }, .msg = messageId, .since = 1 } };
}

struct Fixture {
	std::map<Key, PlaylistSourceRead> cache;
	std::map<Key, PlaylistSourceRead> server;
	std::deque<std::function<void()>> deferred;
	std::vector<std::vector<PlaylistSourceRef>> batches;
	std::optional<std::vector<PlaylistSongResolution>> result;
	std::unique_ptr<PlaylistResolveQueue> queue;
	int completions = 0;
	bool shortReply = false;
	bool duplicateReply = false;

	void start(std::vector<PlaylistResolveSong> songs, uint64_t selfUserId = 101) {
		queue = std::make_unique<PlaylistResolveQueue>(std::move(songs), selfUserId,
			PlaylistResolverIo{
				.cached = [&](const auto &ref) -> std::optional<PlaylistSourceRead> {
					const auto i = cache.find(For(ref));
					return i == cache.end() ? std::nullopt : std::optional(i->second);
				},
				.read = [&](auto refs, auto done) {
					Expect(refs.size() <= 100, "server batches capped at 100 sources");
					batches.push_back(refs);
					deferred.push_back([=, this] {
						auto reads = std::vector<PlaylistSourceRead>();
						for (const auto &ref : refs) {
							const auto i = server.find(For(ref));
							reads.push_back(i == server.end()
								? PlaylistSourceRead{ PlaylistSongStatus::Deleted } : i->second);
						}
						if (shortReply) reads.clear();
						done(reads);
						if (duplicateReply) done(reads);
					});
				},
				.defer = [&](auto next) { deferred.push_back(std::move(next)); },
			}, [&](auto values) { result = std::move(values); ++completions; });
		queue->start();
		queue->start();
	}

	void drain() {
		for (auto steps = 0; !deferred.empty() && steps != 1000; ++steps) {
			auto next = std::move(deferred.front());
			deferred.pop_front();
			next();
		}
		Expect(deferred.empty(), "resolution reaches finite idle state");
	}
};

void Exercise() {
	const auto channel = PlaylistSourceRef{ { PeerTokenKind::Channel, 50 }, 1, u"hint"_q };
	Expect(PlaylistResolvedChannelMatches(channel, { PeerTokenKind::Channel, 50 }, 1),
		"username lookup requires exact channel identity and access hash");
	Expect(!PlaylistResolvedChannelMatches(channel, { PeerTokenKind::Channel, 51 }, 1),
		"reassigned username cannot redirect a stored source");
	Expect(!PlaylistResolvedChannelMatches(channel, { PeerTokenKind::User, 50 }, 1),
		"username returning a user cannot satisfy channel source");
	Expect(!PlaylistResolvedChannelMatches(channel, { PeerTokenKind::Channel, 50 }, 0),
		"channel without access hash cannot be requested");
	{
		Fixture f;
		f.cache[For({ { PeerTokenKind::Self, 0 }, 1 })] = { PlaylistSongStatus::Ready, 7 };
		f.start({ Song() });
		f.drain();
		Expect(f.result && f.result->front().status == PlaylistSongStatus::Ready,
			"cached source resolves without server");
		Expect(f.batches.empty() && f.completions == 1, "local-first and start idempotence");
	}
	for (const auto failure : { PlaylistSongStatus::Deleted, PlaylistSongStatus::NoAccess,
		PlaylistSongStatus::Changed, PlaylistSongStatus::Restricted }) {
		Fixture f;
		auto song = Song();
		song.source.alt = { { { PeerTokenKind::Channel, 50 }, 2, u"channel_hint"_q } };
		f.server[For({ song.source.peer, song.source.msg })] = { failure };
		f.server[For(song.source.alt.front())] = { PlaylistSongStatus::Ready, 7 };
		const auto original = song.source;
		f.start({ song });
		f.drain();
		Expect(f.result && f.result->front().status == PlaylistSongStatus::EarlierCopy,
			"definitive source failure tries fallback");
		Expect(f.result->front().sourceIndex == 1 && f.result->front().source == song.source.alt.front(),
			"result identifies exact fallback without relinking");
		Expect(song.source == original && f.completions == 1, "resolution does not change stored source");
	}
	{
		Fixture f;
		auto song = Song();
		song.source.alt = { { { PeerTokenKind::Self, 0 }, 2 } };
		f.server[For({ song.source.peer, song.source.msg })] = { PlaylistSongStatus::Failed };
		f.server[For(song.source.alt.front())] = { PlaylistSongStatus::Ready, 7 };
		f.start({ song });
		f.drain();
		Expect(f.result->front().status == PlaylistSongStatus::Failed && f.batches.size() == 1,
			"network failure neither retries nor selects fallback");
	}
	{
		Fixture f;
		auto song = Song();
		song.source.alt = { { { PeerTokenKind::Self, 0 }, 2 }, { { PeerTokenKind::Self, 0 }, 3 } };
		f.cache[For({ song.source.peer, song.source.msg })] = { PlaylistSongStatus::Ready, 99 };
		f.server[For(song.source.alt[0])] = { PlaylistSongStatus::Ready, 98 };
		f.server[For(song.source.alt[1])] = { PlaylistSongStatus::Ready, 7 };
		f.start({ song });
		f.drain();
		Expect(f.result->front().status == PlaylistSongStatus::EarlierCopy
			&& f.result->front().sourceIndex == 2, "every source must match document identity");
		Expect(f.batches.size() == 2 && f.batches[0][0].msg == 2 && f.batches[1][0].msg == 3,
			"fallback source order preserved");
	}
	for (const auto cached : { false, true }) {
		Fixture f;
		auto songs = std::vector<PlaylistResolveSong>();
		for (auto i = 1; i != 252; ++i) {
			songs.push_back(Song(uint64_t(i), i));
			(cached ? f.cache : f.server)[For({ { PeerTokenKind::Self, 0 }, i })]
				= { PlaylistSongStatus::Ready, uint64_t(i) };
		}
		f.start(songs);
		Expect(!f.result, "large local or remote resolution yields between bounded chunks");
		f.drain();
		Expect(f.result && f.result->size() == songs.size(), "all requested rows retained");
		for (auto i = std::size_t(0); i != songs.size(); ++i) {
			Expect((*f.result)[i].documentId == songs[i].documentId
				&& (*f.result)[i].status == PlaylistSongStatus::Ready, "result order and identity stable");
		}
		Expect(f.batches.size() == (cached ? 0 : 3), "remote reads coalesced into batches");
	}
	{
		Fixture f;
		auto invalid = Song();
		invalid.source.alt = { { invalid.source.peer, invalid.source.msg } };
		f.start({ Song(0), Song(7, 0), invalid });
		f.drain();
		Expect(f.result && f.result->size() == 3 && f.batches.empty(), "invalid records never issue reads");
		for (const auto &r : *f.result) Expect(r.status == PlaylistSongStatus::InvalidInput, "invalid row explicit");
	}
	{
		Fixture f;
		f.start({ Song() }, 0);
		f.drain();
		Expect(f.result->front().status == PlaylistSongStatus::InvalidInput, "account identity required");
	}
	{
		Fixture f;
		f.shortReply = true;
		f.start({ Song() });
		f.drain();
		Expect(f.result->front().status == PlaylistSongStatus::Failed, "short server result cannot match wrong row");
	}
	{
		Fixture f;
		f.duplicateReply = true;
		f.server[For({ { PeerTokenKind::Self, 0 }, 1 })] = { PlaylistSongStatus::Ready, 7 };
		f.start({ Song() });
		f.drain();
		Expect(f.completions == 1 && f.result->front().status == PlaylistSongStatus::Ready,
			"duplicate read completion ignored");
	}
	{
		Fixture f;
		f.start({ Song() });
		f.queue.reset();
		f.drain();
		Expect(!f.result && f.completions == 0, "destroyed resolver drops pending reads");
	}
	{
		Fixture f;
		f.start({});
		f.drain();
		Expect(f.result && f.result->empty() && f.completions == 1, "empty request completes once");
	}
}

} // namespace

int main(int argc, char *argv[]) {
	QCoreApplication application(argc, argv);
	Exercise();
	std::cout << "PLAYLIST_RESOLVER_CHECKS=" << Checks << " FAILURES=" << Failures << '\n';
	return Failures ? 1 : 0;
}
