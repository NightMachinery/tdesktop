#pragma once

#include "data/data_audio_msg_id.h"
#include "purple/purple_sleep_timer_rules.h"

class QObject;
class DocumentData;
class HistoryItem;
namespace Media::Streaming { class Instance; }

namespace Purple {

struct SleepTimerSnapshot {
	SleepTimerRules::State state;
	crl::time remaining = 0;
	crl::time lastDuration = 30 * SleepTimerRules::kMinute;
	crl::time lastCustom = 45 * SleepTimerRules::kMinute;
	bool lastEnd = false;
};

void StartSleepTimerService();
void FinishSleepTimerService();
[[nodiscard]] SleepTimerSnapshot CurrentSleepTimer();
[[nodiscard]] rpl::producer<SleepTimerSnapshot> SleepTimerValue();
[[nodiscard]] std::optional<SleepTimerRules::Media> SleepTimerEndItem(
	std::optional<uint64_t> owner = std::nullopt);
bool StartSleepTimer(crl::time duration, bool custom = false);
bool StartSleepTimerEnd(std::optional<uint64_t> owner = std::nullopt);
void AddSleepTimerTime(crl::time duration);
void CancelSleepTimer();
void ToggleSleepTimerShortcut();
[[nodiscard]] bool FinishSleepTimerItem(const AudioMsgId &id, Fn<void()> beforePause);
[[nodiscard]] bool TakeSleepTimerAdvance(const AudioMsgId &id);
[[nodiscard]] bool HandleSleepTimerAdvance(const AudioMsgId &id, Fn<bool()> advance);
void DropSleepTimerAdvance(AudioMsgId::Type type);

void BeginSleepTimerAudioSeek(const AudioMsgId &id, bool paused);
void PauseSleepTimerAudioSeek(const AudioMsgId &id);
void SleepTimerAudioPlaybackIntent(const AudioMsgId &id, bool pause);
void PreserveSleepTimerAudioSeek(const AudioMsgId &id, const AudioMsgId &seeking);
[[nodiscard]] bool FinishSleepTimerAudioSeek(
	const AudioMsgId &id, const AudioMsgId &seeking, bool paused);
void CancelSleepTimerAudioSeek(AudioMsgId::Type type);
void RefreshSleepTimerAudioSeek(AudioMsgId::Type type, const AudioMsgId &id);
void BeginSleepTimerVideoSeek(const void *surface, bool paused);
[[nodiscard]] bool FinishSleepTimerVideoSeek(
	const void *surface, bool paused, bool finished, bool pausedBySeek);
void SyncSleepTimerVideoIntent(
	const void *surface, bool paused, bool active, bool finished,
	bool &restartPaused, bool &pausedBySeek);
void SetSleepTimerVideoPlaybackIntent(
	const void *surface, bool pause, bool &restartPaused, bool &pausedBySeek);
void PreserveSleepTimerVideoSeek(
	const void *surface, bool active, bool &restartPaused, bool &pausedBySeek);

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
	Fn<void()> pauseOnly);
void ForgetSleepTimerVideo(const void *surface, bool immediate = false);
[[nodiscard]] bool SleepTimerVideoLoaded(const void *surface);

} // namespace Purple
