/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_mute.h"

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "crl/crl_on_main.h"
#include "data/notify/data_notify_settings.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"

namespace Purple {
namespace {

struct MuteState {
	base::flat_set<PeerId> silencedByFolder;
};

struct MuteBypass {
	not_null<const Data::NotifySettings*> settings;
	const MuteBypass *previous = nullptr;
};

base::flat_map<Main::Session*, std::shared_ptr<MuteState>> States;
thread_local const MuteBypass *Bypass = nullptr;

[[nodiscard]] std::shared_ptr<MuteState> StateFor(
		not_null<Main::Session*> session) {
	const auto key = session.get();
	const auto existing = States.find(key);
	if (existing != States.end()) {
		return existing->second;
	}
	const auto state = std::make_shared<MuteState>();
	States.emplace(key, state);
	session->lifetime().add([=] {
		States.erase(key);
	});
	return state;
}

[[nodiscard]] bool Bypassing(not_null<const PeerData*> peer) {
	const auto settings = &peer->owner().notifySettings();
	for (auto frame = Bypass; frame; frame = frame->previous) {
		if (frame->settings == settings) {
			return true;
		}
	}
	return false;
}

template <typename PeerOrThread>
[[nodiscard]] bool WithoutPreset(PeerOrThread target) {
	const auto settings = &target->owner().notifySettings();
	const auto frame = MuteBypass{ settings, Bypass };
	Bypass = &frame;
	const auto guard = gsl::finally([&] {
		Bypass = frame.previous;
	});
	return settings->isMuted(target);
}

[[nodiscard]] bool SilencedByFolder(not_null<const PeerData*> peer) {
	const auto &silenced = SilencedFolders();
	if (silenced.empty()) {
		return false;
	}
	const auto &owner = peer->owner();
	const auto history = owner.historyLoaded(peer->id);
	if (!history) {
		return false;
	}
	for (const auto &filter : owner.chatsFilters().list()) {
		if (!filter.id()) {
			continue;
		}
		for (const auto &name : silenced) {
			if (!name.compare(filter.title().text.text, Qt::CaseInsensitive)
				&& filter.contains(history, false, true)) {
				return true;
			}
		}
	}
	return false;
}

}

bool PresetMutes(
		not_null<const PeerData*> peer,
		crl::time *changesIn,
		crl::time maxCheckDelay) {
	if (Bypassing(peer) || !Silenced(peer)) {
		return false;
	}
	if (changesIn) {
		*changesIn = maxCheckDelay;
	}
	return true;
}

bool MutedWithoutPreset(not_null<const Data::Thread*> thread) {
	return WithoutPreset(thread);
}

bool MutedWithoutPreset(not_null<const PeerData*> peer) {
	return WithoutPreset(peer);
}

bool Silenced(not_null<const PeerData*> peer) {
	if (!Filtering() || Peeking()) {
		return false;
	}
	if (const auto override = OverrideFor(peer)) {
		switch (*override) {
		case OverrideKind::Notify: return false;
		case OverrideKind::Hide: return true;
		case OverrideKind::Show: break;
		}
	}
	return !VisibleFor(peer).notify || SilencedByFolder(peer);
}

void RefreshMute(not_null<PeerData*> peer) {
	peer->owner().notifySettings().purpleRefreshMute(peer);
}

void RefreshFolderMute(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	if (SilencedFolders().empty()) {
		const auto existing = States.find(session);
		if (existing != States.end()) {
			existing->second->silencedByFolder.clear();
		}
		return;
	}
	const auto state = StateFor(session);
	const auto id = peer->id;
	const auto now = SilencedByFolder(peer);
	if (now == state->silencedByFolder.contains(id)) {
		return;
	} else if (now) {
		state->silencedByFolder.emplace(id);
	} else {
		state->silencedByFolder.remove(id);
	}
	crl::on_main(session, [=] {
		if (const auto found = session->data().peerLoaded(id)) {
			RefreshMute(found);
		}
	});
}

}
