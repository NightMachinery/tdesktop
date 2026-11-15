/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_audio_msg_id.h"

#include <QtCore/QSize>
#include <QtCore/qnamespace.h>

#include <memory>
#include <optional>

class ClickHandler;
class DocumentData;
class History;
class HistoryItem;
class QWidget;

namespace Media::Player {
class Float;
struct PlaylistContext;
} // namespace Media::Player

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Purple {

[[nodiscard]] bool RestartOnDoubleClick(
	not_null<QWidget*> widget,
	const std::shared_ptr<ClickHandler> &active,
	Qt::MouseButton button,
	bool selecting);

[[nodiscard]] QSize VideoMessageThumbSize(
	const DocumentData *document,
	const HistoryView::Element *parent,
	int &inOutWidthMax);
[[nodiscard]] int VideoMessageBubbleWidthLimit(
	const DocumentData *document,
	const HistoryView::Element *parent,
	int maxWidth,
	bool ephemeral);

[[nodiscard]] int FloatPlayerSize(
	const QWidget *parent,
	const DocumentData *document,
	int size);

[[nodiscard]] bool RestartFloatOnDoubleClick(const HistoryItem *item);
[[nodiscard]] bool KeepFloatVisible(const HistoryItem *item);
void DecorateFloat(
	not_null<Media::Player::Float*> floating,
	Fn<void(not_null<const HistoryItem*>)> open);

class VoicePlaybackAdmission final {
public:
	VoicePlaybackAdmission(
		const AudioMsgId &audioId,
		const Media::Player::PlaylistContext *context);
	~VoicePlaybackAdmission();
	VoicePlaybackAdmission(const VoicePlaybackAdmission&) = delete;
	VoicePlaybackAdmission &operator=(const VoicePlaybackAdmission&) = delete;
	[[nodiscard]] bool commit(bool interruptedMusic) const;

private:
	Fn<bool(bool)> _commit;
	Fn<void()> _finish;

};

class VoiceMusicPauseGuard final {
public:
	VoiceMusicPauseGuard(bool &resumeOnVoiceEnd, Fn<bool()> current);
	~VoiceMusicPauseGuard();
	VoiceMusicPauseGuard(const VoiceMusicPauseGuard&) = delete;
	VoiceMusicPauseGuard &operator=(const VoiceMusicPauseGuard&) = delete;

private:
	Fn<void()> _finish;

};

[[nodiscard]] bool VoiceMusicInterrupted(bool resumeOnVoiceEnd);
void ClearVoiceMusicPause();

class VoiceAutomaticPlayGuard final {
public:
	VoiceAutomaticPlayGuard(const AudioMsgId &audioId, bool automatic);
	~VoiceAutomaticPlayGuard();
	VoiceAutomaticPlayGuard(const VoiceAutomaticPlayGuard&) = delete;
	VoiceAutomaticPlayGuard &operator=(const VoiceAutomaticPlayGuard&) = delete;

private:
	Fn<void()> _finish;

};

void FinishVoiceQueue();
[[nodiscard]] bool VoicePlaybackAvailable();
void RevokeVoicePlaybackContinuation();
void NoteVoicePlaybackInteraction();
[[nodiscard]] Fn<bool()> VoiceQueueTransitionGuard();
void StopVoicePlaybackContinuation();
[[nodiscard]] Fn<bool()> VoicePlaybackContinuationGuard();
[[nodiscard]] Fn<bool()> NewVoicePlaybackStreamGuard();
void RetireVoicePlaybackStream();
[[nodiscard]] bool VoiceQueueMayAdvance(AudioMsgId::Type type, bool automatic);
[[nodiscard]] bool ResumeMusicAfterVoice();

[[nodiscard]] std::optional<HistoryItem*> ConfinedVoiceNext(
	AudioMsgId::Type type,
	bool autonext,
	const History *history,
	MsgId topicRootId,
	PeerId monoforumPeerId,
	int from,
	Fn<HistoryItem*(int)> itemAt);

} // namespace Purple
