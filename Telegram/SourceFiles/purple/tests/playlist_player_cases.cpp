#include "purple/purple_playlists_resolve_queue.h"
#include "media/media_common.h"
#include "purple/hooks/playlist_player.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <numeric>

namespace Purple::PlayerProbe {

#include "playlist-player-queue.inc"

} // namespace Purple::PlayerProbe

int main() {
	using namespace Purple;
	using namespace Purple::PlayerProbe;
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::cerr << name << '\n';
		}
	};
	{
		auto cursor = PlaylistCursor();
		cursor.reset(4, 0);
		auto played = std::vector<std::size_t>();
		const auto sources = std::vector<PlaylistSongStatus>{
			PlaylistSongStatus::Deleted,
			PlaylistSongStatus::EarlierCopy,
			PlaylistSongStatus::Failed,
			PlaylistSongStatus::Ready,
		};
		auto retry = false;
		while (const auto index = cursor.index()) {
			const auto action = ActionForResolution(sources[*index]);
			if (action == ResolveAction::Skip) {
				cursor.skip();
			} else if (action == ResolveAction::Retry) {
				retry = true;
				break;
			} else {
				played.push_back(*index);
				cursor.select(cursor.position() + 1, 1);
			}
		}
		expect(played == std::vector<std::size_t>{1},
			"deleted rows skip, earlier copies play in saved order");
		expect(retry && cursor.index() == std::optional<std::size_t>(2),
			"transient completion retains the exact row for explicit Retry");
		const auto old = cursor.admission();
		cursor.cancel();
		expect(!old(), "explicit Retry revokes the previous network completion");
		if (ActionForResolution(PlaylistSongStatus::Ready) == ResolveAction::Play) {
			played.push_back(*cursor.index());
		}
		expect(played == std::vector<std::size_t>{1, 2},
			"successful explicit Retry resumes the retained row without replaying an earlier song");
		cursor.select(3, 1);
		cursor.select(4, 1);
		expect(!cursor.index() && !cursor.stepAvailable(1),
			"end remains an exhausted playlist boundary, not a chat index");
		expect(cursor.stepAvailable(-1), "Previous from exhaustion reaches the last saved row");
		cursor.select(cursor.position() - 1, -1);
		expect(cursor.index() == std::optional<std::size_t>(3),
			"Previous enters the last row without wrapping");
		cursor.select(0, -1);
		cursor.skip();
		expect(!cursor.index() && cursor.stepAvailable(1),
			"unavailable first row exhausts backwards and can resume forwards");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.reset(3, 0);
		const auto pending = cursor.admission();
		cursor.select(2, 1);
		expect(!pending(), "an old selected-row callback cannot restart after a newer selection");
		const auto current = cursor.admission();
		expect(current(), "the selected generation is admitted");
		cursor.retire();
		expect(!current(), "account, route or external-song retirement revokes pending work");
		cursor.reset(3, 2);
		expect(!current(), "a replacement playlist cannot revive a retired generation");
	}
	{
		auto afterDestruction = Fn<bool()>();
		{
			auto cursor = PlaylistCursor();
			cursor.reset(1, 0);
			afterDestruction = cursor.admission();
			expect(afterDestruction(), "a live owner admits its pending callback");
		}
		expect(!afterDestruction(), "the callback remains safe after its queue owner is destroyed");
	}
	{
		expect(ResolveIndices(1000, 0, 1, 1000) == std::vector<std::size_t>{0, 1, 2, 3},
			"large snapshots resolve only a bounded forward window");
		expect(ResolveIndices(1000, 998, 1, 4) == std::vector<std::size_t>{998, 999},
			"lookahead reaches the end without wrapping into another queue");
		expect(ResolveIndices(1000, 2, -1, 4) == std::vector<std::size_t>{2, 1, 0},
			"Previous resolves backwards only within the saved snapshot");
		expect(ResolveIndices(1000, 4, 1, 1) == std::vector<std::size_t>{4},
			"selected playback does not wait for speculative lookahead");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.reset(0, 0);
		expect(!cursor.index() && !cursor.stepAvailable(-1) && !cursor.stepAvailable(1),
			"an empty snapshot has no implicit chat fallback or wrapped song");
		for (const auto status : { PlaylistSongStatus::NoAccess, PlaylistSongStatus::Changed,
			PlaylistSongStatus::Restricted, PlaylistSongStatus::InvalidInput }) {
			expect(ActionForResolution(status) == ResolveAction::Skip,
				"definitive unavailable sources do not start an automatic retry loop");
		}
		expect(ActionForResolution(PlaylistSongStatus::Resolving) == ResolveAction::Retry,
			"an incomplete result is not a playable document");
	}
	{
		const auto state = std::make_shared<PlaylistHandoffState>();
		const auto forced = state->begin(true);
		const auto asynchronous = state->guard(forced);
		expect(forced && asynchronous(), "an unowned async Song callback has a frame");
		state->queue([] {});
		expect(!asynchronous() && !state->take(),
			"a successor revokes an async end callback before timer consumption");
		state->finish(forced);
		state->supersede();
		const auto normal = state->begin();
		expect(!normal && state->guard(normal)(),
			"ordinary stock playback outside playlist handoff is unchanged");
		const auto owner = state->enter();
		const auto frame = state->begin();
		const auto outer = state->callback();
		auto stream = 0;
		state->queue([&] { stream = 2; });
		expect(!outer(), "a nested Song request revokes the outer stock continuation");
		expect(state->requestDispatch(), "the successor gets one deferred dispatch");
		expect(!state->take(), "a nested event pump cannot dispatch while the stock frame is active");
		if (outer()) stream = 1;
		state->finish(frame);
		expect(state->requestDispatch(), "leaving the frame rearms deferred delivery once");
		auto resume = state->take();
		state->leave(owner);
		if (resume) resume();
		expect(stream == 2, "only the successor installs a stream after old clear/play returns");
	}
	{
		const auto state = std::make_shared<PlaylistHandoffState>();
		const auto owner = state->enter();
		const auto frame = state->begin();
		auto played = std::vector<int>();
		state->queue([&] { played.push_back(2); });
		state->queue([&] { played.push_back(3); });
		state->finish(frame);
		auto resume = state->take();
		state->leave(owner);
		if (resume) resume();
		expect(played == std::vector<int>{3},
			"several reentrant Song requests keep only the newest successor");
	}
	{
		const auto state = std::make_shared<PlaylistHandoffState>();
		const auto oldOwner = state->enter();
		const auto oldFrame = state->begin();
		const auto old = state->guard(oldFrame);
		state->queue([] {});
		state->supersede();
		state->finish(oldFrame);
		expect(!state->take() && !old(),
			"a newer playlist intent removes an older queued stock successor");
		state->leave(oldOwner);
		const auto nextOwner = state->enter();
		const auto nextFrame = state->begin();
		const auto next = state->guard(nextFrame);
		state->cancel(oldOwner);
		expect(next(), "retiring an old owner cannot invalidate a newer stock call");
		state->cancel(nextOwner);
		expect(!next(), "current-owner retirement invalidates its own stock continuation");
		state->finish(nextFrame);
		state->leave(nextOwner);
	}
	{
		const auto state = std::make_shared<PlaylistHandoffState>();
		const auto owner = state->enter();
		const auto frame = state->begin();
		auto queued = false;
		state->queue([&] { queued = true; });
		state->finish(frame);
		state->leave(owner);
		const auto laterStock = state->begin();
		expect(!laterStock && !state->take() && !queued,
			"a later ordinary stock play supersedes a still-queued old handoff");
	}
	{
		auto calls = PlaylistInternalCalls();
		const auto adapter = calls.enter(PlaylistPlayerIntent::Play);
		expect(!calls.take(PlaylistPlayerIntent::Stop),
			"a pending internal Play cannot authorize an external Stop");
		expect(calls.take(PlaylistPlayerIntent::Play),
			"the exact adapter or Repeat One Play consumes one authorization");
		expect(!calls.take(PlaylistPlayerIntent::Play),
			"a synchronous subscriber cannot inherit the internal Play authorization");
		const auto stop = calls.enter(PlaylistPlayerIntent::Stop);
		expect(calls.take(PlaylistPlayerIntent::Stop)
			&& !calls.take(PlaylistPlayerIntent::Stop),
			"internal stop authorization is also consumed before callbacks");
		calls.leave(stop);
		calls.leave(adapter);
	}
	{
		auto loading = PlaylistCursor();
		loading.reset(0, 0);
		const auto oldStore = loading.admission();
		loading.cancel();
		expect(!oldStore(), "expiry revokes the held Loading store admission");
		const auto afterExpiry = loading.admission();
		expect(afterExpiry(), "expiry cancellation leaves authority available for a fresh command");
		loading.reset(3, 0);
		expect(!oldStore(), "an explicit new load cannot revive the expired store reply");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.reset(3, 0);
		const auto initialResolve = cursor.admission();
		cursor.cancel();
		expect(!initialResolve() && cursor.index() == std::optional<std::size_t>(0),
			"expiry rejects initial resolution without discarding its saved selection");
		const auto retry = cursor.admission();
		expect(retry() && !initialResolve(), "an explicit Retry has fresh admission only");
		cursor.select(1, 1);
		const auto manualNext = cursor.admission();
		cursor.cancel();
		cursor.select(0, 1);
		expect(!manualNext() && cursor.index() == std::optional<std::size_t>(0),
			"expiry cancels pending Next and restores the still-loaded owned song");
		cursor.select(cursor.position() + 1, 1);
		const auto nextAfterExpiry = cursor.admission();
		expect(nextAfterExpiry() && !manualNext()
			&& cursor.index() == std::optional<std::size_t>(1),
			"explicit Next after expiry selects the canceled successor with fresh authority");
	}
	{
		const auto state = std::make_shared<PlaylistHandoffState>();
		const auto owner = state->enter();
		const auto oldFrame = state->begin();
		const auto oldNative = state->guard(oldFrame);
		auto oldPlayed = false;
		state->queue([&] { oldPlayed = true; });
		state->supersede();
		const auto expiryFrame = state->begin(true);
		expect(!oldNative() && state->guard(expiryFrame)(),
			"expiry revokes the suspended native call but gives pause its own valid frame");
		expect(!state->take() && !oldPlayed, "expiry drops the pre-expiry queued start");
		auto newPlayed = false;
		state->queue([&] { newPlayed = true; });
		state->finish(expiryFrame);
		state->finish(oldFrame);
		state->leave(owner);
		auto fresh = state->take();
		if (fresh) fresh();
		expect(newPlayed && !oldPlayed,
			"a command issued after expiry survives until all native frames unwind");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.reset(2, 1);
		const auto oldAdmission = cursor.admission();
		const auto state = std::make_shared<PlaylistHandoffState>();
		auto played = false;
		state->queue([&] { if (oldAdmission()) played = true; });
		auto held = state->take();
		cursor.cancel();
		state->supersede();
		if (held) held();
		expect(!played && !oldAdmission(),
			"expiry rejects an adapter start already taken from deferred delivery");
	}
	{
		auto calls = PlaylistInternalCalls();
		const auto pause = calls.enter(PlaylistPlayerIntent::Pause);
		expect(!calls.take(PlaylistPlayerIntent::Play),
			"expiry Pause cannot authorize a Play callback");
		expect(calls.take(PlaylistPlayerIntent::Pause)
			&& !calls.take(PlaylistPlayerIntent::Pause),
			"expiry Pause authorization is consumed before synchronous user Pause");
		calls.leave(pause);
	}
	{
		auto calls = PlaylistInternalCalls();
		const auto resume = calls.enter(PlaylistPlayerIntent::Resume);
		expect(calls.take(PlaylistPlayerIntent::Resume)
			&& !calls.take(PlaylistPlayerIntent::Resume),
			"deferred resume consumes its exact authorization before nested manual commands");
		expect(!calls.take(PlaylistPlayerIntent::Play)
			&& !calls.take(PlaylistPlayerIntent::Pause),
			"a resume delivery never authorizes an unrelated Play or Pause");
		calls.leave(resume);
	}
	{
		const auto random = [](std::size_t count) { return count - 1; };
		const auto normal = PlaylistOrderIndices(5, 2, Media::OrderMode::Default, random);
		const auto reverse = PlaylistOrderIndices(5, 2, Media::OrderMode::Reverse, random);
		const auto shuffle = PlaylistOrderIndices(5, 2, Media::OrderMode::Shuffle, random);
		expect(normal == std::vector<std::size_t>{0, 1, 2, 3, 4},
			"normal order retains the immutable visible snapshot");
		expect(reverse == std::vector<std::size_t>{4, 3, 2, 1, 0},
			"reverse order traverses the saved snapshot backwards");
		auto unique = shuffle;
		std::sort(unique.begin(), unique.end());
		expect(shuffle.front() == 2 && unique == normal,
			"shuffle starts at the selected current row and visits every saved row once");
		auto cursor = PlaylistCursor();
		cursor.reset(reverse.size(), std::find(reverse.begin(), reverse.end(), 2) - reverse.begin());
		auto played = std::vector<std::size_t>();
		while (const auto index = cursor.index()) {
			played.push_back(reverse[*index]);
			cursor.select(cursor.position() + 1, 1);
		}
		expect(played == std::vector<std::size_t>{2, 1, 0},
			"a selected reverse row plays first and stops at its saved-order boundary");
		expect(PlaylistOrderIndices(0, 0, Media::OrderMode::Shuffle, random).empty(),
			"empty shuffle neither selects a row nor asks for random entropy");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.setRepeatAll(true);
		cursor.reset(3, 2);
		expect(cursor.stepAvailable(1) && cursor.stepAvailable(-1),
			"Repeat All exposes both directions at either queue edge");
		const auto old = cursor.admission();
		cursor.select(cursor.position() + 1, 1);
		expect(cursor.index() == std::optional<std::size_t>(0) && !old(),
			"Repeat All wraps Next with fresh admission instead of entering a chat queue");
		cursor.select(cursor.position() - 1, -1);
		expect(cursor.index() == std::optional<std::size_t>(2),
			"Repeat All wraps Previous to the final row");
		cursor.setRepeatAll(false);
		cursor.select(3, 1);
		expect(!cursor.index() && !cursor.stepAvailable(1) && cursor.stepAvailable(-1),
			"turning Repeat All off restores non-wrapping None and One navigation");
		cursor.setRepeatAll(true);
		cursor.reset(1, 0);
		cursor.select(1, 1);
		expect(cursor.index() == std::optional<std::size_t>(0) && cursor.stepAvailable(-1),
			"one-song Repeat All can repeat in either manual direction");
		cursor.reset(0, 0);
		expect(!cursor.index() && !cursor.stepAvailable(1),
			"Repeat All cannot manufacture a row in an empty snapshot");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.setRepeatAll(true);
		cursor.reset(3, 1);
		auto visited = std::vector<std::size_t>();
		while (const auto index = cursor.index()) {
			visited.push_back(*index);
			cursor.skip();
		}
		expect(visited == std::vector<std::size_t>{1, 2, 0},
			"an all-unavailable repeating queue exhausts after one traversal without a skip loop");
		cursor.select(cursor.position() + 1, 1);
		expect(cursor.index() == std::optional<std::size_t>(0),
			"explicit Next after unavailable exhaustion restarts at the first repeat row");
		cursor.select(1, -1);
		visited.clear();
		while (const auto index = cursor.index()) {
			visited.push_back(*index);
			cursor.skip();
		}
		expect(visited == std::vector<std::size_t>{1, 0, 2},
			"backward unavailable skipping is bounded across the repeat boundary too");
		cursor.select(cursor.position() - 1, -1);
		expect(cursor.index() == std::optional<std::size_t>(2),
			"explicit Previous after backward exhaustion restarts at the final repeat row");
		expect(ResolveIndices(5, 4, 1, 10, true) == std::vector<std::size_t>{4, 0, 1, 2},
			"Repeat All lookahead crosses the end within the four-result bound");
		expect(ResolveIndices(5, 0, -1, 4, true) == std::vector<std::size_t>{0, 4, 3, 2},
			"backward repeat lookahead retains exact wrapped index identity");
		expect(ResolveIndices(2, 1, 1, 4, true) == std::vector<std::size_t>{1, 0},
			"a short repeating queue never duplicates speculative resolution rows");
	}
	{
		auto cursor = PlaylistCursor();
		cursor.reset(5, 1);
		cursor.skipUnresolved([](std::size_t index) { return index == 3; });
		expect(cursor.index() == std::optional<std::size_t>(3),
			"automatic movement skips unresolved rows to the next completed result without network wait");
		cursor.select(4, 1);
		cursor.skipUnresolved([](std::size_t) { return false; });
		expect(!cursor.index(), "unresolved automatic tail exhausts without entering chat playback");
		cursor.setRepeatAll(true);
		cursor.reset(5, 3);
		cursor.skipUnresolved([](std::size_t index) { return index == 1; });
		expect(cursor.index() == std::optional<std::size_t>(1),
			"automatic Repeat All can cross the boundary to a completed row");
		cursor.select(2, 1);
		cursor.skipUnresolved([](std::size_t) { return false; });
		expect(!cursor.index(), "an entirely unresolved repeating queue cannot spin or wait");
	}
	{
		auto entropyCalls = 0;
		const auto random = [&](std::size_t count) {
			++entropyCalls;
			return count - 1;
		};
		expect(PlaylistStartIndex(5, std::nullopt, 2, Media::OrderMode::Default, random) == 0
			&& PlaylistStartIndex(5, std::nullopt, 2, Media::OrderMode::Reverse, random) == 4,
			"Play without a selected row starts at the correct normal or reverse edge");
		expect(PlaylistStartIndex(5, 1, 2, Media::OrderMode::Reverse, random) == 1
			&& PlaylistStartIndex(5, 1, 2, Media::OrderMode::Shuffle, random) == 1,
			"explicit song selection wins over current-first shuffle and reverse defaults");
		expect(PlaylistStartIndex(5, std::nullopt, 2, Media::OrderMode::Shuffle, random) == 2
			&& entropyCalls == 0,
			"shuffle retains a current song from the snapshot without replacing its start");
		expect(PlaylistStartIndex(5, std::nullopt, std::nullopt, Media::OrderMode::Shuffle, random) == 4
			&& entropyCalls == 1,
			"shuffle chooses a random first song when no current song belongs to the snapshot");
		expect(PlaylistStartIndex(0, std::nullopt, std::nullopt, Media::OrderMode::Shuffle, random) == 0
			&& entropyCalls == 1,
			"an empty shuffle never requests random entropy");
	}
	{
		const auto saved = std::vector<PlaylistResolveSong>{
			{ 11, {} },
			{ 22, {} },
			{ 33, {} },
		};
		for (const auto mode : { Media::OrderMode::Default, Media::OrderMode::Reverse,
			Media::OrderMode::Shuffle }) {
			const auto order = PlaylistOrderIndices(3, 1, mode,
				[](std::size_t count) { return count - 1; });
			auto traversal = std::vector<PlaylistResolveSong>();
			for (const auto index : order) traversal.push_back(saved[index]);
			const auto mapped = PlaylistIndexForDocument(traversal, 22);
			expect(mapped && traversal[*mapped].documentId == 22,
				"current eligibility is rekeyed by document identity after each order change");
			expect(!PlaylistIndexForDocument(traversal, 44),
				"mode reindexing cannot grant eligibility to a document outside the snapshot");
		}
		const auto single = std::vector<PlaylistResolveSong>{ { 11, {} } };
		const auto mapped = PlaylistIndexForDocument(single, 11);
		expect(mapped == std::optional<std::size_t>(0)
			&& ResolveIndices(1, *mapped, 1, 4, true) == std::vector<std::size_t>{0},
			"one-song Repeat All retains a distinct current slot even with no ahead slots");
	}
	std::cout << "PLAYLIST_PLAYER_CHECKS=" << checks << " FAILURES=" << failures << '\n';
	return failures ? 1 : 0;
}
