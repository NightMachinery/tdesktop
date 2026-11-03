/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_playback.h"

#include "purple/purple_media_rules.h"

#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"

namespace Purple {

bool RestartOnDoubleClick(
		const std::shared_ptr<ClickHandler> &active,
		Qt::MouseButton button,
		bool selecting) {
	using Type = AudioMsgId::Type;
	const auto open = dynamic_cast<DocumentOpenClickHandler*>(active.get());
	const auto document = open ? open->document().get() : nullptr;
	const auto context = open ? open->context() : FullMsgId();
	const auto item = document ? document->owner().message(context) : nullptr;
	const auto media = item ? item->media() : nullptr;
	const auto player = Media::Player::instance();
	const auto current = player->current(Type::Voice);
	const auto state = player->getState(Type::Voice);
	const auto facts = MediaRules::RestartFacts{
		.leftButton = (button == Qt::LeftButton),
		.selecting = selecting,
		.documentOpen = (open != nullptr),
		.voiceOrRound = document
			&& (document->isVoiceMessage() || document->isVideoMessage()),
		.viewOnce = media && (media->ttlSeconds() > 0),
		.currentTrack = document
			&& (current.audio() == document)
			&& (current.contextId() == context),
		.seekable = (state.id == current)
			&& !Media::Player::IsStoppedOrStopping(state.state)
			&& (state.length > 0),
	};
	if (!MediaRules::RestartsOnDoubleClick(facts)) {
		return false;
	}
	player->finishSeeking(Type::Voice, 0.);
	if (Media::Player::IsPausedOrPausing(player->getState(Type::Voice).state)) {
		player->play(Type::Voice);
	}
	return true;
}

} // namespace Purple
