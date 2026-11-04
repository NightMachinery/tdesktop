#pragma once

#include "api/api_common.h"

class MainWidget;
namespace Ui { class RpWidget; }
namespace HistoryView::Controls { class VoiceRecordBar; struct VoiceToSend; }

namespace Purple {

struct RecordingContext {
	Api::SendAction action;
	bool scheduled = false;
};

[[nodiscard]] std::optional<RecordingContext> RecordingHistoryContext(
	History *history,
	Fn<Api::SendAction()> action);
[[nodiscard]] std::optional<RecordingContext> RecordingComposeContext(
	History *history,
	FullReplyTo reply,
	PeerData *sendAs,
	BusinessShortcutId shortcut,
	bool scheduled);
void BindRecordingBar(
	not_null<HistoryView::Controls::VoiceRecordBar*> bar,
	Fn<std::optional<RecordingContext>()> context,
	Fn<void()> focus);
[[nodiscard]] bool TryDockRecording(
	not_null<HistoryView::Controls::VoiceRecordBar*> bar);
[[nodiscard]] bool PreventNewRecording();
[[nodiscard]] bool PreventRecordingStart(
	not_null<HistoryView::Controls::VoiceRecordBar*> bar);
void BindRecordingLock(not_null<HistoryView::Controls::VoiceRecordBar*> bar, const QObject *lock);
[[nodiscard]] bool RecordingLockFloats(const QObject *lock);
[[nodiscard]] bool RecordingBarActive(const HistoryView::Controls::VoiceRecordBar *bar, bool nativeActive);
[[nodiscard]] bool RecordingBarFloats(const HistoryView::Controls::VoiceRecordBar *bar);
[[nodiscard]] bool SetRoundRecordingFloat(not_null<HistoryView::Controls::VoiceRecordBar*> bar, bool small);
void RestoreFloatingRecording(not_null<HistoryView::Controls::VoiceRecordBar*> bar);
[[nodiscard]] bool SendPinnedRecording(not_null<const HistoryView::Controls::VoiceRecordBar*> bar, const HistoryView::Controls::VoiceToSend &data);
void AttachRecordingStrip(not_null<Ui::RpWidget*> parent, Fn<void()> relayout);
[[nodiscard]] int RecordingStripHeight(not_null<const Ui::RpWidget*> parent);
void PlaceRecordingStrip(
	not_null<Ui::RpWidget*> parent,
	int left,
	int top,
	int width);

} // namespace Purple
