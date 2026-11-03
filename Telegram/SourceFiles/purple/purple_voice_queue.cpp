/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_voice_queue.h"

#include "purple/purple_config.h"
#include "purple/purple_voice_queue_rules.h"

#include "base/weak_ptr.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/player/media_player_instance.h"
#include "settings/settings_builder.h"
#include "ui/widgets/buttons.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

using Type = AudioMsgId::Type;

struct QueueAnchor {
	base::weak_ptr<Main::Session> session;
	PeerId peer = 0;
	VoiceQueueAnchor rule;
};

QueueAnchor Anchor;
VoiceQueueRules::PlayTracker Plays;
rpl::lifetime Lifetime;

[[nodiscard]] VoiceQueueRules::PlayKey KeyOf(FullMsgId id) {
	return { id.peer.value, id.msg.bare };
}

[[nodiscard]] PlaybackChat ChatOf(not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return PlaybackChat::Self;
	} else if (peer->isUser()
		&& !peer->isRepliesChat()
		&& !peer->isVerifyCodes()) {
		return PlaybackChat::OneToOne;
	}
	return PlaybackChat::Other;
}

void AnchorManualPlay(const AudioMsgId &played) {
	const auto document = played.audio();
	const auto item = (document && played.contextId())
		? document->owner().message(played.contextId())
		: nullptr;
	if (!item) {
		Anchor = QueueAnchor();
		return;
	}
	const auto history = item->history();
	Anchor = QueueAnchor{
		.session = base::make_weak(&history->session()),
		.peer = history->peer->id,
		.rule = AnchorForManualPlay(
			ActiveSettings().playback,
			ChatOf(history->peer),
			item->out()),
	};
}

[[nodiscard]] bool PrivateChatQueue() {
	return ActiveSettings().playback.privateChatQueue;
}

void SetPrivateChatQueue(bool value) {
	WriteSettings([=](const QString &text) {
		return SetTableBool(
			text,
			SettingsFilePath(),
			u"playback"_q,
			u"private_chat_queue_p"_q,
			value);
	}, u"the voice queue toggle"_q);
}

} // namespace

void StartVoiceQueue() {
	Lifetime.destroy();
	const auto player = Media::Player::instance();
	player->switchToNextEvents(
	) | rpl::filter([](const Media::Player::Instance::Switch &pair) {
		return (pair.from.type() == Type::Voice);
	}) | rpl::on_next([](const Media::Player::Instance::Switch &pair) {
		Plays.automaticNext(KeyOf(pair.to));
	}, Lifetime);
	player->trackChanged(
	) | rpl::filter([](Type type) {
		return (type == Type::Voice);
	}) | rpl::on_next([=] {
		const auto current = player->current(Type::Voice);
		if (Plays.manual(KeyOf(current.contextId()))) {
			AnchorManualPlay(current);
		}
	}, Lifetime);
}

std::optional<HistoryItem*> ConfinedVoiceNext(
		AudioMsgId::Type type,
		bool autonext,
		const History *history,
		int from,
		Fn<HistoryItem*(int)> itemAt) {
	if (type != Type::Voice || !autonext || !history) {
		return std::nullopt;
	}
	const auto session = Anchor.session.get();
	if (session != &history->session() || Anchor.peer != history->peer->id) {
		return std::nullopt;
	}
	using Candidate = VoiceQueueRules::Candidate;
	const auto index = VoiceQueueRules::ConfinedIndex(
		Anchor.rule,
		from,
		[&](int index) {
			const auto item = itemAt(index);
			return !item
				? Candidate::Missing
				: item->out()
				? Candidate::Outgoing
				: Candidate::Incoming;
		});
	if (!index) {
		return std::nullopt;
	}
	return (*index >= 0) ? itemAt(*index) : nullptr;
}

void AddVoiceQueueSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto toggle = builder.addButton({
		.id = u"advanced/purple_voice_queue_private"_q,
		.title = tr::lng_purple_voice_queue_private(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(rpl::empty) | rpl::then(
			SettingsChanges()
		) | rpl::map([] { return PrivateChatQueue(); }),
		.keywords = { u"purple"_q, u"voice"_q, u"queue"_q, u"autoplay"_q },
	});
	if (toggle) {
		toggle->toggledValue() | rpl::filter([](bool value) {
			return value != PrivateChatQueue();
		}) | rpl::on_next(SetPrivateChatQueue, toggle->lifetime());
	}
	builder.addDividerText(tr::lng_purple_voice_queue_private_about());
	builder.addSkip();
}

} // namespace Purple
