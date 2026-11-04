#pragma once

#include "api/api_common.h"

class MainWidget;
namespace Ui { class RpWidget; }
namespace HistoryView::Controls { class VoiceRecordBar; }

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
[[nodiscard]] bool PreventRecordingStart(
	not_null<HistoryView::Controls::VoiceRecordBar*> bar);
void AttachRecordingStrip(not_null<Ui::RpWidget*> parent, Fn<void()> relayout);
[[nodiscard]] int RecordingStripHeight(not_null<const Ui::RpWidget*> parent);
void PlaceRecordingStrip(
	not_null<Ui::RpWidget*> parent,
	int left,
	int top,
	int width);

} // namespace Purple
