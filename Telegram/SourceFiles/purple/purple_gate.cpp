/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_gate.h"

#include "base/timer.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "purple/purple_config.h"

#include <QtCore/QDateTime>

namespace Purple {
namespace {

// Local wall-clock seconds, matching what peek_deadline_unix holds. Not
// base::unixtime::now(), which is the server's clock and needs a connection:
// a peek is a two-minute local affair and must expire while offline too.
[[nodiscard]] int64 NowUnix() {
	return QDateTime::currentSecsSinceEpoch();
}

[[nodiscard]] Resolved NormalResolution() {
	auto result = Resolved();
	result.preset = NormalPreset();
	result.normal = true;
	return result;
}

// Holds the resolution the rest of the app reads. Recomputing it is cheap but
// not free - it walks every list in the file - and the answer is needed once
// per chat per repaint, so it is computed on change rather than on demand.
class Gate final {
public:
	Gate();

	[[nodiscard]] const Resolved &resolved() const {
		return _resolved;
	}
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

private:
	// `settingsChanged' says which of the two signals brought us here. It
	// matters only when the resolution comes out identical - see refresh().
	void refresh(bool settingsChanged);
	void refreshPeekTimer(const State &state, bool peeking);

	Resolved _resolved = NormalResolution();

	// What the "until" decisions were last time we looked. They live in the
	// state rather than in the resolution - they are about one chat, not about
	// what the preset means - so a new one produces a bit-identical Resolved
	// and would otherwise tell nobody. Exactly the shape of the membership bug
	// the comment in refresh() describes.
	std::vector<Override> _overrides;

	bool _refreshing = false;

	// Nothing else would look at the deadline again, so without this a peek
	// would sit there until the next unrelated config change. It is the only
	// thing that ends one on time.
	base::Timer _peekTimer;

	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

Gate::Gate() {
	_peekTimer.setCallback([] {
		UpdateState([](State &state) {
			state.peekActive = false;
			state.peekDeadlineUnix = 0;
		});
	});
	refresh(false);

	// Kept apart rather than merged, because the two are not interchangeable
	// once the resolution comes out identical: a settings change can still mean
	// something, and a state change cannot.
	SettingsChanges(
	) | rpl::on_next([=] {
		refresh(true);
	}, _lifetime);

	StateChanges(
	) | rpl::on_next([=] {
		refresh(false);
	}, _lifetime);
}

void Gate::refresh(bool settingsChanged) {
	// Persisting the cache below fires StateChanges(), which lands back here.
	// The nested pass would resolve to the same thing and write nothing, so it
	// is only wasted work and a duplicate notification - but the notification
	// is what rebuilds every chat list, so it is worth not sending twice.
	if (_refreshing) {
		return;
	}
	_refreshing = true;
	const auto guard = gsl::finally([&] { _refreshing = false; });

	const auto &settings = ActiveSettings();
	const auto &state = CurrentState();
	const auto wanted = state.activePreset.isEmpty()
		? NormalPreset()
		: state.activePreset;

	const auto overridesChanged = (state.overrides != _overrides);
	_overrides = state.overrides;

	auto next = Resolve(settings, wanted);
	if (!next) {
		// The preset was deleted or renamed while it was active, or its
		// inheritance chain broke. Falling back to defaults here would unhide
		// every chat the user hid, which is the one outcome a work mode must
		// never produce by accident, so run on the last resolution that worked.
		next = FromCache(state.resolvedCache);
		LOG(("Purple Error: Preset '%1' does not resolve; %2."
			).arg(wanted
			).arg(next
				? u"keeping the cached resolution"_q
				: u"keeping the one already in effect"_q));
	}
	if (!next) {
		return;
	}

	// Peek suspends the hiding of whatever resolution is in force; it is not
	// part of the resolution itself, which is why it is applied here rather
	// than in Resolve(). ToCache() below ignores the flag, so a peek can never
	// be persisted into the fallback.
	next->peeking = !next->normal && PeekLive(state, NowUnix());
	refreshPeekTimer(state, next->peeking);

	if (*next == _resolved) {
		// Membership is not part of a resolution. Resolved holds the list
		// *names* a preset ordered, and MatchList() looks the members up live
		// against ActiveSettings() - so adding a chat to a list produces a
		// byte-different file and a bit-identical resolution. Without this
		// nobody would be told, and the chat would sit exactly where it was
		// until some later change moved the resolution: switching presets and
		// back, which is what made the context menu look broken.
		//
		// Only for a settings change, and only while a preset is running. A
		// state change lands here from inside this very function - the cache
		// write below - and from the peek timer, and firing there would rebuild
		// every chat list for a write that changed nothing anyone can see.
		// Normal has nothing to rebuild at all: an unconfigured fork must go on
		// paying nothing for a file it is not using. A settings change that
		// *starts* a preset moves the resolution and never reaches here.
		if ((settingsChanged || overridesChanged) && !_resolved.normal) {
			_changes.fire({});
		}
		return;
	}
	_resolved = std::move(*next);
	LOG(("Purple: preset '%1'%2%3, %4 lists."
		).arg(_resolved.preset
		).arg(_resolved.normal ? u" (stock behaviour)"_q : QString()
		).arg(_resolved.peeking ? u" (peeking)"_q : QString()
		).arg(_resolved.lists.size()));
	_changes.fire({});

	// Only ever widened, never cleared: a resolution we could not compute is
	// exactly when the cache has to still be there.
	if (!_resolved.normal) {
		auto cache = ToCache(_resolved);
		UpdateState([&](State &state) {
			state.resolvedCache = cache;
		});
	}
}

void Gate::refreshPeekTimer(const State &state, bool peeking) {
	_peekTimer.cancel();
	if (peeking) {
		if (const auto deadline = state.peekDeadlineUnix) {
			// At least a second, so a deadline that has just landed still goes
			// through the timer instead of writing state from inside refresh().
			const auto left = std::max(deadline - NowUnix(), int64(1));
			_peekTimer.callOnce(left * crl::time(1000));
		}
	} else if (state.peekActive) {
		// A peek that outlived the app, or the preset it was revealing. Clear
		// it rather than leave state.toml claiming a peek that is not running:
		// the flag is what the next toggle reads to decide which way to go.
		UpdateState([](State &state) {
			state.peekActive = false;
			state.peekDeadlineUnix = 0;
		});
	}
}

// Deliberately never destroyed, for the same reason as the config singleton -
// it holds an rpl subscription to something with static storage duration.
[[nodiscard]] Gate &Instance() {
	static const auto result = new Gate();
	return *result;
}

// What the folders holding this chat said about its stories, if any did.
//
// Most permissive wins, as with the exempt walk: a narrow folder saying no must
// not speak for a wide one that would say yes. The enum's declaration order is
// the permissiveness order here - unlike ShowMode's, which is why that one
// needs ShowModeRank() and this one does not.
[[nodiscard]] std::optional<StoryMode> StoryFolderMode(
		not_null<const PeerData*> peer) {
	const auto &folders = Instance().resolved().storyFolders;
	if (folders.empty()) {
		// The common case, and free: a preset that said nothing about any
		// folder's stories never reaches the walk below.
		return std::nullopt;
	}
	const auto history = peer->owner().historyLoaded(peer->id);
	if (!history) {
		return std::nullopt;
	}
	auto result = std::optional<StoryMode>();
	for (const auto &filter : peer->owner().chatsFilters().list()) {
		if (!filter.id()) {
			continue;
		}
		for (const auto &folder : folders) {
			if (filter.title().text.text.compare(
					folder.name,
					Qt::CaseInsensitive)
				|| !filter.contains(history)) {
				continue;
			}
			if (!result || int(folder.mode) < int(*result)) {
				result = folder.mode;
			}
		}
	}
	return result;
}

// Whether the preset excludes this chat outright, rather than merely holding it
// back for being quiet. The distinction is the whole of what `follow' means: a
// story IS new activity, so somebody the preset admits under `message' or
// `mention' keeps theirs, and only somebody it refuses altogether loses it.
[[nodiscard]] bool ExcludedOutright(not_null<const PeerData*> peer) {
	if (VisibleFor(peer).show != ShowMode::Never) {
		return false;
	}
	// A folder that pulls the chat into the view speaks for it here too, the
	// same way it overrides hiding in purpleHiddenFromView().
	if (const auto history = peer->owner().historyLoaded(peer->id)) {
		return !history->purpleExemptFolderMode().has_value();
	}
	return true;
}

} // namespace

ChatKind KindOf(not_null<const PeerData*> peer) {
	if (peer->isBot()) {
		return ChatKind::Bot;
	} else if (peer->isUser()) {
		return ChatKind::Private;
	} else if (peer->isBroadcast()) {
		return ChatKind::Channel;
	}
	// Basic groups and supergroups alike: the distinction is a migration
	// detail, and a chat that gets upgraded must not change which list it
	// falls into.
	return ChatKind::Group;
}

PeerIdValue IdOf(not_null<const PeerData*> peer) {
	const auto id = peer->id;
	if (const auto user = peerToUser(id)) {
		return user.bare;
	} else if (const auto chat = peerToChat(id)) {
		return chat.bare;
	} else if (const auto channel = peerToChannel(id)) {
		return channel.bare;
	}
	return 0;
}

MemberTitle TitleResolver(not_null<Main::Session*> session) {
	const auto owner = &session->data();
	return [=](PeerIdValue id) {
		const auto bare = BareId(id);
		// The file keeps the bare id, which is all IdOf() ever had, so the type
		// has to be guessed back. Bare ids are unique across the three kinds in
		// practice; if they ever were not, the worst case is a stale name in a
		// comment nothing reads back.
		const auto peer = [&]() -> PeerData* {
			if (const auto user = owner->peerLoaded(peerFromUser(bare))) {
				return user;
			} else if (const auto chat = owner->peerLoaded(peerFromChat(bare))) {
				return chat;
			}
			return owner->peerLoaded(peerFromChannel(bare));
		}();
		return peer ? peer->name() : QString();
	};
}

const Resolved &ActiveResolved() {
	return Instance().resolved();
}

rpl::producer<> ActiveChanges() {
	return Instance().changes();
}

QString ViewName() {
	const auto &resolved = Instance().resolved();
	return PresetTitle(resolved.preset, resolved.viewName);
}

const std::vector<ResolvedView> &ExtraViews() {
	static const auto kNone = std::vector<ResolvedView>();
	const auto &resolved = Instance().resolved();
	return resolved.normal ? kNone : resolved.views;
}

bool ExtraViewHolds(int index, not_null<const PeerData*> peer) {
	const auto &views = ExtraViews();
	if (index < 0 || index >= int(views.size())) {
		return false;
	}
	return ViewHolds(
		ActiveSettings(),
		views[index],
		IdOf(peer),
		KindOf(peer));
}

const std::vector<PeerIdValue> &ExtraViewPins(int index) {
	static const auto kNone = std::vector<PeerIdValue>();
	const auto &views = ExtraViews();
	return (index < 0 || index >= int(views.size()))
		? kNone
		: views[index].pinned;
}

bool SaveExtraViewPins(
		int index,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	const auto &resolved = Instance().resolved();
	const auto &views = ExtraViews();
	if (index < 0 || index >= int(views.size())) {
		return false;
	}
	return SetViewPins(resolved.preset, views[index].name, ids, title);
}

const std::vector<PeerIdValue> &PresetPins() {
	static const auto kNone = std::vector<PeerIdValue>();
	const auto &resolved = Instance().resolved();
	return resolved.normal ? kNone : resolved.pinned;
}

bool PresetOwnsPins() {
	return !PresetPins().empty();
}

bool SavePresetPins(
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	const auto &resolved = Instance().resolved();
	if (resolved.normal) {
		return false;
	}
	return SetPresetPins(resolved.preset, ids, title);
}

std::optional<OverrideKind> OverrideFor(not_null<const PeerData*> peer) {
	const auto &resolved = Instance().resolved();
	if (resolved.normal) {
		// Normal is a bypass, and an override is a statement about a preset.
		return std::nullopt;
	}
	const auto found = Purple::OverrideFor(
		CurrentState(),
		IdOf(peer),
		resolved.preset,
		NowUnix());
	return found ? std::make_optional(found->kind) : std::nullopt;
}

void SetOverride(
		not_null<const PeerData*> peer,
		OverrideKind kind,
		int seconds) {
	const auto id = IdOf(peer);
	const auto &resolved = Instance().resolved();
	if (!id || resolved.normal) {
		return;
	}
	const auto preset = resolved.preset;
	const auto until = NowUnix() + seconds;
	UpdateState([&](State &state) {
		// One per chat per preset: a second "until" replaces the first rather
		// than queueing behind it, because the menu offers a decision and not
		// a schedule.
		auto kept = std::vector<Override>();
		kept.reserve(state.overrides.size() + 1);
		for (auto &entry : state.overrides) {
			if (entry.peer != id
				|| entry.preset.compare(preset, Qt::CaseInsensitive)) {
				kept.push_back(std::move(entry));
			}
		}
		if (seconds > 0) {
			kept.push_back({ id, kind, until, until - seconds, preset });
		}
		state.overrides = std::move(kept);
	});
}

void ClearOverride(not_null<const PeerData*> peer) {
	SetOverride(peer, OverrideKind::Show, 0);
}

HideScope HideUntilScope() {
	return ActiveSettings().overrides.hideScope;
}

bool PruneOverrides() {
	auto dropped = false;
	const auto now = NowUnix();
	UpdateState([&](State &state) {
		dropped = Purple::PruneOverrides(state, now);
	});
	return dropped;
}

int64 NextOverrideDeadline() {
	const auto &resolved = Instance().resolved();
	return resolved.normal
		? 0
		: Purple::NextOverrideDeadline(CurrentState(), resolved.preset);
}

bool Filtering() {
	return !Instance().resolved().normal;
}

bool NamedExplicitly(not_null<const PeerData*> peer) {
	return Filtering()
		&& NamedExplicitly(
			ActiveSettings(),
			Instance().resolved(),
			IdOf(peer));
}

int RecentStaySeconds() {
	// Nothing is hidden under Normal, so nothing has to be kept from going.
	return Filtering() ? ActiveSettings().recent.staySecondsAfterClose : 0;
}

RecentScope RecentAppliesTo() {
	return ActiveSettings().recent.scope;
}

RecentStyle RecentMarkStyle() {
	return Filtering() ? ActiveSettings().recent.style : RecentStyle::None;
}

OverrideSpan OverrideDeadline(not_null<const PeerData*> peer) {
	const auto &resolved = Instance().resolved();
	if (resolved.normal) {
		return OverrideSpan();
	}
	const auto found = Purple::OverrideFor(
		CurrentState(),
		IdOf(peer),
		resolved.preset,
		NowUnix());
	return found
		? OverrideSpan{ found->startedUnix, found->untilUnix, found->kind }
		: OverrideSpan();
}

Visibility VisibleFor(not_null<const PeerData*> peer) {
	// No exemption for Saved Messages. It used to be unconditionally visible
	// and unconditionally audible, on the argument that nothing arrives in it
	// unbidden - but a preset names what gets through, and a chat that ignores
	// that is a chat you cannot reason about from the file. Naming the self id
	// in a list is how you keep it; see docs/purple/config.md.
	return Visible(
		ActiveSettings(),
		Instance().resolved(),
		IdOf(peer),
		KindOf(peer));
}

const std::vector<PresetFolder> &ShownFolders() {
	// A hidden folder is hidden, so a peek brings it back with everything else
	// - which under the new model is spelled "*ALL", the same thing a preset
	// writes when it wants the whole strip.
	static const auto kAll = std::vector<PresetFolder>{
		PresetFolder{ .name = AllFoldersName() },
	};
	const auto &resolved = Instance().resolved();
	return resolved.peeking ? kAll : resolved.folders;
}

bool Peeking() {
	return Instance().resolved().peeking;
}

bool HideEverywhere() {
	const auto &resolved = Instance().resolved();
	return !resolved.normal
		&& !resolved.peeking
		&& resolved.hideEverywhere;
}

PeekChange TogglePeek() {
	const auto &resolved = Instance().resolved();
	if (resolved.normal) {
		// Nothing to reveal, and starting one anyway would leave a peek
		// running that no chat list could show the end of.
		return { .refused = true };
	}
	const auto wanted = !resolved.peeking;
	const auto seconds = wanted ? ActiveSettings().peek.autoOffSeconds : 0;
	UpdateState([&](State &state) {
		state.peekActive = wanted;
		state.peekDeadlineUnix = (seconds > 0) ? (NowUnix() + seconds) : 0;
	});
	return { .peeking = wanted, .seconds = seconds };
}

const std::vector<ExemptFolder> &ExemptFolders() {
	static const auto kNone = std::vector<ExemptFolder>();
	const auto &resolved = Instance().resolved();
	if (resolved.normal || resolved.folders.empty()) {
		return kNone;
	}
	// Not short-circuited by a peek, although it used to be. Revealing is
	// History::purpleHiddenFromView()'s job and it says so itself now; this
	// list also answers "is this archived chat in the view at all", which a
	// peek does not change - an archived chat a folder pulled in used to LEAVE
	// the view the moment you started peeking, which is backwards.
	return resolved.exemptFolders;
}

const std::vector<QString> &QuietFolders() {
	static const auto kNone = std::vector<QString>();
	const auto &resolved = Instance().resolved();

	// Not lifted by a peek, unlike hiding. A peek is a look at the chat list;
	// it is not a request to be interrupted by a folder you asked to be quiet.
	return resolved.normal ? kNone : resolved.quietFolders;
}

const std::vector<QString> &SilencedFolders() {
	static const auto kNone = std::vector<QString>();
	const auto &resolved = Instance().resolved();
	return resolved.normal ? kNone : resolved.silencedFolders;
}

bool FoldersRestricted() {
	const auto &resolved = Instance().resolved();
	if (resolved.normal) {
		return false;
	} else if (!resolved.views.empty()) {
		// Extra views sit on the strip too, so a strip index is no longer the
		// real list's index shifted by one - the preset's main view standing in
		// for All chats is what made that arithmetic work. Checked before the
		// peek below, because a peek reveals folders and leaves the extra views
		// exactly where they were.
		return true;
	} else if (resolved.peeking) {
		return false;
	}
	// "*ALL" on its own is every folder in the account's own order, so nothing
	// is restricted and reordering stays safe. Any other shape - a subset, a
	// chosen order, a folder with flags on it - means a strip index no longer
	// matches the server's, and saveOrder() must refuse.
	const auto &folders = resolved.folders;
	return (folders.size() != 1)
		|| !IsAllFolders(folders.front())
		|| folders.front().show.has_value()
		|| folders.front().notify.has_value()
		|| folders.front().include.has_value();
}

const EffectiveList *ListFor(not_null<const PeerData*> peer) {
	return MatchList(
		ActiveSettings(),
		Instance().resolved(),
		IdOf(peer),
		KindOf(peer));
}

bool StoryShown(not_null<const PeerData*> peer, bool hasUnseen) {
	if (!Filtering()) {
		return true;
	}
	const auto &resolved = Instance().resolved();
	if (resolved.stories == StoryPolicy::None) {
		return false;
	} else if (resolved.peeking) {
		// A peek reveals stories along with the chats they belong to. It is one
		// deliberate look at what the preset is keeping from you, and a strip
		// that stayed filtered through it would be answering a question nobody
		// asked twice.
		return true;
	}

	// A folder beats a list entry, the same way a folder already beats one for
	// hiding, and both beat the preset's own policy.
	auto mode = StoryFolderMode(peer);
	if (!mode) {
		if (const auto entry = ListFor(peer)) {
			mode = entry->stories;
		}
	}
	if (mode) {
		switch (*mode) {
		case StoryMode::Always: return true;
		case StoryMode::Unseen: return hasUnseen;
		case StoryMode::Never: return false;
		}
	}
	switch (resolved.stories) {
	case StoryPolicy::All: return true;
	case StoryPolicy::AllUnseen: return hasUnseen;
	case StoryPolicy::Follow: return !ExcludedOutright(peer);
	case StoryPolicy::FollowUnseen:
		return hasUnseen && !ExcludedOutright(peer);
	case StoryPolicy::None: return false;
	}
	return true;
}

} // namespace Purple
