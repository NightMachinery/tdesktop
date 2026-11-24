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

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <map>

namespace Purple {
namespace {

const auto kAppKey = QString::fromLatin1(ModalNotices::kAppAccountKey);

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
	if (!session || accountKey.isEmpty() || accountKey == kAppKey) {
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
	if (accountKey == kAppKey) {
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
	if (!window || !ModalKeysEnabled()) {
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

[[nodiscard]] std::optional<QString> ResolveNoticePeerLabel(
		not_null<Main::Session*> session,
		uint64_t rawId) {
	if (rawId > PeerId::kChatTypeMask) {
		return std::nullopt;
	}
	const auto bare = BareId(rawId);
	const auto &data = session->data();
	const auto candidates = {
		data.peerLoaded(peerFromUser(bare)),
		data.peerLoaded(peerFromChat(bare)),
		data.peerLoaded(peerFromChannel(bare)),
	};
	auto *peer = static_cast<PeerData*>(nullptr);
	for (const auto candidate : candidates) {
		if (candidate) {
			if (peer) {
				return std::nullopt;
			}
			peer = candidate;
		}
	}
	if (!peer || !PickerVisible(peer)) {
		return std::nullopt;
	}
	const auto name = peer->name().trimmed();
	return name.isEmpty() ? std::nullopt : std::make_optional(name);
}

[[nodiscard]] ModalNotices::NoticeRecord ResolveNoticeLabels(
		::MainWindow *window,
		ModalNotices::NoticeRecord rec) {
	if (rec.accountKey == kAppKey) {
		return rec;
	}
	const auto session = not_null{ window->controller().maybeSession() };
	rec.accountLabel = AccountLabelFor(session, rec.accountLabel);
	const auto lookup = [=](uint64_t id) { return ResolveNoticePeerLabel(session, id); };
	for (auto &row : rec.rows) {
		if (row.hasConfigPeerIds) {
			row.before = ModalNotices::ResolvePeerIdsInConfigValue(row.before, lookup);
			row.after = ModalNotices::ResolvePeerIdsInConfigValue(row.after, lookup);
		}
	}
	return rec;
}

void CopyRecord(::MainWindow *window, const ModalNotices::NoticeRecord &rec) {
	if (!CanExposeNoticeInWindow(window, rec.accountKey)) {
		return;
	}
	if (auto *cb = QGuiApplication::clipboard()) {
		cb->setText(ResolveNoticeLabels(window, rec).plainText());
	}
	SetModalStatusMessage(
		window,
		u"Copied notice to clipboard"_q,
		false,
		ModalNotices::kAckDurationInfoMs,
		false);
}

void OpenNoticeLayerForRecord(
		not_null<::MainWindow*> window,
		const ModalNotices::NoticeRecord &rec) {
	if (!CanExposeNoticeInWindow(window.get(), rec.accountKey)) {
		return;
	}
	const auto weakWin = QPointer<::MainWindow>(window.get());
	const auto current = [weakWin, accountKey = rec.accountKey, id = rec.id] {
		return CanExposeNoticeInWindow(weakWin.data(), accountKey)
			? Store().findById(accountKey, id)
			: std::nullopt;
	};
	auto guardedAction = Fn<void()>();
	if (!rec.primaryActionLabel.isEmpty() && Actions().contains(rec.id)) {
		guardedAction = [weakWin, current, id = rec.id, revision = rec.repeatCount] {
			const auto live = current();
			const auto it = Actions().find(id);
			if (live && live->repeatCount == revision && it != Actions().end()) {
				const auto run = it->second.action.run;
				if (run) {
					run(&weakWin->controller());
				}
			}
		};
	}
	auto layer = ModalUi::CreateNoticeLayer({
		.record = ResolveNoticeLabels(window.get(), rec),
		.primaryActionLabel = guardedAction ? rec.primaryActionLabel : QString(),
		.onPrimaryAction = std::move(guardedAction),
		.onCopy = [weakWin, current] {
			if (const auto live = current()) {
				CopyRecord(weakWin.data(), *live);
			}
		},
	});
	ShowModalNoticeLayer(window, std::move(layer), /*isHistory=*/false);
}

[[nodiscard]] QString RequestAccountKey(
		const std::optional<Main::Session*> &session,
		bool appScoped = false) {
	if (appScoped || !session) {
		return kAppKey;
	}
	return *session ? PurpleNoticeAccountKey(*session) : QString();
}

} // namespace

QString PurpleNoticeAccountKey(Main::Session *session) {
	if (!session) {
		return kAppKey;
	}
	if (!SessionIsLive(session)) {
		return QString();
	}
	const auto key = SessionAccountKey(not_null{ session });
	return AccountKeyText(key.environment, key.userId).value_or(QString());
}

Window::Controller *PurpleNoticeWindow(not_null<Main::Session*> session) {
	const auto window = ResolveEligibleMainWindow(nullptr, PurpleNoticeAccountKey(session));
	return window ? &window->controller() : nullptr;
}

void ShowPurpleNotice(PurpleNoticeRequest request) {
	const auto accountKey = RequestAccountKey(request.session, request.appScoped);
	const auto session = request.appScoped ? nullptr : request.session.value_or(nullptr);
	if (accountKey.isEmpty()) {
		// Dead or logging-out session: drop rather than leaking to "app".
		return;
	}
	EnsureLogoutHook(session, accountKey);

	ModalNotices::NoticeRecord rec;
	rec.accountKey = accountKey;
	rec.accountLabel = AccountLabelFor(session, request.accountLabel);
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

	if (!ctx.modalEnabled) {
		if (route == ModalNotices::DeliveryRoute::StockToast && mw) {
			if (request.fallbackToast && (!request.window || &mw->controller() == request.window)) {
				request.fallbackToast();
			} else if (!request.fallbackToast) {
				mw->controller().showToast(request.summary);
			}
		}
		return;
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
			RefreshModalNoticeIndicator(mw);
		}
		return;
	case ModalNotices::DeliveryRoute::StatusLineMessage:
	case ModalNotices::DeliveryRoute::StatusLinePersistent:
		break;
	}
}

void ShowPurpleAck(PurpleAckRequest request) {
	const auto accountKey = RequestAccountKey(request.session);
	const auto session = request.session.value_or(nullptr);
	if (accountKey.isEmpty()) {
		// Dead or logging-out session: drop rather than leaking to "app".
		return;
	}
	EnsureLogoutHook(session, accountKey);

	ModalNotices::NoticeRecord rec;
	rec.accountKey = accountKey;
	rec.accountLabel = AccountLabelFor(session, request.accountLabel);
	rec.scope = request.scope.isEmpty() ? u"Notice"_q : request.scope;
	rec.summary = request.text;
	rec.coalesceKey = request.coalesceKey;
	rec.severity = request.severity;
	rec.kind = ModalNotices::NoticeKind::Ack;
	rec.background = request.background;
	rec.persistent = request.persistent;
	rec.timestampMs = QDateTime::currentMSecsSinceEpoch();
	auto *mw = ResolveEligibleMainWindow(request.window, accountKey);
	const auto ctx = ModalDeliveryContext(mw);
	const auto route = ModalNotices::DecideDelivery(
		ModalNotices::NoticeKind::Ack,
		request.background,
		request.persistent,
		ctx);

	if (ctx.modalEnabled) {
		const auto id = Store().record(rec, /*enqueuePending=*/false);
		Actions().erase(id);
		PruneOrphanedActions(accountKey);
	}

	switch (route) {
	case ModalNotices::DeliveryRoute::StockToast:
		if (mw && request.fallbackToast
			&& (!request.window || &mw->controller() == request.window)) {
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
	if (key != kAppKey) {
		count += Store().pendingCount(kAppKey);
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
	if (!next && accountKey != kAppKey) {
		next = Store().takeNextPending(kAppKey);
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
	if (accountKey != kAppKey) {
		const auto &appItems = Store().historyFor(kAppKey);
		items.insert(items.end(), appItems.begin(), appItems.end());
		std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
			return a.timestampMs > b.timestampMs;
		});
	}

	const auto weakWin = QPointer<::MainWindow>(window.get());
	auto layer = ModalUi::CreateNoticeHistoryLayer({
		.accountLabel = accountLabel,
		.items = std::move(items),
		.onInspect = [weakWin, accountKey](const ModalNotices::NoticeRecord &rec) {
			if (CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				if (const auto live = Store().findById(rec.accountKey, rec.id)) {
					OpenNoticeLayerForRecord(weakWin.data(), *live);
				}
			}
		},
		.onCopy = [weakWin, accountKey](const ModalNotices::NoticeRecord &rec) {
			if (CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				if (const auto live = Store().findById(rec.accountKey, rec.id)) {
					CopyRecord(weakWin.data(), *live);
				}
			}
		},
		.onClear = [weakWin, accountKey] {
			if (!weakWin || !CanExposeNoticeInWindow(weakWin.data(), accountKey)) {
				return;
			}
			// Clear both history and pending entries across all shown scopes
			// so nothing cleared reappears on reopening SPC h e.
			Store().clearHistoryAndPending(accountKey);
			PurgeActionsForAccount(accountKey);
			if (accountKey != kAppKey) {
				Store().clearHistoryAndPending(kAppKey);
				PurgeActionsForAccount(kAppKey);
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
