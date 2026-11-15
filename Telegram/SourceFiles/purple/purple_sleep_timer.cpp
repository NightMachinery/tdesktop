#include "purple/purple_sleep_timer.h"
#include "purple/hooks/playlist_player.h"
#include "purple/purple_playlist_player.h"

#include "base/qt_signal_producer.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/shortcuts.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_player.h"
#include "ui/toast/toast.h"
#include "rpl/deferred.h"

#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <array>
#include <map>

namespace Purple {
namespace {

namespace Rules = SleepTimerRules;
using Type = AudioMsgId::Type;

struct Video {
	Rules::Media media;
	Rules::SeekIntent seek;
	QPointer<QObject> lifetime;
	Fn<void()> clearResume;
	Fn<void()> pauseOnly;
};

struct AudioSeek {
	AudioMsgId id;
	Rules::SeekIntent intent;
};

class Controller final : public base::has_weak_ptr {
public:
	Controller();
	[[nodiscard]] SleepTimerSnapshot snapshot() const;
	[[nodiscard]] std::vector<Rules::Media> media() const;
	void changed();
	void check();
	void reconcile();
	void deferReconcile();
	void expire(Fn<void()> beforePause = nullptr, bool wasPlaying = false);
	void notice(QString text);
	void saveChoice();

	Rules::State state;
	std::map<const void*, Video> videos;
	std::array<AudioSeek, 2> audioSeeks;
	crl::time lastDuration = 30 * Rules::kMinute;
	crl::time lastCustom = 45 * Rules::kMinute;
	bool lastEnd = false;
	bool stoppedSong = true;
	bool stoppedVoice = true;
	bool songWasPlaying = false;
	bool voiceWasPlaying = false;
	bool reconciling = false;
	bool deferred = false;
	QString pendingNotice;
	rpl::event_stream<SleepTimerSnapshot> changes;
	rpl::lifetime lifetime;
	base::Timer timer;

};

std::unique_ptr<Controller> Service;

Rules::Item ItemFor(const AudioMsgId &id) {
	const auto document = id.audio();
	return {
		.kind = id.type() == Type::Voice ? Rules::Kind::Voice : Rules::Kind::Song,
		.session = document ? uint64_t(reinterpret_cast<uintptr_t>(&document->session())) : 0,
		.document = document ? document->id : 0,
		.peer = id.contextId().peer.value,
		.message = id.contextId().msg.bare,
	};
}

void ShowNotice(const QString &text) {
	if (const auto active = QApplication::activeWindow()) Ui::Toast::Show(active, text);
	else Ui::Toast::Show(text);
}

Controller::Controller()
: timer([=] { check(); }) {
	const auto weak = base::make_weak(this);
	const auto stored = Core::App().settings().readPref<QByteArray>("purple-sleep-timer-choice");
	const auto parts = stored.split(':');
	if (parts.size() == 3) {
		const auto duration = parts[1].toLongLong();
		const auto custom = parts[2].toLongLong();
		lastEnd = parts[0] == "end";
		if (Rules::ValidDuration(duration)) lastDuration = duration;
		if (Rules::ValidDuration(custom)) lastCustom = custom;
	}
	const auto player = Media::Player::instance();
	player->trackChanged() | rpl::on_next([=](Type type) {
		RefreshSleepTimerAudioSeek(type, player->current(type));
		Rules::DropAdvance(state, type == Type::Song ? Rules::Kind::Song : Rules::Kind::Voice);
		if (type == Type::Song) stoppedSong = false;
		if (type == Type::Voice) stoppedVoice = false;
		deferReconcile();
	}, lifetime);
	for (const auto type : { Type::Song, Type::Voice }) {
		player->startsPlay(type) | rpl::on_next([=] {
			if (type == Type::Song) stoppedSong = false;
			else stoppedVoice = false;
			deferReconcile();
		}, lifetime);
		player->stops(type) | rpl::on_next([=] {
			CancelSleepTimerAudioSeek(type);
			if (type == Type::Song) stoppedSong = true;
			else stoppedVoice = true;
			Rules::DropAdvance(state, type == Type::Song ? Rules::Kind::Song : Rules::Kind::Voice);
			reconcile();
		}, lifetime);
	}
	player->updatedNotifier() | rpl::on_next([=](const Media::Player::TrackState &update) {
		if (update.state != Media::Player::State::StoppedAtEnd) {
			const auto playing = !Media::Player::IsPausedOrPausing(update.state)
				&& !Media::Player::IsStoppedOrStopping(update.state);
			if (update.id.type() == Type::Song) songWasPlaying = playing;
			if (update.id.type() == Type::Voice) voiceWasPlaying = playing;
		}
		deferReconcile();
	}, lifetime);
	player->tracksFinished() | rpl::on_next([=](Type type) {
		if (type == Type::Voice) stoppedVoice = true;
		deferReconcile();
	}, lifetime);
	Core::App().systemSleepEvents() | rpl::on_next([=] {
		crl::on_main([weak] {
			if (const auto self = weak.get()) self->check();
		});
	}, lifetime);
	base::qt_signal_producer(qGuiApp, &QGuiApplication::applicationStateChanged)
		| rpl::on_next([=](Qt::ApplicationState active) {
			if (active == Qt::ApplicationActive && !pendingNotice.isEmpty()) {
				ShowNotice(base::take(pendingNotice));
			}
		}, lifetime);
	Shortcuts::Requests() | rpl::on_next([](not_null<Shortcuts::Request*> request) {
		request->check(Shortcuts::Command::MediaSleepTimer) && request->handle([] {
			ToggleSleepTimerShortcut();
			return true;
		});
	}, lifetime);
}

SleepTimerSnapshot Controller::snapshot() const {
	return { state, Rules::Remaining(state, crl::now()), lastDuration, lastCustom, lastEnd };
}

std::vector<Rules::Media> Controller::media() const {
	auto result = std::vector<Rules::Media>();
	const auto player = Media::Player::instance();
	for (const auto type : { Type::Song, Type::Voice }) {
		const auto id = player->current(type);
		const auto status = player->getState(type);
		const auto stopped = type == Type::Song ? stoppedSong : stoppedVoice;
		if (!id.audio() || stopped
			|| status.state == Media::Player::State::Stopped
			|| status.state == Media::Player::State::StoppedAtStart
			|| status.state == Media::Player::State::StoppedAtError
			|| status.state == Media::Player::State::Stopping) continue;
		result.push_back({
			.item = ItemFor(id),
			.owner = type == Type::Song ? Rules::kSongOwner : Rules::kVoiceOwner,
			.loaded = true,
			.eligible = status.state != Media::Player::State::StoppedAtEnd,
			.playing = !Media::Player::IsPausedOrPausing(status.state)
				&& !Media::Player::IsStoppedOrStopping(status.state),
		});
	}
	for (const auto &[key, video] : videos) {
		if (video.lifetime) result.push_back(video.media);
	}
	return result;
}

void Controller::notice(QString text) {
	if (QGuiApplication::applicationState() == Qt::ApplicationActive) {
		ShowNotice(text);
	} else {
		pendingNotice = std::move(text);
	}
}

void Controller::saveChoice() {
	Core::App().settings().writePref<QByteArray>("purple-sleep-timer-choice",
		QByteArray(lastEnd ? "end:" : "timed:")
			+ QByteArray::number(qlonglong(lastDuration)) + ':' + QByteArray::number(qlonglong(lastCustom)));
}

void Controller::changed() {
	timer.cancel();
	changes.fire_copy(snapshot());
	if (state.mode == Rules::Mode::Timed) {
		const auto remaining = Rules::Remaining(state, crl::now());
		const auto unit = remaining <= Rules::kMinute
			? crl::time(1000)
			: remaining % Rules::kMinute ? remaining % Rules::kMinute : Rules::kMinute;
		timer.callOnce(std::max(crl::time(1), std::min(remaining, unit)));
	}
}

void Controller::check() {
	if (Rules::Expire(state, crl::now())) expire();
	else changed();
}

void Controller::reconcile() {
	if (reconciling) return;
	const auto generation = state.generation;
	const auto loss = Rules::Reconcile(state, media());
	if (loss == Rules::Loss::Notice) notice(tr::lng_purple_sleep_off(tr::now));
	if (state.generation != generation) changed();
}

void Controller::deferReconcile() {
	if (deferred) return;
	deferred = true;
	const auto weak = base::make_weak(this);
	crl::on_main([weak] {
		if (const auto self = weak.get()) {
			self->deferred = false;
			self->reconcile();
		}
	});
}

void Controller::expire(Fn<void()> beforePause, bool wasPlaying) {
	CancelPlaylistStartsForSleepTimer();
	const auto playback = PlaylistPlayerCallGuard(Type::Song);
	const auto facts = media();
	const auto audible = wasPlaying
		|| std::ranges::any_of(facts, [](const auto &entry) { return entry.playing; });
	reconciling = true;
	if (beforePause) beforePause();
	const auto player = Media::Player::instance();
	auto clears = std::vector<Fn<void()>>();
	auto pauses = std::vector<Fn<void()>>();
	for (const auto &[key, video] : videos) {
		if (video.lifetime) {
			clears.push_back(video.clearResume);
			pauses.push_back(video.pauseOnly);
		}
	}
	Rules::PauseForExpiry({
		.clearAudioResumes = [=] { player->clearSleepTimerResumes(); },
		.clearVideoResumes = [&] { for (const auto &clear : clears) clear(); },
		.pauseVoice = [=] { player->pause(Type::Voice); },
		.pauseSong = [=] {
			const auto internal = PlaylistPlayerInternalCallGuard(
				Type::Song, PlaylistPlayerIntent::Pause);
			player->pause(Type::Song);
		},
		.pauseVideos = [&] { for (const auto &pause : pauses) pause(); },
	});
	reconciling = false;
	changed();
	if (audible) notice(tr::lng_purple_sleep_expired(tr::now));
}

} // namespace

void StartSleepTimerService() {
	if (!Service) Service = std::make_unique<Controller>();
}

void FinishSleepTimerService() {
	Service = nullptr;
}

SleepTimerSnapshot CurrentSleepTimer() {
	return Service ? Service->snapshot() : SleepTimerSnapshot();
}

rpl::producer<SleepTimerSnapshot> SleepTimerValue() {
	return rpl::deferred([]() -> rpl::producer<SleepTimerSnapshot> {
		if (Service) {
			return rpl::single(Service->snapshot()) | rpl::then(Service->changes.events());
		}
		return rpl::single(SleepTimerSnapshot());
	});
}

std::optional<Rules::Media> SleepTimerEndItem(std::optional<uint64_t> owner) {
	if (!Service) return std::nullopt;
	auto facts = Service->media();
	if (owner) std::erase_if(facts, [&](const auto &entry) { return entry.owner != *owner; });
	return Rules::ChooseEnd(facts);
}

bool StartSleepTimer(crl::time duration, bool custom) {
	if (!Service) return false;
	const auto loaded = std::ranges::any_of(Service->media(), [](const auto &entry) { return entry.loaded; });
	if (!Rules::StartTimed(Service->state, crl::now(), duration, loaded)) {
		Service->notice(tr::lng_purple_sleep_nothing(tr::now));
		return false;
	}
	Service->lastDuration = duration;
	Service->lastEnd = false;
	if (custom) Service->lastCustom = duration;
	Service->saveChoice();
	Service->changed();
	Service->notice(tr::lng_purple_sleep_started(tr::now, lt_count, duration / Rules::kMinute));
	return true;
}

bool StartSleepTimerEnd(std::optional<uint64_t> owner) {
	if (!Service) return false;
	if (!Rules::StartEnd(Service->state, SleepTimerEndItem(owner))) {
		Service->notice(tr::lng_purple_sleep_nothing(tr::now));
		return false;
	}
	Service->lastEnd = true;
	Service->saveChoice();
	Service->changed();
	Service->notice(tr::lng_purple_sleep_after_started(tr::now));
	return true;
}

void AddSleepTimerTime(crl::time duration) {
	if (!Service || !Rules::AddTime(Service->state, crl::now(), duration)) return;
	Service->changed();
	Service->notice(tr::lng_purple_sleep_left(tr::now, lt_count,
		(Rules::Remaining(Service->state, crl::now()) + Rules::kMinute - 1) / Rules::kMinute));
}

void CancelSleepTimer() {
	if (!Service || Service->state.mode == Rules::Mode::Off) return;
	Rules::Cancel(Service->state);
	Service->changed();
	Service->notice(tr::lng_purple_sleep_off(tr::now));
}

void ToggleSleepTimerShortcut() {
	if (!Service) return;
	if (Service->state.mode != Rules::Mode::Off) CancelSleepTimer();
	else if (Service->lastEnd) StartSleepTimerEnd();
	else StartSleepTimer(Service->lastDuration);
}

bool FinishSleepTimerItem(const AudioMsgId &id, Fn<void()> beforePause) {
	if (!Service || !Rules::Finish(Service->state, ItemFor(id))) return false;
	Service->expire(std::move(beforePause),
		id.type() == Type::Song ? Service->songWasPlaying : Service->voiceWasPlaying);
	return true;
}

bool TakeSleepTimerAdvance(const AudioMsgId &id) {
	return Service && Rules::TakeAdvance(Service->state, ItemFor(id));
}

bool HandleSleepTimerAdvance(const AudioMsgId &id, Fn<bool()> advance) {
	if (!TakeSleepTimerAdvance(id)) return false;
	const auto handled = MoveInAccountPlaylist(id.type(), id, 1, false);
	return handled.has_value() || advance();
}

void DropSleepTimerAdvance(Type type) {
	if (Service) Rules::DropAdvance(Service->state,
		type == Type::Song ? Rules::Kind::Song : Rules::Kind::Voice);
}

namespace {

bool SameAudioSeekItem(const AudioMsgId &a, const AudioMsgId &b) {
	// A seek restart replaces external play ids without changing its item.
	return a.audio() && a.audio() == b.audio() && a.contextId() == b.contextId();
}

AudioSeek *AudioSeekFor(Type type) {
	return Service && (type == Type::Song || type == Type::Voice)
		? &Service->audioSeeks[type == Type::Song ? 0 : 1] : nullptr;
}

Video *VideoSeekFor(const void *surface) {
	if (!Service) return nullptr;
	const auto i = Service->videos.find(surface);
	return i != end(Service->videos) && i->second.lifetime ? &i->second : nullptr;
}

} // namespace

void BeginSleepTimerAudioSeek(const AudioMsgId &id, bool paused) {
	if (const auto slot = AudioSeekFor(id.type())) {
		Rules::BeginAudioSeek(slot->intent, paused, SameAudioSeekItem(slot->id, id));
		slot->id = id;
	}
}

void PauseSleepTimerAudioSeek(const AudioMsgId &id) {
	const auto slot = AudioSeekFor(id.type());
	if (slot && SameAudioSeekItem(slot->id, id)) {
		Rules::PauseSeek(slot->intent);
	}
}

void SleepTimerAudioPlaybackIntent(const AudioMsgId &id, bool pause) {
	if (const auto slot = AudioSeekFor(id.type())) {
		if (SameAudioSeekItem(slot->id, id)) Rules::SetSeekPlaybackIntent(slot->intent, pause);
		else *slot = {};
	}
}

void PreserveSleepTimerAudioSeek(const AudioMsgId &id, const AudioMsgId &seeking) {
	const auto slot = AudioSeekFor(id.type());
	if (slot && slot->intent.pending && SameAudioSeekItem(slot->id, id)
		&& SameAudioSeekItem(seeking, id)) {
		Rules::ExpireSeek(slot->intent);
	}
}

bool FinishSleepTimerAudioSeek(
		const AudioMsgId &id, const AudioMsgId &seeking, bool paused) {
	if (const auto slot = AudioSeekFor(id.type())) {
		const auto held = SameAudioSeekItem(slot->id, id) && SameAudioSeekItem(seeking, id);
		const auto result = Rules::FinishSeek(slot->intent, held, paused);
		*slot = {};
		return result;
	}
	return paused;
}

void CancelSleepTimerAudioSeek(Type type) {
	if (const auto slot = AudioSeekFor(type)) *slot = {};
}

void RefreshSleepTimerAudioSeek(Type type, const AudioMsgId &id) {
	if (const auto slot = AudioSeekFor(type)) {
		if (SameAudioSeekItem(slot->id, id)) slot->id = id;
		else *slot = {};
	}
}

void BeginSleepTimerVideoSeek(const void *surface, bool paused) {
	if (const auto video = VideoSeekFor(surface)) {
		Rules::BeginVideoSeek(video->seek, paused);
	}
}

bool FinishSleepTimerVideoSeek(
		const void *surface, bool paused, bool finished, bool pausedBySeek) {
	if (const auto video = VideoSeekFor(surface)) {
		return Rules::FinishSeek(video->seek, true, paused && !finished);
	}
	return !pausedBySeek && !finished;
}

void SyncSleepTimerVideoIntent(
		const void *surface, bool paused, bool active, bool finished,
		bool &restartPaused, bool &pausedBySeek) {
	const auto video = VideoSeekFor(surface);
	if (!video) return;
	Rules::ToggleVideoSeekPlaybackIntent(video->seek, paused, active, finished,
		restartPaused, pausedBySeek);
}

void SetSleepTimerVideoPlaybackIntent(
		const void *surface, bool pause, bool &restartPaused, bool &pausedBySeek) {
	const auto video = VideoSeekFor(surface);
	if (!video) return;
	Rules::SetVideoSeekPlaybackIntent(video->seek, pause, restartPaused, pausedBySeek);
}

void PreserveSleepTimerVideoSeek(
		const void *surface, bool active, bool &restartPaused, bool &pausedBySeek) {
	auto fallback = Rules::SeekIntent();
	const auto video = VideoSeekFor(surface);
	Rules::PreserveVideoSeek(video ? video->seek : fallback, active,
		restartPaused, pausedBySeek);
}

void ObserveSleepTimerVideo(
		const void *surface,
		uint64_t owner,
		not_null<QObject*> lifetime,
		DocumentData *document,
		HistoryItem *message,
		Media::Streaming::Instance *stream,
		bool excluded,
		bool sound,
		Fn<void()> clearResume,
		Fn<void()> pauseOnly) {
	if (!Service) return;
	if (!document || !stream || excluded
		|| (!document->isVideoFile() && !document->isVideoMessage())) {
		ForgetSleepTimerVideo(surface);
		return;
	}
	const auto id = message ? message->fullId() : FullMsgId();
	auto item = Rules::Item{
		.kind = Rules::Kind::Video,
		.session = uint64_t(reinterpret_cast<uintptr_t>(&document->session())),
		.document = document->id,
		.peer = id.peer.value,
		.message = id.msg.bare,
	};
	const auto streamId = uint64_t(reinterpret_cast<uintptr_t>(stream->shared().get()));
	const auto previous = Service->videos.find(surface);
	if (!message && previous != end(Service->videos)
		&& previous->second.media.stream == streamId
		&& previous->second.media.item.session == item.session
		&& previous->second.media.item.document == item.document) {
		item.peer = previous->second.media.item.peer;
		item.message = previous->second.media.item.message;
	}
	if (stream->player().finished() && Rules::Finish(Service->state, item, streamId)) {
		Service->expire();
	}
	const auto current = Service->videos.find(surface);
	const auto seek = current != end(Service->videos)
		&& current->second.media.stream == streamId
		&& current->second.media.item == item
		? current->second.seek : Rules::SeekIntent();
	const auto guard = QPointer<QObject>(lifetime.get());
	Service->videos[surface] = {
		.media = { .item = item, .owner = owner,
			.stream = streamId,
			.loaded = !stream->failed(),
			.eligible = !stream->failed() && sound && !document->isAnimation() && !stream->player().finished(),
			.playing = stream->active() && !stream->paused() && !stream->player().finished() },
		.seek = seek,
		.lifetime = guard,
		.clearResume = [=] { if (guard) clearResume(); },
		.pauseOnly = [=] { if (guard) pauseOnly(); },
	};
	Service->deferReconcile();
}

void ForgetSleepTimerVideo(const void *surface, bool immediate) {
	if (Service && Service->videos.erase(surface)) {
		if (immediate) Service->reconcile();
		else Service->deferReconcile();
	}
}

bool SleepTimerVideoLoaded(const void *surface) {
	if (!Service) return false;
	const auto i = Service->videos.find(surface);
	return i != end(Service->videos) && i->second.lifetime && i->second.media.loaded;
}

} // namespace Purple
