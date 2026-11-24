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
	if (!session || accountKey.isEmpty()) {
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

[[nodiscard]] ::MainWindow *ResolveMainWindow(
		Window::Controller *window,
		Main::Session *session) {
	if (window && window->widget()) {
		return window->widget();
	}
	if (const auto active = Core::App().activeWindow(); active && active->widget()) {
		if (!session || active->maybeSession() == session) {
			return active->widget();
		}
	}
	::MainWindow *matched = nullptr;
	if (session) {
		Core::App().enumerateWindows([&](not_null<Window::Controller*> ctrl) {
			if (!matched && ctrl->maybeSession() == session && ctrl->widget()) {
				matched = ctrl->widget();
			}
		});
	}
	if (matched) {
		return matched;
	}
	if (const auto active = Core::App().activeWindow(); active && active->widget()) {
		return active->widget();
	}
	return nullptr;
}

[[nodiscard]] QString AccountLabelFor(Main::Session *session, const QString &fallback) {
	if (!fallback.isEmpty()) {
		return fallback;
	}
	if (session && !session->account().loggingOut()
		&& session->account().maybeSession() == session) {
		return session->user()->name();
	}
	return QString();
}

void OpenNoticeLayerForRecord(
		not_null<::MainWindow*> window,
		const ModalNotices::NoticeRecord &rec) {
	auto actionLabel = rec.primaryActionLabel;
	Fn<void()> actionRun;
	if (const auto it = Actions().find(rec.id); it != Actions().end()) {
		if (actionLabel.isEmpty()) {
			actionLabel = it->second.action.label;
		}
		actionRun = it->second.action.run;
	}
	const auto weakWin = QPointer<::MainWindow>(window.get());
	auto layer = ModalUi::CreateNoticeLayer({
		.record = rec,
		.primaryActionLabel = actionLabel,
		.onPrimaryAction = std::move(actionRun),
		.onCopy = [weakWin, text = rec.plainText()] {
			if (auto *cb = QGuiApplication::clipboard()) {
				cb->setText(text);
			}
			if (weakWin) {
				SetModalStatusMessage(
					weakWin.data(),
					u"Copied notice to clipboard"_q,
					false,
					ModalNotices::kAckDurationInfoMs,
					false);
			}
		},
	});
	ShowModalNoticeLayer(window, std::move(layer), /*isHistory=*/false);
}

} // namespace

QString PurpleNoticeAccountKey(Main::Session *session) {
	if (!session || session->account().loggingOut()
		|| session->account().maybeSession() != session) {
		return u"app"_q;
	}
	const auto key = SessionAccountKey(not_null{ session });
	return AccountKeyText(key.environment, key.userId).value_or(u"app"_q);
}

std::optional<QString> ResolveNoticePeerLabel(
		not_null<Main::Session*> session,
		uint64_t rawId) {
	if (session->account().loggingOut()
		|| session->account().maybeSession() != session.get()) {
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

	auto *mw = ResolveMainWindow(request.window, request.session);
	const auto ctx = ModalDeliveryContext(mw);
	const auto route = ModalNotices::DecideDelivery(
		ModalNotices::NoticeKind::Dialog,
		request.background,
		false,
		ctx);

	const auto enqueuePending = (route == ModalNotices::DeliveryRoute::QueueDialogWithCue
		|| route == ModalNotices::DeliveryRoute::QueueSilentUntilAvailable);
	const auto id = Store().record(rec, enqueuePending);
	if (request.primaryAction) {
		Actions()[id] = StoredAction{
			.accountKey = accountKey,
			.action = std::move(*request.primaryAction),
		};
	}
	PruneOrphanedActions(accountKey);

	switch (route) {
	case ModalNotices::DeliveryRoute::StockToast:
		if (request.fallbackToast) {
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
		} else if (request.fallbackToast) {
			request.fallbackToast();
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

	auto *mw = ResolveMainWindow(request.window, request.session);
	const auto ctx = ModalDeliveryContext(mw);
	const auto route = ModalNotices::DecideDelivery(
		ModalNotices::NoticeKind::Ack,
		request.background,
		request.persistent,
		ctx);

	switch (route) {
	case ModalNotices::DeliveryRoute::StockToast:
		if (request.fallbackToast) {
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
		} else if (request.fallbackToast) {
			request.fallbackToast();
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
		} else if (request.fallbackToast) {
			request.fallbackToast();
		}
		return;
	case ModalNotices::DeliveryRoute::OpenDialogNow:
	case ModalNotices::DeliveryRoute::QueueDialogWithCue:
		break;
	}
}

int PendingPurpleNoticeCount(Main::Session *session) {
	auto count = Store().pendingCount(PurpleNoticeAccountKey(session));
	if (session) {
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
	const auto accountLabel = AccountLabelFor(session, u"Purple"_q);

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
			if (weakWin) {
				OpenNoticeLayerForRecord(weakWin.data(), rec);
			}
		},
		.onCopy = [weakWin](const QString &text) {
			if (auto *cb = QGuiApplication::clipboard()) {
				cb->setText(text);
			}
			if (weakWin) {
				SetModalStatusMessage(
					weakWin.data(),
					u"Copied notice to clipboard"_q,
					false,
					ModalNotices::kAckDurationInfoMs,
					false);
			}
		},
		.onClear = [weakWin, accountKey] {
			Store().clearHistory(accountKey);
			PurgeActionsForAccount(accountKey);
			if (weakWin) {
				SetModalStatusMessage(
					weakWin.data(),
					u"Cleared notice history"_q,
					false,
					ModalNotices::kAckDurationInfoMs,
					false);
			}
		},
	});
	ShowModalNoticeLayer(window, std::move(layer), /*isHistory=*/true);
}

} // namespace Purple
