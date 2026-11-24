#include "purple/purple_notices.h"

#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "purple/purple_config.h"
#include "purple/purple_keys.h"
#include "purple/purple_keys_ui.h"
#include "purple/purple_picker.h"
#include "purple/purple_privacy_screen.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <map>

namespace Purple {
namespace {

struct StoredAction {
	QString accountKey;
	PurpleNoticeAction action;
};

ModalNotices::AccountNoticeStore &Store() {
	static auto *store = new ModalNotices::AccountNoticeStore();
	return *store;
}

std::map<uint64_t, StoredAction> &Actions() {
	static auto *map = new std::map<uint64_t, StoredAction>();
	return *map;
}

QSet<const Main::Session*> &WatchedSessions() {
	static auto *set = new QSet<const Main::Session*>();
	return *set;
}

void PurgeActionsForAccount(const QString &accountKey) {
	auto &actions = Actions();
	for (auto it = actions.begin(); it != actions.end();) {
		if (it->second.accountKey == accountKey) {
			it = actions.erase(it);
		} else {
			++it;
		}
	}
}

void PruneOrphanedActions(const QString &accountKey) {
	const auto &hist = Store().historyFor(accountKey);
	auto liveIds = QSet<uint64_t>();
	liveIds.reserve(int(hist.size()));
	for (const auto &rec : hist) {
		liveIds.insert(rec.id);
	}
	auto &actions = Actions();
	for (auto it = actions.begin(); it != actions.end();) {
		if (it->second.accountKey == accountKey && !liveIds.contains(it->first)) {
			it = actions.erase(it);
		} else {
			++it;
		}
	}
}

void EnsureLogoutHook(Main::Session *session, const QString &accountKey) {
	if (!session || accountKey.isEmpty() || accountKey == u"app"_q) {
		return;
	}
	auto &watched = WatchedSessions();
	if (watched.contains(session)) {
		return;
	}
	watched.insert(session);
	const auto raw = session;
	session->data().sessionDataAboutToBeCleared() | rpl::on_next([raw, accountKey] {
		WatchedSessions().remove(raw);
		Store().clearAccount(accountKey);
		PurgeActionsForAccount(accountKey);
	}, session->lifetime());
}

[[nodiscard]] bool SessionIsLive(Main::Session *session) {
	return session
		&& !session->account().loggingOut()
		&& session->account().maybeSession() == session;
}

[[nodiscard]] bool WindowMatchesAccount(
		Window::Controller *ctrl,
		const QString &accountKey) {
	if (!ctrl || !ctrl->widget()) {
		return false;
	}
	if (accountKey == u"app"_q) {
		return true;
	}
	const auto live = ctrl->maybeSession();
	return SessionIsLive(live) && PurpleNoticeAccountKey(live) == accountKey;
}

// Account-scoped notices ONLY resolve to a window showing THAT account,
// never falling back to another account's active window.
[[nodiscard]] ::MainWindow *ResolveEligibleMainWindow(
		Window::Controller *window,
		const QString &accountKey) {
	if (accountKey.isEmpty()) {
		return nullptr;
	}
	if (WindowMatchesAccount(window, accountKey)) {
		return window->widget();
	}
	if (const auto active = Core::App().activeWindow();
		WindowMatchesAccount(active, accountKey)) {
		return active->widget();
	}
	::MainWindow *matched = nullptr;
	Core::App().enumerateWindows([&](not_null<Window::Controller*> ctrl) {
		if (!matched && WindowMatchesAccount(ctrl.get(), accountKey)) {
			matched = ctrl->widget();
		}
	});
	return matched;
}

// Prefer the live session's user name first; fallback only when session is absent.
[[nodiscard]] QString AccountLabelFor(Main::Session *session, const QString &fallback) {
	if (SessionIsLive(session)) {
		const auto name = session->user()->name().trimmed();
		if (!name.isEmpty()) {
			return name;
		}
	}
	return fallback;
}

[[nodiscard]] bool CanExposeNoticeInWindow(
		::MainWindow *window,
		const QString &accountKey) {
	if (!window) {
		return false;
	}
	if (Core::App().passcodeLocked()
		|| window->controller().locked()
		|| Purple::PrivacyScreenActiveAny()
		|| Purple::PrivacyScreenActive(window)) {
		return false;
	}
	return WindowMatchesAccount(&window->controller(), accountKey);
}

void OpenNoticeLayerForRecord(
		not_null<::MainWindow*> window,
		const ModalNotices::NoticeRecord &rec) {
	if (!CanExposeNoticeInWindow(window.get(), rec.accountKey)) {
		return;
	}
	auto actionLabel = rec.primaryActionLabel;
	Fn<void()> actionRun;
	if (!actionLabel.isEmpty()) {
		if (const auto it = Actions().find(rec.id); it != Actions().end()) {
			actionRun = it->second.action.run;
		}
	}
	const auto weakWin = QPointer<::MainWindow>(window.get());
	const auto accountKey = rec.accountKey;
	auto guardedAction = Fn<void()>();
	if (!actionLabel.isEmpty() && actionRun) {
		guardedAction = [weakWin, accountKey, run = std::move(actionRun)] {
			if (!weakWin || !CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				return;
			}
			run();
		};
	}
	auto layer = ModalUi::CreateNoticeLayer({
		.record = rec,
		.primaryActionLabel = guardedAction ? actionLabel : QString(),
		.onPrimaryAction = std::move(guardedAction),
		.onCopy = [weakWin, accountKey, text = rec.plainText()] {
			if (!weakWin || !CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				return;
			}
			if (auto *cb = QGuiApplication::clipboard()) {
				cb->setText(text);
			}
			SetModalStatusMessage(
				weakWin.data(),
				u"Copied notice to clipboard"_q,
				false,
				ModalNotices::kAckDurationInfoMs,
				false);
		},
	});
	ShowModalNoticeLayer(window, std::move(layer), /*isHistory=*/false);
}

} // namespace

// Returns "app" ONLY when session == nullptr (genuinely account-free).
// Returns empty QString when a non-null session is dead or logging out so
// callers drop the event instead of leaking it into the shared "app" bucket.
QString PurpleNoticeAccountKey(Main::Session *session) {
	if (!session) {
		return u"app"_q;
	}
	if (!SessionIsLive(session)) {
		return QString();
	}
	const auto key = SessionAccountKey(not_null{ session });
	return AccountKeyText(key.environment, key.userId).value_or(QString());
}

std::optional<QString> ResolveNoticePeerLabel(
		not_null<Main::Session*> session,
		uint64_t rawId) {
	if (!SessionIsLive(session.get())) {
		return std::nullopt;
	}
	const auto peer = session->data().peerLoaded(PeerId(rawId));
	if (!peer) {
		return std::nullopt;
	}
	// Privacy rule: hidden Work Mode chats fall back to raw ID.
	if (!PickerVisible(peer)) {
		return std::nullopt;
	}
	const auto name = peer->name().trimmed();
	return name.isEmpty() ? std::nullopt : std::make_optional(name);
}

void ShowPurpleNotice(PurpleNoticeRequest request) {
	const auto accountKey = !request.accountKeyOverride.isEmpty()
		? request.accountKeyOverride
		: PurpleNoticeAccountKey(request.session);
	if (accountKey.isEmpty()) {
		// Dead or logging-out session: drop rather than leaking to "app".
		return;
	}
	EnsureLogoutHook(request.session, accountKey);

	ModalNotices::NoticeRecord rec;
	rec.accountKey = accountKey;
	rec.accountLabel = AccountLabelFor(request.session, request.accountLabel);
	rec.scope = request.scope;
	rec.title = request.title;
	rec.summary = request.summary;
	rec.coalesceKey = request.coalesceKey;
	rec.severity = request.severity;
	rec.kind = ModalNotices::NoticeKind::Dialog;
	rec.background = request.background;
	rec.timestampMs = QDateTime::currentMSecsSinceEpoch();
	rec.rows = std::move(request.rows);
	if (request.primaryAction) {
		rec.primaryActionLabel = request.primaryAction->label;
	}

	auto *mw = ResolveEligibleMainWindow(request.window, accountKey);
	const auto ctx = ModalDeliveryContext(mw);
	auto route = ModalNotices::DecideDelivery(
		ModalNotices::NoticeKind::Dialog,
		request.background,
		false,
		ctx);
	// If modal is enabled and no window for this account exists yet, queue
	// silently under this account rather than displaying in another account's window.
	if (ctx.modalEnabled && !mw
		&& route != ModalNotices::DeliveryRoute::QueueSilentUntilAvailable) {
		route = ModalNotices::DeliveryRoute::QueueSilentUntilAvailable;
	}

	const auto enqueuePending = (route == ModalNotices::DeliveryRoute::QueueDialogWithCue
		|| route == ModalNotices::DeliveryRoute::QueueSilentUntilAvailable);
	const auto id = Store().record(rec, enqueuePending);
	if (request.primaryAction) {
		Actions()[id] = StoredAction{
			.accountKey = accountKey,
			.action = std::move(*request.primaryAction),
		};
	} else {
		// Coalesced passive update invalidates any previously stored action callback.
		Actions().erase(id);
	}
	PruneOrphanedActions(accountKey);

	switch (route) {
	case ModalNotices::DeliveryRoute::StockToast:
		if (mw && request.fallbackToast) {
			request.fallbackToast();
		}
		return;
	case ModalNotices::DeliveryRoute::QueueSilentUntilAvailable:
		return;
	case ModalNotices::DeliveryRoute::QueueDialogWithCue:
		if (mw) {
			RefreshModalNoticeIndicator(mw);
		}
		return;
	case ModalNotices::DeliveryRoute::OpenDialogNow:
		if (mw) {
			if (const auto stored = Store().findById(accountKey, id)) {
				OpenNoticeLayerForRecord(mw, *stored);
			}
		}
		return;
	case ModalNotices::DeliveryRoute::StatusLineMessage:
	case ModalNotices::DeliveryRoute::StatusLinePersistent:
		break;
	}
}

void ShowPurpleAck(PurpleAckRequest request) {
	const auto accountKey = !request.accountKeyOverride.isEmpty()
		? request.accountKeyOverride
		: PurpleNoticeAccountKey(request.session);
	if (accountKey.isEmpty()) {
		// Dead or logging-out session: drop rather than leaking to "app".
		return;
	}
	EnsureLogoutHook(request.session, accountKey);

	ModalNotices::NoticeRecord rec;
	rec.accountKey = accountKey;
	rec.accountLabel = AccountLabelFor(request.session, request.accountLabel);
	rec.scope = request.scope.isEmpty() ? u"Notice"_q : request.scope;
	rec.summary = request.text;
	rec.coalesceKey = request.coalesceKey;
	rec.severity = request.severity;
	rec.kind = ModalNotices::NoticeKind::Ack;
	rec.background = request.background;
	rec.persistent = request.persistent;
	rec.timestampMs = QDateTime::currentMSecsSinceEpoch();
	Store().record(rec, /*enqueuePending=*/false);
	PruneOrphanedActions(accountKey);

	auto *mw = ResolveEligibleMainWindow(request.window, accountKey);
	const auto ctx = ModalDeliveryContext(mw);
	const auto route = ModalNotices::DecideDelivery(
		ModalNotices::NoticeKind::Ack,
		request.background,
		request.persistent,
		ctx);

	switch (route) {
	case ModalNotices::DeliveryRoute::StockToast:
		if (mw && request.fallbackToast) {
			request.fallbackToast();
		}
		return;
	case ModalNotices::DeliveryRoute::QueueSilentUntilAvailable:
		return;
	case ModalNotices::DeliveryRoute::StatusLineMessage:
		if (mw) {
			SetModalStatusMessage(
				mw,
				request.text,
				request.severity != ModalNotices::NoticeSeverity::Info,
				ModalNotices::AckDurationMs(request.severity),
				/*persistent=*/false);
		}
		return;
	case ModalNotices::DeliveryRoute::StatusLinePersistent:
		if (mw) {
			SetModalStatusMessage(
				mw,
				request.text,
				false,
				0,
				/*persistent=*/true);
		}
		return;
	case ModalNotices::DeliveryRoute::OpenDialogNow:
	case ModalNotices::DeliveryRoute::QueueDialogWithCue:
		break;
	}
}

int PendingPurpleNoticeCount(Main::Session *session) {
	const auto key = PurpleNoticeAccountKey(session);
	if (key.isEmpty()) {
		return 0;
	}
	auto count = Store().pendingCount(key);
	if (key != u"app"_q) {
		count += Store().pendingCount(u"app"_q);
	}
	return count;
}

void MaybeFlushPendingNotices(not_null<::MainWindow*> window) {
	const auto ctx = ModalDeliveryContext(window.get());
	if (!ModalNotices::ShouldFlushPendingDialog(ctx)) {
		return;
	}
	const auto session = window->controller().maybeSession();
	const auto accountKey = PurpleNoticeAccountKey(session);
	if (accountKey.isEmpty()) {
		return;
	}
	// Flush at most ONE eligible dialog per transition so multiple queued
	// notices never cause a chain of modal interruptions.
	auto next = Store().takeNextPending(accountKey);
	if (!next && accountKey != u"app"_q) {
		next = Store().takeNextPending(u"app"_q);
	}
	if (!next) {
		return;
	}
	OpenNoticeLayerForRecord(window, *next);
	RefreshModalNoticeIndicator(window.get());
}

void OpenPurpleNoticeHistory(not_null<::MainWindow*> window) {
	const auto ctx = ModalDeliveryContext(window.get());
	if (!ModalNotices::HistoryAccessible(ctx)) {
		return;
	}
	const auto session = window->controller().maybeSession();
	const auto accountKey = PurpleNoticeAccountKey(session);
	if (accountKey.isEmpty()) {
		return;
	}
	const auto accountLabel = AccountLabelFor(session, u"App"_q);

	auto items = Store().historyFor(accountKey);
	if (accountKey != u"app"_q) {
		const auto &appItems = Store().historyFor(u"app"_q);
		items.insert(items.end(), appItems.begin(), appItems.end());
		std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
			return a.timestampMs > b.timestampMs;
		});
	}

	const auto weakWin = QPointer<::MainWindow>(window.get());
	auto layer = ModalUi::CreateNoticeHistoryLayer({
		.accountLabel = accountLabel,
		.items = std::move(items),
		.onInspect = [weakWin](ModalNotices::NoticeRecord rec) {
			if (weakWin && CanExposeNoticeInWindow(weakWin.data(), rec.accountKey)) {
				OpenNoticeLayerForRecord(weakWin.data(), rec);
			}
		},
		.onCopy = [weakWin, accountKey](const QString &text) {
			if (!weakWin || !CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				return;
			}
			if (auto *cb = QGuiApplication::clipboard()) {
				cb->setText(text);
			}
			SetModalStatusMessage(
				weakWin.data(),
				u"Copied notice to clipboard"_q,
				false,
				ModalNotices::kAckDurationInfoMs,
				false);
		},
		.onClear = [weakWin, accountKey] {
			if (!weakWin || !CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				return;
			}
			// Clear both history and pending entries across all shown scopes
			// so nothing cleared reappears on reopening SPC h e.
			Store().clearHistoryAndPending(accountKey);
			PurgeActionsForAccount(accountKey);
			if (accountKey != u"app"_q) {
				Store().clearHistoryAndPending(u"app"_q);
				PurgeActionsForAccount(u"app"_q);
			}
			RefreshModalNoticeIndicator(weakWin.data());
			SetModalStatusMessage(
				weakWin.data(),
				u"Cleared notices and pending queue"_q,
				false,
				ModalNotices::kAckDurationInfoMs,
				false);
		},
	});
	ShowModalNoticeLayer(window, std::move(layer), /*isHistory=*/true);
}

} // namespace Purple
