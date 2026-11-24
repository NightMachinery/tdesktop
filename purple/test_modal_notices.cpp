#include "purple/purple_notice_state.h"

#include <iostream>
#include <string>

namespace {

using namespace Purple::ModalNotices;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string &name) {
	++gChecks;
	if (!ok) {
		++gFailures;
		std::cerr << "FAIL: " << name << "\n";
	}
}

} // namespace

int main() {
	// 1. Modal keys OFF always keeps stock toast unchanged everywhere.
	{
		DeliveryContext ctx;
		ctx.modalEnabled = false;
		ctx.statusBarVisible = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, ctx) == DeliveryRoute::StockToast,
			"modal off: background dialog -> stock toast");
		Check(DecideDelivery(NoticeKind::Dialog, false, false, ctx) == DeliveryRoute::StockToast,
			"modal off: user dialog -> stock toast");
		Check(DecideDelivery(NoticeKind::Ack, false, false, ctx) == DeliveryRoute::StockToast,
			"modal off: ack -> stock toast");
		Check(!ShouldFlushPendingDialog(ctx), "modal off: never flushes modal dialogs");
		Check(!HistoryAccessible(ctx), "modal off: modal history layer unavailable");
	}

	// 2. Passcode locked or privacy screen active queues silently and hides history.
	{
		DeliveryContext locked;
		locked.modalEnabled = true;
		locked.passcodeLocked = true;
		locked.statusBarVisible = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, locked) == DeliveryRoute::QueueSilentUntilAvailable,
			"passcode locked: dialog queued silently");
		Check(DecideDelivery(NoticeKind::Ack, false, false, locked) == DeliveryRoute::QueueSilentUntilAvailable,
			"passcode locked: ack suppressed silently");
		Check(!ShouldFlushPendingDialog(locked), "passcode locked: no flush");
		Check(!HistoryAccessible(locked), "passcode locked: SPC h e unavailable");

		DeliveryContext priv;
		priv.modalEnabled = true;
		priv.privacyScreenActive = true;
		priv.statusBarVisible = true;
		Check(DecideDelivery(NoticeKind::Dialog, false, false, priv) == DeliveryRoute::QueueSilentUntilAvailable,
			"privacy active: dialog queued silently");
		Check(!ShouldFlushPendingDialog(priv), "privacy active: no flush");
		Check(!HistoryAccessible(priv), "privacy active: SPC h e unavailable");

		// Privacy/lock check runs BEFORE modalEnabled check: never leak stock toast while locked/private!
		DeliveryContext modalOffLocked;
		modalOffLocked.modalEnabled = false;
		modalOffLocked.passcodeLocked = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, modalOffLocked) == DeliveryRoute::QueueSilentUntilAvailable,
			"modal off + passcode locked: suppresses revealing stock toast fallback");

		DeliveryContext modalOffPrivate;
		modalOffPrivate.modalEnabled = false;
		modalOffPrivate.privacyScreenActive = true;
		Check(DecideDelivery(NoticeKind::Ack, false, false, modalOffPrivate) == DeliveryRoute::QueueSilentUntilAvailable,
			"modal off + privacy active: suppresses revealing stock toast fallback");
	}

	// 3. NO STOLEN TYPING: background dialogs queue with cue during Insert, IME, prefix, or layer.
	{
		DeliveryContext normal;
		normal.modalEnabled = true;
		normal.statusBarVisible = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, normal) == DeliveryRoute::OpenDialogNow,
			"normal mode, clear: background dialog opens immediately");
		Check(ShouldFlushPendingDialog(normal), "normal mode, clear: pending dialogs flush");

		auto insertCtx = normal;
		insertCtx.inInsertMode = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, insertCtx) == DeliveryRoute::QueueDialogWithCue,
			"insert mode: background dialog queues with status cue (never steals typing)");
		Check(!ShouldFlushPendingDialog(insertCtx), "insert mode: does not flush pending dialog");

		auto imeCtx = normal;
		imeCtx.hasImePreedit = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, imeCtx) == DeliveryRoute::QueueDialogWithCue,
			"IME preedit: background dialog queues with cue");

		auto prefixCtx = normal;
		prefixCtx.hasPendingPrefix = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, prefixCtx) == DeliveryRoute::QueueDialogWithCue,
			"pending prefix: background dialog queues with cue");

		auto layerCtx = normal;
		layerCtx.hasBlockingLayerOrBox = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, layerCtx) == DeliveryRoute::QueueDialogWithCue,
			"open layer/box: background dialog queues with cue");

		auto inactiveCtx = normal;
		inactiveCtx.canInterruptWindow = false;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, inactiveCtx) == DeliveryRoute::QueueDialogWithCue
			&& !ShouldFlushPendingDialog(inactiveCtx),
			"inactive window or held key: defer background notices");
	}

	// 4. Group B visibility: status line in Normal, transient bar in Zen, stock toast fallback when box covers bar.
	{
		DeliveryContext normal;
		normal.modalEnabled = true;
		normal.statusBarVisible = true;
		Check(DecideDelivery(NoticeKind::Ack, false, false, normal) == DeliveryRoute::StatusLineMessage,
			"normal with visible status bar -> status line");
		Check(DecideDelivery(NoticeKind::Ack, true, true, normal) == DeliveryRoute::StatusLinePersistent,
			"soft screen-time budget -> persistent status line cue");

		DeliveryContext zen;
		zen.modalEnabled = true;
		zen.inZenMode = true;
		zen.statusBarVisible = false;
		Check(DecideDelivery(NoticeKind::Ack, false, false, zen) == DeliveryRoute::StatusLineMessage,
			"Zen mode -> transient status bar shows Group B message");

		DeliveryContext covered;
		covered.modalEnabled = true;
		covered.statusBarVisible = false;
		covered.inZenMode = false;
		covered.hasBlockingLayerOrBox = true;
		Check(DecideDelivery(NoticeKind::Ack, false, false, covered) == DeliveryRoute::StockToast,
			"status bar covered by open box -> falls back to stock toast");

		Check(AckDurationMs(NoticeSeverity::Error) > AckDurationMs(NoticeSeverity::Info),
			"Group B error stays longer than acknowledgement");
	}

	// 5. Per-account coalescing, memory-only cap, and logout clearing.
	{
		AccountNoticeStore store;
		NoticeRecord r1;
		r1.accountKey = QStringLiteral("prod:1001");
		r1.accountLabel = QStringLiteral("Alice");
		r1.scope = QStringLiteral("Local archive");
		r1.summary = QStringLiteral("Archive gap");
		r1.coalesceKey = QStringLiteral("archive:gap:prod:1001");
		r1.timestampMs = 1000;

		const auto id1 = store.record(r1, true);
		r1.timestampMs = 2000;
		const auto id2 = store.record(r1, true);
		const auto id3 = store.record(r1, true);
		Check(id1 == id2 && id2 == id3, "coalesced archive notices reuse same record id");
		Check(store.pendingCount(QStringLiteral("prod:1001")) == 1,
			"three repeated archive gap events produce one pending notice per account");
		Check(store.historyFor(QStringLiteral("prod:1001")).size() == 1
			&& store.historyFor(QStringLiteral("prod:1001")).front().repeatCount == 3,
			"coalesced history record tracks repeat count");

		// Separate account stays strictly isolated.
		NoticeRecord other;
		other.accountKey = QStringLiteral("prod:2002");
		other.accountLabel = QStringLiteral("Bob");
		other.summary = QStringLiteral("Bob notice");
		other.coalesceKey = QStringLiteral("archive:gap:prod:2002");
		store.record(other, true);
		Check(store.pendingCount(QStringLiteral("prod:2002")) == 1, "second account has its own bucket");

		// Logout clears only that account.
		store.clearAccount(QStringLiteral("prod:1001"));
		Check(store.historyFor(QStringLiteral("prod:1001")).empty()
			&& store.pendingCount(QStringLiteral("prod:1001")) == 0,
			"logout clears account history and pending queue");
		Check(store.historyFor(QStringLiteral("prod:2002")).size() == 1,
			"clearing one account does not affect another account");

		const auto appKey = QString::fromLatin1(kAppAccountKey);
		NoticeRecord removal;
		removal.accountKey = appKey;
		removal.summary = QStringLiteral("Removal pending");
		removal.coalesceKey = QString::fromLatin1(kArchiveRemovalNoticeKey);
		const auto removalId = store.record(removal, true);
		Check(store.record(removal, true) == removalId && store.pendingCount(appKey) == 1,
			"archive removal reports from different accounts share one app pending entry");
		store.clearAccount(r1.accountKey);
		const auto pendingRemoval = store.takeNextPending(appKey);
		Check(pendingRemoval && pendingRemoval->id == removalId && pendingRemoval->repeatCount == 2,
			"shared archive removal warning remains deliverable after logout");

		other.primaryActionLabel = QStringLiteral("Open settings");
		const auto actionableId = store.record(other, true);
		other.primaryActionLabel.clear();
		Check(store.record(other, false) == actionableId
			&& store.pendingCount(other.accountKey) == 0
			&& store.findById(other.accountKey, actionableId)->primaryActionLabel.isEmpty(),
			"an already delivered passive update removes its stale queued action");

		// Cap enforced at kMaxHistoryPerAccount.
		for (int i = 0; i < kMaxHistoryPerAccount + 15; ++i) {
			NoticeRecord n;
			n.accountKey = QStringLiteral("prod:2002");
			n.summary = QStringLiteral("Entry %1").arg(i);
			store.record(n, true);
		}
		Check(int(store.historyFor(QStringLiteral("prod:2002")).size()) == kMaxHistoryPerAccount,
			"per-account history is capped at kMaxHistoryPerAccount");
		Check(store.pendingCount(QStringLiteral("prod:2002")) == kMaxPendingPerAccount
			&& store.droppedPendingCount(QStringLiteral("prod:2002")) > 0,
			"pending queue is capped and tracks dropped overflow count");

		// Clear history also clears pending entries so nothing cleared reappears.
		store.clearHistoryAndPending(QStringLiteral("prod:2002"));
		Check(store.historyFor(QStringLiteral("prod:2002")).empty()
			&& store.pendingCount(QStringLiteral("prod:2002")) == 0,
			"clearHistoryAndPending clears both history and pending queue");
	}

	// 6. Local chat-name resolution: resolves visible peer ids in peer keys, keeps hidden/unknown as raw id, ignores non-peer numbers.
	{
		const auto lookup = [](uint64_t id) -> std::optional<QString> {
			switch (id) {
			case 12: return QStringLiteral("User");
			case 3456: return QStringLiteral("Group");
			case 78901: return QStringLiteral("Channel");
			default: return std::nullopt;
			}
		};
		const auto resolved = ResolvePeerIdsInConfigValue(
			QStringLiteral("[12, 3_456, +78901, 100999999, -3456, -1000000078901]"),
			lookup);
		Check(resolved == QStringLiteral("[User, Group, Channel, 100999999, -3456, -1000000078901]"),
			"bare IDs resolve and hidden, unknown, and Bot API IDs stay literal");
		Check(IsPeerIdConfigKey(QStringLiteral("lists.work"), QStringLiteral("members"))
			&& IsPeerIdConfigKey(QStringLiteral("presets.work.views.list"), QStringLiteral("pinned"))
			&& !IsPeerIdConfigKey(QStringLiteral("notifications"), QStringLiteral("preview_always"))
			&& !IsPeerIdConfigKey(QStringLiteral("peek"), QStringLiteral("delay_seconds")),
			"only settings storing bare IDs get bare ID lookup");
		const auto quoted = QStringLiteral("[\"channel:78901\", \"[12]\"]");
		Check(ResolvePeerIdsInConfigValue(quoted, lookup) == quoted,
			"numbers in strings are never rewritten");
	}

	// 7. Plain-text copy formatting.
	{
		NoticeRecord rec;
		rec.accountLabel = QStringLiteral("Alice");
		rec.scope = QStringLiteral("Settings reload");
		rec.summary = QStringLiteral("2 changes loaded");
		rec.rows.push_back({
			.label = QStringLiteral("Work list (lists.work.members)"),
			.before = QStringLiteral("[Design Team]"),
			.after = QStringLiteral("[Design Team, 100999999]"),
			.tag = QStringLiteral("Used here"),
		});
		const auto txt = rec.plainText();
		Check(txt.contains(QStringLiteral("Alice  ·  Settings reload"))
			&& txt.contains(QStringLiteral("[Design Team] -> [Design Team, 100999999]")),
			"plainText produces clean readable copy output");
	}

	std::cout << gChecks << " checks, " << gFailures << " failures\n";
	return gFailures ? 1 : 0;
}
