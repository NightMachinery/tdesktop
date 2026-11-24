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
#include "ui/toast/toast_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

#include <map>
#include <utility>

namespace Purple {
namespace {

const auto kAppKey = QString::fromLatin1(ModalNotices::kAppAccountKey);

void SchedulePendingDelivery();

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
		if (it->second.accountKey == accountKey && !liveIds.contains(it->first)
			&& !Store().isPending(accountKey, it->first)) {
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
		SchedulePendingDelivery();
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

[[nodiscard]] bool CanPresentNotice(
		::MainWindow *window,
		const QString &accountKey) {
	return window
		&& !Core::App().passcodeLocked()
		&& !window->controller().locked()
		&& !PrivacyScreenActiveAny()
		&& !PrivacyScreenActive(window)
		&& WindowMatchesAccount(&window->controller(), accountKey);
}

[[nodiscard]] bool CanExposeNoticeInWindow(
		::MainWindow *window,
		const QString &accountKey) {
	return ModalKeysEnabled() && CanPresentNotice(window, accountKey);
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

using ModalNotices::DeliveryRoute;
using ModalNotices::NoticeRecord;

[[nodiscard]] bool CanShowDeferredToast(
		::MainWindow *window,
		const QString &accountKey) {
	return CanPresentNotice(window, accountKey)
		&& window->isVisible()
		&& !window->isMinimized();
}

class ToastCapture final : public QObject {
public:
	explicit ToastCapture(const QString &accountKey) : _accountKey(accountKey) {
		qApp->installEventFilter(this);
	}

	[[nodiscard]] QWidget *visibleToast() const {
		if (!_toast || _toast->isHidden()) {
			return nullptr;
		}
		if (!_toast->isVisible()) {
			_toast->deleteLater();
			return nullptr;
		}
		return _toast.data();
	}

private:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (event->type() == QEvent::Show || event->type() == QEvent::ShowToParent) {
			if (const auto toast = dynamic_cast<Ui::Toast::internal::Widget*>(object)) {
				const auto host = dynamic_cast<::MainWindow*>(toast->window());
				if (CanShowDeferredToast(host, _accountKey)) {
					_toast = toast;
				} else {
				const auto guarded = QPointer(toast);
				guarded->hide();
				if (guarded) {
					guarded->deleteLater();
				}
				}
			}
		}
		return false;
	}

	QString _accountKey;
	QPointer<QWidget> _toast;
};

[[nodiscard]] QWidget *ShowDeferredToast(
		not_null<::MainWindow*> window,
		const ModalNotices::PendingNotice &pending) {
	if (pending.fallbackToast) {
		// The legacy callbacks return void, and a stale show may do nothing.
		auto capture = ToastCapture(pending.record.accountKey);
		pending.fallbackToast();
		if (const auto toast = capture.visibleToast()) {
			return toast;
		}
	}
	if (!CanShowDeferredToast(window.get(), pending.record.accountKey)) {
		return nullptr;
	}
	const auto text = pending.record.summary.isEmpty()
		? pending.record.formattedTitle()
		: pending.record.summary;
	const auto toast = Ui::Toast::Show(window->bodyWidget(), text);
	return toast ? toast->widget().get() : nullptr;
}

void ShowStatusAck(::MainWindow *window, const NoticeRecord &record) {
	if (CanExposeNoticeInWindow(window, record.accountKey)) {
		SetModalStatusMessage(
			window,
			record.summary,
			!record.persistent && record.severity != ModalNotices::NoticeSeverity::Info,
			record.persistent ? 0 : ModalNotices::AckDurationMs(record.severity),
			record.persistent);
	}
}

class PendingNoticeDelivery final : public QObject {
public:
	PendingNoticeDelivery() : QObject(qApp) {
		qApp->installEventFilter(this);
		rpl::merge(
			Core::App().passcodeLockChanges(),
			PrivacyScreenChanges(),
			ModalKeysEnabledChanges()
		) | rpl::on_next([this] { schedule(); }, _lifetime);
		Core::App().enumerateWindows([&](not_null<Window::Controller*> ctrl) {
			if (const auto window = ctrl->widget()) {
				watch(window);
			}
		});
	}

	void schedule() {
		if (Core::Quitting()) {
			return;
		}
		if (_toast) {
			const auto host = dynamic_cast<::MainWindow*>(_toast->window());
			if (!CanShowDeferredToast(host, _toastAccount)) {
				const auto toast = std::exchange(_toast, {});
				toast->hide();
				if (toast) {
					toast->deleteLater();
				}
			}
		}
		if (!_scheduled) {
			_scheduled = true;
			QTimer::singleShot(0, this, [this] {
				_scheduled = false;
				flush();
			});
		}
	}

private:
	void watch(not_null<::MainWindow*> window) {
		if (_windows.contains(window.get())) {
			return;
		}
		_windows.insert(window.get());
		const auto raw = window.get();
		connect(raw, &QObject::destroyed, this, [this, raw] { _windows.remove(raw); });
		window->controller().sessionControllerValue() | rpl::map([](auto session) {
			return session ? session->session().termsLockValue() : rpl::single(false);
		}) | rpl::flatten_latest() | rpl::on_next([this] {
			schedule();
		}, window->controller().lifetime());
	}

	bool eventFilter(QObject *object, QEvent *event) override {
		const auto type = event->type();
		if (type == QEvent::Hide && _toast && _toast.data() == object) {
			_toast->deleteLater();
			_toast.clear();
			schedule();
		} else if (type == QEvent::Show || type == QEvent::WindowActivate
			|| type == QEvent::WindowStateChange) {
			if (const auto window = dynamic_cast<::MainWindow*>(object)) {
				watch(window);
				schedule();
			}
		}
		return false;
	}

	void flush() {
		if (Core::Quitting() || Core::App().passcodeLocked() || PrivacyScreenActiveAny()) {
			return;
		}
		Store().forgetExpiredAcks(crl::now());
		const auto active = Core::App().activeWindow();
		auto delivered = active && deliverIn(active);
		Core::App().enumerateWindows([&](not_null<Window::Controller*> ctrl) {
			if (!delivered && ctrl.get() != active) {
				delivered = deliverIn(ctrl);
			}
		});
	}

	bool deliverIn(not_null<Window::Controller*> ctrl) {
		const auto window = ctrl->widget();
		if (!CanShowDeferredToast(window, kAppKey)) {
			return false;
		}
		const auto accountKey = PurpleNoticeAccountKey(ctrl->maybeSession());
		if (!Store().pendingCount(accountKey) && !Store().pendingCount(kAppKey)) {
			return false;
		}
		if (ModalKeysEnabled() && window->isActiveWindow()) {
			EnsureModalKeysAttached(window);
		}
		const auto ctx = ModalDeliveryContext(window);
		const auto next = Store().takeNextPending(accountKey, ctx, _toast || _statusBusy);
		if (!next) {
			return false;
		}
		const auto &record = next->notice.record;
		switch (next->route) {
		case DeliveryRoute::StockToast:
			_toast = ShowDeferredToast(window, next->notice);
			if (_toast) {
				_toastAccount = record.accountKey;
				connect(_toast.data(), &QObject::destroyed, this, [this] { schedule(); });
			} else {
				schedule();
			}
			break;
		case DeliveryRoute::OpenDialogNow:
			OpenNoticeLayerForRecord(window, record);
			break;
		case DeliveryRoute::StatusLineMessage:
		case DeliveryRoute::StatusLinePersistent:
			ShowStatusAck(window, record);
			_statusBusy = true;
			QTimer::singleShot(ModalNotices::AckDurationMs(record.severity), this, [this] {
				_statusBusy = false;
				schedule();
			});
			break;
		case DeliveryRoute::QueueDialogWithCue:
		case DeliveryRoute::QueueSilentUntilAvailable:
			Unexpected("Queue route for a ready Purple notice.");
		}
		PruneOrphanedActions(record.accountKey);
		RefreshModalNoticeIndicator(window);
		return true;
	}

	QSet<::MainWindow*> _windows;
	QPointer<QWidget> _toast;
	QString _toastAccount;
	bool _scheduled = false;
	bool _statusBusy = false;
	rpl::lifetime _lifetime;
};

void SchedulePendingDelivery() {
	static const auto delivery = new PendingNoticeDelivery();
	delivery->schedule();
}

[[nodiscard]] Fn<void()> GuardDeferredFallback(
		Window::Controller *origin,
		const QString &accountKey,
		Fn<void()> fallback) {
	if (!origin || !fallback) {
		return fallback;
	}
	const auto window = QPointer(origin->widget().get());
	return [window, accountKey, run = std::move(fallback)] {
		if (CanShowDeferredToast(window.data(), accountKey)) {
			run();
		}
	};
}

void SubmitNotice(
		NoticeRecord record,
		Window::Controller *origin,
		Fn<void()> fallback,
		std::optional<PurpleNoticeAction> action = std::nullopt) {
	const auto &accountKey = record.accountKey;
	const auto window = ResolveEligibleMainWindow(origin, accountKey);
	const auto ctx = ModalDeliveryContext(window);
	const auto route = ModalNotices::DecideDelivery(
		record.kind,
		record.background,
		record.persistent,
		ctx);
	const auto enqueue = !window
		|| !CanPresentNotice(window, accountKey)
		|| Store().hasPendingKey(record)
		|| route == DeliveryRoute::QueueDialogWithCue;
	if (!enqueue && route == DeliveryRoute::StockToast) {
		if (fallback && (!origin || &window->controller() == origin)) {
			fallback();
		} else if (!fallback && record.kind == ModalNotices::NoticeKind::Dialog
			&& !ctx.modalEnabled) {
			window->controller().showToast(record.summary);
		}
	}
	if (!ctx.modalEnabled && !enqueue) {
		return;
	}
	const auto id = Store().record(
		record,
		enqueue,
		enqueue ? GuardDeferredFallback(origin, accountKey, std::move(fallback)) : Fn<void()>(),
		crl::now());
	if (action) {
		Actions()[id] = StoredAction{ accountKey, std::move(*action) };
	} else {
		Actions().erase(id);
	}
	PruneOrphanedActions(accountKey);
	if (enqueue) {
		SchedulePendingDelivery();
	} else if (route == DeliveryRoute::OpenDialogNow) {
		if (const auto stored = Store().findById(accountKey, id)) {
			OpenNoticeLayerForRecord(window, *stored);
		}
	} else if (route == DeliveryRoute::StatusLineMessage
		|| route == DeliveryRoute::StatusLinePersistent) {
		ShowStatusAck(window, record);
	}
	if (window && CanExposeNoticeInWindow(window, accountKey)) {
		RefreshModalNoticeIndicator(window);
	}
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

	SubmitNotice(
		std::move(rec),
		request.window,
		std::move(request.fallbackToast),
		std::move(request.primaryAction));
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
	SubmitNotice(std::move(rec), request.window, std::move(request.fallbackToast));
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
	if (PendingPurpleNoticeCount(window->controller().maybeSession())
		&& ModalNotices::ShouldFlushPendingDialog(ModalDeliveryContext(window.get()))) {
		SchedulePendingDelivery();
	}
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
