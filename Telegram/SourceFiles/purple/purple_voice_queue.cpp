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
#include "media/audio/media_audio.h"
#include "settings/settings_builder.h"
#include "ui/widgets/buttons.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

using Type = AudioMsgId::Type;

struct QueueAnchor {
	base::weak_ptr<Main::Session> session;
	PeerId peer = 0;
	MsgId topicRootId = 0;
	PeerId monoforumPeerId = 0;
};

struct AutomaticIntent {
	AudioMsgId audioId;
	std::uint64_t generation = 0;
	std::uint64_t serial = 0;
};

QueueAnchor Anchor;
VoiceQueueRules::Continuation Continuation;
base::weak_ptr<Main::Session> Session;
bool SessionRequired = false;
std::uint64_t Generation = 0;
std::uint64_t Interaction = 0;
std::uint64_t AutomaticSerial = 0;
std::optional<AutomaticIntent> Automatic;
std::shared_ptr<int> VoiceStream;
bool Starting = false;
bool Finished = false;
struct MusicPauseClaim {
	Fn<void()> cancel;
	std::shared_ptr<MusicPauseClaim> previous;
};

std::shared_ptr<MusicPauseClaim> MusicPause;

[[nodiscard]] PlaybackChat ChatOf(not_null<PeerData*> peer) {
	return VoiceQueueRules::PlaybackChatFor({
		.self = peer->isSelf(),
		.user = peer->isUser(),
		.replies = peer->isRepliesChat(),
		.verifyCodes = peer->isVerifyCodes(),
	});
}

[[nodiscard]] QueueAnchor AnchorFor(
		const AudioMsgId &played,
		const Media::Player::PlaylistContext *context) {
	const auto document = played.audio();
	const auto item = (document && played.contextId())
		? document->owner().message(played.contextId())
		: nullptr;
	if (!item) return {};
	const auto history = item->history()->migrateToOrMe();
	const auto peer = history->peer;
	const auto sameHistory = item->history() == history;
	const auto topic = context
		? context->topicRootId
		: (sameHistory && peer->isForum())
		? item->topicRootId()
		: MsgId();
	const auto monoforum = context
		? context->monoforumPeerId
		: (!topic && sameHistory && peer->amMonoforumAdmin())
		? item->sublistPeerId()
		: PeerId();
	return {
		.session = base::make_weak(&history->session()),
		.peer = peer->id,
		.topicRootId = topic,
		.monoforumPeerId = monoforum,
	};
}

[[nodiscard]] bool SessionAlive() {
	return !SessionRequired || Session.get();
}

[[nodiscard]] QString ContinuationName(VoiceContinuation mode) {
	switch (mode) {
	case VoiceContinuation::ResumeMusic:
		return tr::lng_purple_after_voice_music(tr::now);
	case VoiceContinuation::Unheard:
		return tr::lng_purple_after_voice_unheard(tr::now);
	case VoiceContinuation::AllFollowing:
		return tr::lng_purple_after_voice_all(tr::now);
	}
	Unexpected("Invalid voice continuation mode.");
}

void ChooseContinuationBox(not_null<Ui::GenericBox*> box) {
	SingleChoiceBox(box, {
		.title = tr::lng_purple_after_voice(),
		.options = {
			ContinuationName(VoiceContinuation::ResumeMusic),
			ContinuationName(VoiceContinuation::Unheard),
			ContinuationName(VoiceContinuation::AllFollowing),
		},
		.initialSelection = int(ActiveSettings().playback.afterVoice),
		.callback = [](int index) {
			const auto value = (index == 0)
				? u"resume_music"_q
				: (index == 1)
				? u"unheard"_q
				: u"all_following"_q;
			WriteSettings([=](const QString &text) {
				return SetTableString(text, SettingsFilePath(),
					u"playback"_q, u"after_voice"_q, value);
			}, u"the voice continuation chooser"_q);
		},
	});
}

void SetResumeMusic(bool value) {
	WriteSettings([=](const QString &text) {
		return SetTableBool(text, SettingsFilePath(), u"playback"_q,
			u"resume_music_after_voice_p"_q, value);
	}, u"the voice music resume toggle"_q);
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
	ClearVoiceMusicPause();
	Finished = false;
	RevokeVoicePlaybackContinuation();
	Anchor = {};
	Continuation.clear();
	Session = {};
	SessionRequired = false;
	VoiceStream.reset();
}

void FinishVoiceQueue() {
	Finished = true;
	RevokeVoicePlaybackContinuation();
	ClearVoiceMusicPause();
	RetireVoicePlaybackStream();
	Continuation.close();
}

bool VoicePlaybackAvailable() {
	return !Finished;
}

VoicePlaybackAdmission::VoicePlaybackAdmission(
		const AudioMsgId &audioId,
		const Media::Player::PlaylistContext *context) {
	if (audioId.type() != Type::Voice || !audioId.audio()) return;
	if (Finished) {
		_commit = [](bool) { return false; };
		return;
	}
	const auto automatic = Automatic
		&& Automatic->generation == Generation
		&& Automatic->audioId == audioId;
	const auto previousSession = Session;
	const auto previousRequired = SessionRequired;
	const auto committed = std::make_shared<bool>(false);
	RevokeVoicePlaybackContinuation();
	Session = base::make_weak(&audioId.audio()->session());
	SessionRequired = true;
	Starting = true;
	const auto current = VoicePlaybackContinuationGuard();
	const auto generation = Generation;
	const auto anchor = AnchorFor(audioId, context);
	const auto item = audioId.contextId()
		? audioId.audio()->owner().message(audioId.contextId())
		: nullptr;
	const auto chat = item ? ChatOf(item->history()->peer) : PlaybackChat::Other;
	const auto outgoing = item && item->out();
	const auto settings = ActiveSettings().playback;
	_commit = [=](bool interruptedMusic) {
		if (!current()) return false;
		*committed = true;
		if (!automatic) {
			auto effective = settings;
			if (!interruptedMusic) {
				effective.afterVoice = VoiceContinuation::AllFollowing;
			}
			Continuation.begin(effective, chat, outgoing);
			Anchor = anchor;
		}
		return true;
	};
	_finish = [=] {
		if (Generation == generation) {
			Starting = false;
			if (!*committed) {
				Session = previousSession;
				SessionRequired = previousRequired;
			}
		}
	};
}

VoicePlaybackAdmission::~VoicePlaybackAdmission() {
	if (_finish) _finish();
}

bool VoicePlaybackAdmission::commit(bool interruptedMusic) const {
	return !_commit || _commit(interruptedMusic);
}

VoiceMusicPauseGuard::VoiceMusicPauseGuard(
		bool &resumeOnVoiceEnd,
		Fn<bool()> current) {
	const auto claim = std::make_shared<MusicPauseClaim>();
	claim->previous = MusicPause;
	claim->cancel = [current, &resumeOnVoiceEnd] {
		if (current()) resumeOnVoiceEnd = false;
	};
	resumeOnVoiceEnd = true;
	MusicPause = claim;
	_finish = [=] {
		if (MusicPause == claim) MusicPause = claim->previous;
	};
}

VoiceMusicPauseGuard::~VoiceMusicPauseGuard() {
	if (_finish) _finish();
}

bool VoiceMusicInterrupted(bool resumeOnVoiceEnd) {
	if (resumeOnVoiceEnd) return true;
	const auto state = Media::Player::instance()->getState(Type::Song);
	return state.id
		&& !Media::Player::IsStoppedOrStopping(state.state)
		&& !Media::Player::IsPausedOrPausing(state.state);
}

void ClearVoiceMusicPause() {
	for (auto claim = std::exchange(MusicPause, nullptr)
		; claim
		; claim = claim->previous) {
		claim->cancel();
	}
}

VoiceAutomaticPlayGuard::VoiceAutomaticPlayGuard(
		const AudioMsgId &audioId,
		bool automatic) {
	if (!automatic || audioId.type() != Type::Voice) return;
	const auto serial = ++AutomaticSerial;
	Automatic = AutomaticIntent{ audioId, Generation, serial };
	_finish = [=] {
		if (Automatic && Automatic->serial == serial) Automatic.reset();
	};
}

VoiceAutomaticPlayGuard::~VoiceAutomaticPlayGuard() {
	if (_finish) _finish();
}

void RevokeVoicePlaybackContinuation() {
	++Generation;
	++Interaction;
	Automatic.reset();
	Starting = false;
}

void NoteVoicePlaybackInteraction() {
	++Interaction;
	Automatic.reset();
}

Fn<bool()> VoiceQueueTransitionGuard() {
	const auto current = VoicePlaybackContinuationGuard();
	const auto interaction = Interaction;
	return [=] { return current() && Interaction == interaction; };
}

void StopVoicePlaybackContinuation() {
	RevokeVoicePlaybackContinuation();
	Continuation.close();
}

Fn<bool()> VoicePlaybackContinuationGuard() {
	const auto generation = Generation;
	const auto session = Session;
	const auto required = SessionRequired;
	return [=] {
		return !Finished && Generation == generation && (!required || session.get());
	};
}

Fn<bool()> NewVoicePlaybackStreamGuard() {
	if (Finished) return [] { return false; };
	VoiceStream = std::make_shared<int>(0);
	Starting = false;
	const auto weak = std::weak_ptr<int>(VoiceStream);
	const auto session = Session;
	const auto required = SessionRequired;
	return [=] {
		return !Starting && !weak.expired() && (!required || session.get());
	};
}

void RetireVoicePlaybackStream() {
	VoiceStream.reset();
}

bool VoiceQueueMayAdvance(Type type, bool automatic) {
	return type != Type::Voice || !automatic
		|| (!Finished && SessionAlive() && Continuation.permitsAdvance());
}

bool ResumeMusicAfterVoice() {
	return !Finished && SessionAlive() && Continuation.resumesMusic();
}

std::optional<HistoryItem*> ConfinedVoiceNext(
		AudioMsgId::Type type,
		bool autonext,
		const History *history,
		MsgId topicRootId,
		PeerId monoforumPeerId,
		int from,
		Fn<HistoryItem*(int)> itemAt) {
	if (type != Type::Voice || !autonext || !history) {
		return std::nullopt;
	}
	const auto session = Anchor.session.get();
	if (session != &history->session() || Anchor.peer != history->peer->id
		|| Anchor.topicRootId != topicRootId
		|| Anchor.monoforumPeerId != monoforumPeerId) {
		return std::nullopt;
	}
	using Candidate = VoiceQueueRules::Candidate;
	const auto index = Continuation.next(
		from,
		[&](int index) {
			const auto item = itemAt(index);
			return VoiceQueueRules::ContinuationCandidate{
				.direction = !item
					? Candidate::Missing
					: item->out()
					? Candidate::Outgoing
					: Candidate::Incoming,
				.unheard = item && item->isUnreadMedia(),
			};
		});
	if (!index) {
		return std::nullopt;
	}
	return (*index >= 0) ? itemAt(*index) : nullptr;
}

void AddVoiceQueueSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addButton({
		.id = u"advanced/purple_after_voice"_q,
		.title = tr::lng_purple_after_voice(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::single(rpl::empty) | rpl::then(SettingsChanges())
			| rpl::map([] { return ContinuationName(ActiveSettings().playback.afterVoice); }),
		.onClick = [=] {
			if (controller) controller->show(Box(ChooseContinuationBox));
		},
		.keywords = { u"purple"_q, u"voice"_q, u"music"_q, u"unheard"_q },
	});
	const auto resume = builder.addButton({
		.id = u"advanced/purple_resume_music_after_voice"_q,
		.title = tr::lng_purple_resume_music_after_voice(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(rpl::empty) | rpl::then(SettingsChanges())
			| rpl::map([] { return ActiveSettings().playback.resumeMusicAfterVoice; }),
		.keywords = { u"purple"_q, u"voice"_q, u"music"_q, u"resume"_q },
	});
	if (resume) {
		resume->toggledValue() | rpl::filter([](bool value) {
			return value != ActiveSettings().playback.resumeMusicAfterVoice;
		}) | rpl::on_next(SetResumeMusic, resume->lifetime());
	}
	builder.addDividerText(tr::lng_purple_after_voice_about());
	builder.addSkip();
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
