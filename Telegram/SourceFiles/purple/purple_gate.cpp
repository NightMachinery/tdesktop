/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_gate.h"

#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "purple/purple_config.h"

namespace Purple {
namespace {

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
	void refresh();

	Resolved _resolved = NormalResolution();
	bool _refreshing = false;
	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

Gate::Gate() {
	refresh();

	rpl::merge(
		SettingsChanges(),
		StateChanges()
	) | rpl::on_next([=] {
		refresh();
	}, _lifetime);
}

void Gate::refresh() {
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
	if (!next || *next == _resolved) {
		return;
	}
	_resolved = std::move(*next);
	LOG(("Purple: preset '%1'%2, %3 lists."
		).arg(_resolved.preset
		).arg(_resolved.normal ? u" (stock behaviour)"_q : QString()
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

// Deliberately never destroyed, for the same reason as the config singleton -
// it holds an rpl subscription to something with static storage duration.
[[nodiscard]] Gate &Instance() {
	static const auto result = new Gate();
	return *result;
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

const Resolved &ActiveResolved() {
	return Instance().resolved();
}

rpl::producer<> ActiveChanges() {
	return Instance().changes();
}

bool Filtering() {
	return !Instance().resolved().normal;
}

Visibility VisibleFor(not_null<const PeerData*> peer) {
	// Saved Messages is never hidden. It is where the user files things for
	// themselves, nothing arrives in it unbidden, and there is no route back to
	// it from a chat list that does not show it.
	if (peer->isSelf()) {
		return Visibility();
	}
	return Visible(
		ActiveSettings(),
		Instance().resolved(),
		IdOf(peer),
		KindOf(peer));
}

const std::optional<std::vector<PresetFolder>> &ShownFolders() {
	return Instance().resolved().folders;
}

bool SilencedByPreset(not_null<const PeerData*> peer) {
	return Filtering() && !VisibleFor(peer).notify;
}

bool FoldersRestricted() {
	const auto &resolved = Instance().resolved();
	return !resolved.normal && resolved.folders.has_value();
}

const EffectiveList *ListFor(not_null<const PeerData*> peer) {
	if (peer->isSelf()) {
		return nullptr;
	}
	return MatchList(
		ActiveSettings(),
		Instance().resolved(),
		IdOf(peer),
		KindOf(peer));
}

} // namespace Purple
