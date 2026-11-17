/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_gate.h"

#include "base/timer.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_peek_controls.h"
#include "window/window_controller.h"

#include <QtCore/QDateTime>

#include <utility>

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
	[[nodiscard]] bool globalChoiceReady() const;
	[[nodiscard]] bool refreshGlobalChoiceNormal();

	[[nodiscard]] const Resolved &resolved() const {
		return _resolved;
	}
	[[nodiscard]] const Resolved &resolved(not_null<Main::Session*> session);
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

private:
	// `settingsChanged' says which of the two signals brought us here. It
	// matters only when the resolution comes out identical - see refresh().
	void refresh(bool settingsChanged);
	void refreshPeekTimer(const State &state);

	Resolved _resolved = NormalResolution();
	std::map<QString, Resolved> _accountResolved;
	std::map<QString, AccountPeek> _accountPeeks;
	bool _initialized = false;

	// What the "until" decisions were last time we looked. They live in the
	// state rather than in the resolution - they are about one chat, not about
	// what the preset means - so a new one produces a bit-identical Resolved
	// and would otherwise tell nobody. Exactly the shape of the membership bug
	// the comment in refresh() describes.
	std::vector<Override> _overrides;

	bool _refreshing = false;
	std::optional<bool> _refreshPending;

	// Nothing else would look at the deadline again, so without this a peek
	// would sit there until the next unrelated config change. It is the only
	// thing that ends one on time.
	base::Timer _peekTimer;

	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;
	rpl::lifetime _sessionsLifetime;

};

bool Gate::globalChoiceReady() const {
	return !_refreshing;
}

bool Gate::refreshGlobalChoiceNormal() {
	if (_refreshing) { return false; }
	refresh(false);
	return _resolved.normal;
}

Gate::Gate() {
	_peekTimer.setCallback([=] { refresh(false); });
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
	auto &domain = Core::App().domain();
	const auto observe = [=, &domain] {
		_sessionsLifetime.destroy();
		for (const auto &entry : domain.accounts()) {
			entry.account->sessionChanges() | rpl::on_next([=] {
				refresh(true);
			}, _sessionsLifetime);
		}
		refresh(true);
	};
	domain.accountsChanges() | rpl::on_next(observe, _lifetime);
	domain.activeSessionChanges() | rpl::on_next([=] {
		refresh(true);
		_changes.fire({});
	}, _lifetime);
	observe();
}

void Gate::refresh(bool settingsChanged) {
	if (_refreshing) {
		_refreshPending = _refreshPending.value_or(false) || settingsChanged;
		return;
	}
	_refreshing = true;
	const auto guard = gsl::finally([&] {
		_refreshing = false;
		const auto pending = std::exchange(_refreshPending, std::nullopt);
		if (pending) { refresh(*pending); }
	});

	const auto &settings = ActiveSettings();
	auto state = CurrentState();
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

	const auto now = NowUnix();
	auto stopped = false;
	const auto reset = next->normal || (_initialized && _resolved.preset != next->preset);
	for (auto &[key, peek] : state.accountPeeks) {
		if (peek.active && (reset || (peek.deadlineUnix && peek.deadlineUnix <= now))) {
			peek.active = false;
			peek.deadlineUnix = 0;
			stopped = true;
		}
	}
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (!session || entry.account->loggingOut()) { continue; }
		const auto key = SessionAccountKey(session);
		const auto text = AccountKeyText(key.environment, key.userId);
		if (!text) { continue; }
		if (AccountWorkModeForSession(session) != AccountWorkMode::Stock) { continue; }
		const auto i = state.accountPeeks.find(*text);
		if (i != state.accountPeeks.end() && i->second.active) {
			i->second.active = false;
			i->second.deadlineUnix = 0;
			stopped = true;
		}
	}
	if (stopped) {
		UpdateState([&](State &current) { current.accountPeeks = state.accountPeeks; });
	}
	auto endedAccounts = std::vector<QString>();
	for (const auto &[key, peek] : _accountPeeks) {
		if (peek.active && !PeekLive(AccountPeekState(state, key), now)) {
			endedAccounts.push_back(key);
		}
	}
	const auto peeksChanged = state.accountPeeks != _accountPeeks;
	_accountPeeks = state.accountPeeks;
	_accountResolved.clear();
	_initialized = true;
	next->peeking = false;
	refreshPeekTimer(state);

	if (*next == _resolved) {
		// Membership is not part of a resolution. Resolved holds the list
		// *names* a preset ordered, and MatchList() looks the members up live
		// against ActiveSettings() - so adding a chat to a list produces a
		// byte-different file and a bit-identical resolution. A restored
		// resolution also holds the list snapshot for names the file lacks.
		// Without this nobody would be told, and the chat would sit where it was
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
		if (peeksChanged || ((settingsChanged || overridesChanged) && !_resolved.normal)) {
			_changes.fire({});
		}
		if (settingsChanged && !_resolved.normal) {
			auto cache = ToCache(_resolved, settings);
			if (cache != state.resolvedCache) {
				UpdateState([&](State &state) {
					state.resolvedCache = std::move(cache);
				});
			}
		}
		for (const auto &key : endedAccounts) { CloseHiddenPeekChats(key); }
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
		auto cache = ToCache(_resolved, settings);
		UpdateState([&](State &state) {
			state.resolvedCache = cache;
		});
	}
	for (const auto &key : endedAccounts) { CloseHiddenPeekChats(key); }
}

void Gate::refreshPeekTimer(const State &state) {
	_peekTimer.cancel();
	auto deadline = int64(0);
	for (const auto &[key, peek] : state.accountPeeks) {
		if (peek.active && peek.deadlineUnix
			&& (!deadline || peek.deadlineUnix < deadline)) {
			deadline = peek.deadlineUnix;
		}
	}
	if (deadline) {
		_peekTimer.callOnce(std::max(deadline - NowUnix(), int64(1)) * crl::time(1000));
	}
}

const Resolved &Gate::resolved(not_null<Main::Session*> session) {
	static const auto normal = NormalResolution();
	if (AccountWorkModeForSession(session) == AccountWorkMode::Stock) { return normal; }
	const auto key = SessionAccountKey(session);
	const auto text = AccountKeyText(key.environment, key.userId);
	if (!text) { return normal; }
	const auto [i, inserted] = _accountResolved.try_emplace(*text, _resolved);
	if (inserted) {
		i->second.peeking = !i->second.normal
			&& PeekLive(AccountPeekState(CurrentState(), *text), NowUnix());
	}
	return i->second;
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
	const auto &folders = ActiveResolved(&peer->session()).storyFolders;
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

bool GlobalAccountChoiceActivationReady() {
	return Instance().globalChoiceReady();
}

bool RefreshGlobalAccountChoiceNormal() {
	return Instance().refreshGlobalChoiceNormal();
}

const Resolved &ActiveResolved() {
	static const auto normal = NormalResolution();
	const auto window = Core::App().activeWindow();
	const auto session = window ? window->maybeSession() : nullptr;
	return session ? Instance().resolved(session) : normal;
}

const Resolved &ActiveResolved(not_null<Main::Session*> session) {
	return Instance().resolved(session);
}

bool Filtering(not_null<Main::Session*> session) {
	return !ActiveResolved(session).normal;
}

rpl::producer<> ActiveChanges() {
	return Instance().changes();
}

QString ViewName() {
	const auto &resolved = ActiveResolved();
	return PresetTitle(resolved.preset, resolved.viewName);
}

const std::vector<ResolvedView> &ExtraViews() {
	static const auto kNone = std::vector<ResolvedView>();
	const auto &resolved = ActiveResolved();
	return resolved.normal ? kNone : resolved.views;
}

bool ExtraViewHolds(int index, not_null<const PeerData*> peer) {
	if (!Filtering(&peer->session())) {
		return false;
	}

	const auto &views = ExtraViews(&peer->session());
	if (index < 0 || index >= int(views.size())) {
		return false;
	}
	return ViewHolds(
		AccountSettings(&peer->session()),
		ActiveResolved(&peer->session()),
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
	const auto &resolved = ActiveResolved();
	const auto &views = ExtraViews();
	if (index < 0 || index >= int(views.size())) {
		return false;
	}
	return SetViewPins(resolved.preset, views[index].name, ids, title);
}

const std::vector<PeerIdValue> &PresetPins() {
	static const auto kNone = std::vector<PeerIdValue>();
	const auto &resolved = ActiveResolved();
	return resolved.normal ? kNone : resolved.pinned;
}

bool PresetOwnsPins() {
	return !PresetPins().empty();
}

bool PresetOwnsPins(not_null<Main::Session*> session) {
	return !PresetPins(session).empty();
}

bool SavePresetPins(
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	const auto &resolved = ActiveResolved();
	if (resolved.normal) {
		return false;
	}
	return SetPresetPins(resolved.preset, ids, title);
}

std::optional<OverrideKind> OverrideFor(not_null<const PeerData*> peer) {
	const auto &resolved = ActiveResolved(&peer->session());
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
	const auto &resolved = ActiveResolved(&peer->session());
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
	return !ActiveResolved().normal;
}

bool NamedExplicitly(not_null<const PeerData*> peer) {
	return Filtering(&peer->session())
		&& NamedExplicitly(
			AccountSettings(&peer->session()),
			ActiveResolved(&peer->session()),
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
	const auto &resolved = ActiveResolved(&peer->session());
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
		AccountSettings(&peer->session()),
		ActiveResolved(&peer->session()),
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
	const auto &resolved = ActiveResolved();
	return resolved.peeking ? kAll : resolved.folders;
}

bool Peeking() {
	return ActiveResolved().peeking;
}

bool HideEverywhere() {
	const auto &resolved = ActiveResolved();
	return !resolved.normal
		&& !resolved.peeking
		&& resolved.hideEverywhere;
}

bool HiddenFromSuggestions(not_null<History*> history) {
	if (!Filtering(&history->session())) {
		return false;
	}
	// Ordered so that a chat under Normal costs one bool: the two History
	// questions below both walk the preset's lists, and this runs once per
	// row of every strip the app draws.
	const auto &resolved = ActiveResolved(&history->session());
	return !resolved.normal
		&& !resolved.peeking
		&& ActiveSettings().suggestions.hideInvisible
		&& history->purpleHiddenByPreset()
		&& !history->purpleReachableElsewhere();
}

State CurrentPeekState(not_null<Main::Session*> session) {
	const auto key = SessionAccountKey(session);
	return AccountPeekState(CurrentState(), AccountKeyText(key.environment, key.userId).value_or(QString()));
}

State CurrentPeekState() {
	const auto window = Core::App().activeWindow();
	const auto session = window ? window->maybeSession() : nullptr;
	return session ? CurrentPeekState(session) : State();
}

void UpdatePeekState(not_null<Main::Session*> session, Fn<void(State&)> apply) {
	const auto key = SessionAccountKey(session);
	const auto text = AccountKeyText(key.environment, key.userId);
	if (!text || session->account().loggingOut()
		|| session->account().maybeSession() != session.get()) { return; }
	UpdateState([&](State &state) {
		auto peek = AccountPeekState(state, *text);
		apply(peek);
		SaveAccountPeek(state, *text, peek);
	});
}

PeekChange details::ActivatePeekFor(not_null<Main::Session*> session, int seconds) {
	const auto window = Core::App().activeWindow();
	if (!window || window->maybeSession() != session || window->locked()
		|| Core::App().passcodeLocked() || Core::App().screenIsLocked()
		|| !Filtering(session)) {
		// Nothing to reveal, and starting one anyway would leave a peek
		// running that no chat list could show the end of.
		return { .refused = true };
	}
	const auto now = NowUnix();
	UpdatePeekState(session, [&](State &state) {
		[[maybe_unused]] const auto started = StartPeek(state, now, seconds);
	});
	return {
		.peeking = true,
		.seconds = seconds,
		.leftSeconds = PeekLeftSeconds(CurrentPeekState(session), NowUnix()),
	};
}

PeekChange details::ActivatePeekExtension(not_null<Main::Session*> session, int seconds) {
	const auto window = Core::App().activeWindow();
	if (!window || window->maybeSession() != session || !Filtering(session)) { return { .refused = true }; }
	const auto now = NowUnix();
	auto extended = false;
	UpdatePeekState(session, [&](State &state) {
		extended = ExtendPeek(state, now, seconds, kPeekExtendCapSeconds);
	});
	return {
		.peeking = Peeking(session),
		.extended = extended,
		.seconds = seconds,
		.leftSeconds = PeekLeftSeconds(CurrentPeekState(session), NowUnix()),
	};
}

PeekChange ExtendPeekBy(int seconds) {
	const auto result = RequestWait({ .action = WaitAction::ExtendPeek, .seconds = seconds });
	return {
		.peeking = Peeking(),
		.pending = result.outcome == WaitOutcome::Pending,
		.refused = result.outcome == WaitOutcome::Refused,
		.extended = result.outcome == WaitOutcome::Applied,
		.seconds = seconds,
		.leftSeconds = PeekLeftSeconds(CurrentPeekState(), NowUnix()),
	};
}

PeekChange details::DeactivatePeek(not_null<Main::Session*> session) {
	UpdatePeekState(session, [](State &state) { StopPeek(state); });
	return {};
}

PeekChange EndPeek() {
	RequestWait({ .action = WaitAction::EndPeek });
	return {};
}

bool ReportLock(LockKind kind) {
	CancelPendingPeek();
	const auto now = NowUnix();
	auto ended = false;
	UpdateState([&](State &state) {
		for (const auto &[key, saved] : state.accountPeeks) {
			auto peek = AccountPeekState(state, key);
			if (EndPeekForLock(peek, now, ActiveSettings(), ThisDevice(), kind)) {
				SaveAccountPeek(state, key, peek);
				ended = true;
			}
		}
	});
	return ended;
}

const std::vector<ExemptFolder> &ExemptFolders() {
	static const auto kNone = std::vector<ExemptFolder>();
	const auto &resolved = ActiveResolved();
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
	const auto &resolved = ActiveResolved();

	// Not lifted by a peek, unlike hiding. A peek is a look at the chat list;
	// it is not a request to be interrupted by a folder you asked to be quiet.
	return resolved.normal ? kNone : resolved.quietFolders;
}

const std::vector<QString> &SilencedFolders() {
	static const auto kNone = std::vector<QString>();
	const auto &resolved = ActiveResolved();
	return resolved.normal ? kNone : resolved.silencedFolders;
}

bool FoldersRestricted() {
	const auto &resolved = ActiveResolved();
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
		AccountSettings(&peer->session()),
		ActiveResolved(&peer->session()),
		IdOf(peer),
		KindOf(peer));
}

bool StoryShown(not_null<const PeerData*> peer, bool hasUnseen) {
	if (!Filtering(&peer->session())) {
		return true;
	}
	const auto &resolved = ActiveResolved(&peer->session());

	// Your own row at the head of the strip is the "add a story" button, and it
	// answers to `hide_add_story_p' alone - not to `stories', not to a list and
	// not to a folder. It is a door to posting rather than a chat the preset is
	// judging, and deciding it the usual way meant it came and went with
	// whether some list happened to name Saved Messages, which has nothing to
	// do with it.
	//
	// Answered here rather than from an AddStoryShown() of its own so that the
	// strip keeps the single call site it has: this is still "does this peer
	// belong on the strip", and a caller that had to remember to ask a second
	// function for one peer is a caller that will one day forget. The branch
	// sits above the `stories' test on purpose, so None does not swallow it,
	// and above the peek test because a peek putting the button back is the
	// clients' rule rather than the flag's.
	if (peer->isSelf()) {
		return resolved.peeking || !resolved.hideAddStory;
	}

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

bool Peeking(not_null<Main::Session*> session) {
	return ActiveResolved(session).peeking;
}

bool HideEverywhere(not_null<Main::Session*> session) {
	const auto &resolved = ActiveResolved(session);
	return !resolved.normal && !resolved.peeking && resolved.hideEverywhere;
}

bool FoldersRestricted(not_null<Main::Session*> session) {
	const auto &resolved = ActiveResolved(session);
	if (resolved.normal) { return false; }
	if (!resolved.views.empty()) { return true; }
	if (resolved.peeking) { return false; }
	const auto &folders = resolved.folders;
	return folders.size() != 1 || !IsAllFolders(folders.front())
		|| folders.front().show.has_value() || folders.front().notify.has_value()
		|| folders.front().include.has_value();
}

int RecentStaySeconds(not_null<Main::Session*> session) {
	return Filtering(session) ? ActiveSettings().recent.staySecondsAfterClose : 0;
}

RecentStyle RecentMarkStyle(not_null<Main::Session*> session) {
	return Filtering(session) ? ActiveSettings().recent.style : RecentStyle::None;
}

const std::vector<ResolvedView> &ExtraViews(not_null<Main::Session*> session) {
	return ActiveResolved(session).views;
}

const std::vector<PeerIdValue> &PresetPins(not_null<Main::Session*> session) {
	return ActiveResolved(session).pinned;
}

const std::vector<PeerIdValue> &ExtraViewPins(not_null<Main::Session*> session, int index) {
	static const auto none = std::vector<PeerIdValue>();
	const auto &views = ExtraViews(session);
	return index >= 0 && index < int(views.size()) ? views[index].pinned : none;
}

const std::vector<PresetFolder> &ShownFolders(not_null<Main::Session*> session) {
	static const auto all = std::vector<PresetFolder>{ PresetFolder{ .name = AllFoldersName() } };
	const auto &resolved = ActiveResolved(session);
	return resolved.peeking ? all : resolved.folders;
}

const std::vector<ExemptFolder> &ExemptFolders(not_null<Main::Session*> session) {
	return ActiveResolved(session).exemptFolders;
}

const std::vector<QString> &SilencedFolders(not_null<Main::Session*> session) {
	return ActiveResolved(session).silencedFolders;
}

const std::vector<QString> &QuietFolders(not_null<Main::Session*> session) {
	return ActiveResolved(session).quietFolders;
}

} // namespace Purple
