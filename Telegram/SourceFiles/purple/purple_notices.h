#pragma once

#include "base/basic_types.h"
#include "purple/purple_notice_state.h"

#include <QtCore/QString>

#include <optional>
#include <vector>

class MainWindow;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class Controller;
} // namespace Window

namespace Purple {

struct PurpleNoticeAction {
	QString label;
	Fn<void()> run;
};

struct PurpleNoticeRequest {
	Main::Session *session = nullptr;
	Window::Controller *window = nullptr;
	QString accountKeyOverride;
	QString accountLabel;
	QString scope;
	QString title;
	QString summary;
	QString coalesceKey;
	ModalNotices::NoticeSeverity severity = ModalNotices::NoticeSeverity::Info;
	bool background = false;
	std::vector<ModalNotices::NoticeRow> rows;
	std::optional<PurpleNoticeAction> primaryAction;
	Fn<void()> fallbackToast;
};

struct PurpleAckRequest {
	Main::Session *session = nullptr;
	Window::Controller *window = nullptr;
	QString accountKeyOverride;
	QString accountLabel;
	QString scope;
	QString text;
	QString coalesceKey;
	ModalNotices::NoticeSeverity severity = ModalNotices::NoticeSeverity::Info;
	bool background = false;
	bool persistent = false;
	Fn<void()> fallbackToast;
};

void ShowPurpleNotice(PurpleNoticeRequest request);
void ShowPurpleAck(PurpleAckRequest request);

void MaybeFlushPendingNotices(not_null<::MainWindow*> window);
void OpenPurpleNoticeHistory(not_null<::MainWindow*> window);
[[nodiscard]] int PendingPurpleNoticeCount(Main::Session *session);

[[nodiscard]] QString PurpleNoticeAccountKey(Main::Session *session);
[[nodiscard]] std::optional<QString> ResolveNoticePeerLabel(
	not_null<Main::Session*> session,
	uint64_t rawId);

} // namespace Purple
