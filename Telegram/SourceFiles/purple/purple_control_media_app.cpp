#include "purple/purple_control_media.h"

#include "purple/purple_gate.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_domain.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "storage/storage_domain.h"
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

namespace Purple {
namespace {

AudioMsgId::Type PlayerType(std::optional<ControlMediaType> type) {
	return !type ? Media::Player::instance()->getActiveType()
		: *type == ControlMediaType::Music ? AudioMsgId::Type::Song : AudioMsgId::Type::Voice;
}

ControlMediaSnapshot MediaSnapshot(std::optional<ControlMediaType> requested) {
	using namespace Media::Player;
	const auto player = instance();
	const auto type = PlayerType(requested);
	const auto current = player->current(type);
	const auto document = current.audio();
	auto result = ControlMediaSnapshot();
	if (!document) return result;
	result.type = type == AudioMsgId::Type::Song ? ControlMediaType::Music : ControlMediaType::Voice;
	const auto state = player->getState(type);
	result.state = IsStoppedOrStopping(state.state) ? ControlMediaState::Stopped
		: IsPausedOrPausing(state.state) ? ControlMediaState::Paused : ControlMediaState::Playing;
	result.position = ControlMediaMilliseconds(state.position, state.frequency);
	result.duration = ControlMediaMilliseconds(state.length, state.frequency);
	result.name = document->filename();
	result.path = document->filepath();
	result.mime = document->mimeString();
	const auto item = current.contextId() ? document->owner().message(current.contextId()) : nullptr;
	result.hidden = item ? item->history()->purpleHiddenFromView() : Filtering(&document->session());
	if (result.hidden) return result;
	if (const auto song = document->song()) {
		result.title = song->title;
		result.performer = song->performer;
	}
	if (item && result.type == ControlMediaType::Voice) {
		result.sender = item->from()->name();
		result.chat = item->history()->peer->name();
		result.date = item->date();
	}
	return result;
}

ControlResult ControlPlayback(ControlMediaAction action, std::optional<ControlMediaType> requested) {
	const auto player = Media::Player::instance();
	const auto type = PlayerType(requested);
	if (!player->current(type)) {
		return ControlError(u"not-found"_q, u"No current media item of this type."_q);
	}
	switch (action) {
	case ControlMediaAction::Toggle: player->playPause(type); break;
	case ControlMediaAction::Play: player->play(type); break;
	case ControlMediaAction::Pause: player->pause(type); break;
	}
	const auto state = player->getState(type).state;
	const auto text = Media::Player::IsStoppedOrStopping(state) ? u"stopped"_q
		: Media::Player::IsPausedOrPausing(state) ? u"paused"_q : u"playing"_q;
	return { QJsonObject{
		{ u"type"_q, type == AudioMsgId::Type::Song ? u"music"_q : u"voice"_q },
		{ u"state"_q, text },
	} };
}

void ReadMediaInfo(std::optional<ControlMediaType> type,
		std::function<bool()> active, ControlReply reply) {
	if (active && !active()) return;
	const auto candidate = MediaSnapshot(type).path;
	crl::async([=] {
		if (active && !active()) return;
		const auto path = ExistingControlMediaPath(candidate);
		crl::on_main([=] {
			if (active && !active()) return;
			if (Core::App().passcodeLocked()) {
				reply(ControlError(u"locked"_q, u"Media information is unavailable while locked."_q));
				return;
			}
			reply(ControlMediaInfo(MediaSnapshot(type), path));
		});
	});
}

ControlResult LockApplication() {
	return ControlLock(Core::App().domain().local().hasLocalPasscode(), [] {
		if (!Core::App().passcodeLocked()) Core::App().lockByPasscode();
		return Core::App().passcodeLocked();
	});
}

}

ControlMediaAdapter AppControlMediaAdapter() {
	return { ControlPlayback, ReadMediaInfo, LockApplication };
}

}
