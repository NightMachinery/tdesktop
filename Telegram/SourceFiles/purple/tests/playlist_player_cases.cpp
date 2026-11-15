#include "purple/purple_playlists_resolve_queue.h"
#include "purple/hooks/playlist_player.h"

#include <algorithm>
#include <iostream>
#include <memory>

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
	std::cout << "PLAYLIST_PLAYER_CHECKS=" << checks << " FAILURES=" << failures << '\n';
	return failures ? 1 : 0;
}
