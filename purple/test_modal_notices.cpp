#include "purple/purple_notice_state.h"
#include "purple/purple_diagnostics.h"
#include "purple/purple_list_selection.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

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
	{
		using Identity = std::pair<QString, uint64_t>;
		const auto account = QStringLiteral("account");
		const auto app = QStringLiteral("app");
		const auto identities = std::vector<Identity>{
			{ app, 3 }, { account, 1 }, { app, 1 }, { account, 3 },
		};
		const auto all = std::vector<int>{ 0, 1, 2, 3 };
		const auto two = std::vector<int>{ 0, 3 };
		const auto at = [&](int index) { return identities[index]; };
		auto selection = Purple::ListSelection<Identity>();
		const auto narrowed = selection.refilter(two, at(3), 3, true, at);
		Check(narrowed == 1 && at(two[narrowed]) == Identity(account, 3),
			"history retains the account-qualified identity when two scopes share the ID");
		const auto widened = selection.refilter(all, at(3), narrowed, false, at);
		Check(widened == 3 && at(all[widened]) == Identity(account, 3),
			"clearing the filter restores the original history row and its action target");
		const auto filteredOut = std::vector<int>{ 0, 2 };
		Check(selection.refilter(filteredOut, at(3), 3, true, at) == 1,
			"an app notice with the same ID cannot take the account notice's selection");
	}
	{
		namespace Diagnostic = Purple::Diagnostics;
		auto scratch = QTemporaryDir();
		Check(scratch.isValid(), "diagnostics use an isolated temporary directory");
		auto journal = Diagnostic::Journal();
		journal.start(scratch.path());
		const auto path = scratch.filePath(QString::fromLatin1(Diagnostic::kLogFile));
		Check(!journal.append(u"Telegram: never copied to the local log"),
			"only Purple lines enter the journal");
		Check(journal.append(u"Purple Archive: Could not discard the account's archive; reason=discard-held "
			"peer=123456 message_id=98765 duration_ms=5400 count=23 sqlite_code=2067 errno=13 "
			"token=fixturetoken /Users/fixture/archive +49 170 1234567 fixture@example.org"),
			"archive diagnostics written");
		Check(journal.append(u"Purple Settings: fixture section"), "multiple subsystems are retained");
		const auto data = [&] {
			auto file = QFile(path);
			return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
		}();
		Check(data.contains("reason=discard-held") && data.contains("sqlite_code=2067")
			&& data.contains("errno=13") && data.contains("duration_ms=5400") && data.contains("count=23"),
			"archive failure retains exact reason, codes, durations and counts");
		Check(data.size() > 4 && data.at(4) == '-' && data.contains("Z [Archive]"),
			"archive line has a UTC timestamp and subsystem");
		Check(!data.contains("123456") && !data.contains("98765") && !data.contains("fixturetoken")
			&& !data.contains("fixture@example.org") && !data.contains("/Users/fixture")
			&& !data.contains("+49") && data.contains("~/archive"),
			"journal stores masked IDs, credentials, contacts and home paths");
#ifdef Q_OS_UNIX
		const auto privatePath = [](const QString &path) {
			const auto flags = QFileInfo(path).permissions();
			return flags.testFlag(QFile::ReadOwner) && flags.testFlag(QFile::WriteOwner)
				&& !(flags & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
					| QFile::ReadOther | QFile::WriteOther | QFile::ExeOther | QFile::ExeOwner));
		};
		Check(privatePath(path), "diagnostics use mode 0600");
#endif
		const auto copy = journal.copy(u"Local archive",
			u"Visible heading\nVisible archive notice and detail; peer=123456");
		Check(copy.startsWith(QStringLiteral("Visible heading\nVisible archive notice"))
			&& copy.count(QStringLiteral("peer=id-1")) == 2
			&& copy.contains(QStringLiteral("message_id=id-2"))
			&& !copy.contains(QStringLiteral("fixture section")),
			"copy retains visible notice text and matching subsystem with stable pseudonyms");
		for (auto i = 0; i != 70; ++i) {
			journal.append(u"Purple Archive: counted fixture scan");
		}
		Check(journal.copy(u"Local archive", {}).count(QStringLiteral("counted fixture scan")) == 60,
			"copy is limited to 60 matching lines");
		auto accountNotices = std::vector<NoticeRecord>(32);
		auto noticeId = uint64_t(0);
		for (auto &record : accountNotices) {
			record.id = ++noticeId;
			record.scope = QStringLiteral("Local archive");
			record.summary = QStringLiteral("peer=123456 token=notice-secret");
			record.timestampMs = 1;
		}
		accountNotices.back().repeatCount = 3;
		accountNotices.back().rows.push_back({
			.label = QStringLiteral("members"),
			.before = QStringLiteral("[+123_456, 98765]"),
			.after = QStringLiteral("[+123456]"),
			.hasConfigPeerIds = true,
		});
		const auto appNotices = std::vector<NoticeRecord>{ {
			.scope = QStringLiteral("Local archive"),
			.summary = QStringLiteral("The account's archive removal is pending."),
			.timestampMs = 2,
		} };
		const auto notices = Diagnostic::ExportNotices(accountNotices, appNotices);
		const auto exported = journal.exportText(notices);
		Check(exported.count(QStringLiteral("[This account]")) == 31
			&& exported.endsWith(QStringLiteral("The account's archive removal is pending.\n"))
			&& exported.contains(QStringLiteral("Repeated 3 times"))
			&& exported.count(QStringLiteral("peer=id-1")) == 32
			&& exported.contains(QStringLiteral("[id-1, id-2] -> [id-1]"))
			&& !exported.contains(QStringLiteral("notice-secret")),
			"recent notices share masking with the complete diagnostic export");
		auto restarted = Diagnostic::Journal();
		restarted.start(scratch.path());
		Check(restarted.exportText(notices) == exported, "restart reads existing file and preserves aliasing");
		Check(restarted.clear() && QFileInfo(path).size() == 0
			&& journal.exportText({}).trimmed().isEmpty(),
			"clear truncates the persistent log and is immediately visible");
		auto rotating = Diagnostic::Journal(85);
		rotating.start(scratch.path());
		for (const auto line : { u"Purple Archive: first fixture",
			u"Purple Archive: second fixture", u"Purple Archive: third fixture" }) {
			Check(rotating.append(line), "rotating write succeeds");
		}
		const auto rotated = rotating.exportText({});
		Check(!rotated.contains(QStringLiteral("first fixture"))
			&& rotated.indexOf(QStringLiteral("second fixture")) < rotated.indexOf(QStringLiteral("third fixture"))
			&& rotated.contains(QStringLiteral("second fixture"))
			&& QFileInfo(path).size() <= 85 && QFileInfo(path + QStringLiteral(".1")).size() <= 85,
			"rotation retains the two most recent bounded segments in order");
#ifdef Q_OS_UNIX
		Check(privatePath(path + QStringLiteral(".1")), "rotated segment keeps mode 0600");
#endif
		Check(rotating.clear() && QFileInfo(path + QStringLiteral(".1")).size() == 0,
			"clear also truncates the rotated segment");
	}
	for (const auto modal : { true, false }) {
		auto store = AccountNoticeStore();
		auto context = DeliveryContext{ .modalEnabled = modal, .privacyScreenActive = true };
		const auto notice = NoticeRecord{
			.accountKey = QStringLiteral("fixture"),
			.summary = QStringLiteral("covered fixture"),
		};
		const auto submitted = store.submit(notice, context, false, {}, 1000);
		Check(submitted.queued && submitted.id && store.pendingCount(notice.accountKey) == 1,
			"router retains submission while covered");
		Check(!store.takeNextPending(notice.accountKey, context), "router keeps cover silent");
		context.privacyScreenActive = false;
		context.canInterruptWindow = true;
		context.statusBarVisible = true;
		Check(!store.takeNextPending(QStringLiteral("another"), context), "wrong account cannot take queued notice");
		const auto delivered = store.takeNextPending(notice.accountKey, context);
		Check(delivered && delivered->notice.record.id == submitted.id
			&& delivered->route == DeliveryRoute::OpenDialogNow,
			"router delivers retained record after uncovering");
		Check(!store.takeNextPending(notice.accountKey, context), "retained record is delivered once");
	}

	// 1. Modal keys off still presents a dialog when the window is available.
	{
		DeliveryContext ctx;
		ctx.modalEnabled = false;
		ctx.statusBarVisible = true;
		Check(DecideDelivery(NoticeKind::Dialog, true, false, ctx) == DeliveryRoute::OpenDialogNow,
			"modal off: background notice opens a dialog");
		Check(DecideDelivery(NoticeKind::Dialog, false, false, ctx) == DeliveryRoute::OpenDialogNow,
			"modal off: foreground notice opens a dialog");
		Check(DecideDelivery(NoticeKind::Ack, false, false, ctx) == DeliveryRoute::StockToast,
			"modal off: ack -> stock toast");
		Check(ShouldFlushPendingDialog(ctx), "modal off: pending dialog can flush");
		Check(!HistoryAccessible(ctx), "modal off: modal history layer unavailable");
		ctx.recentTyping = true;
		Check(DecideDelivery(NoticeKind::Dialog, false, false, ctx) == DeliveryRoute::QueueDialogWithCue,
			"modal off: a foreground error waits while typing");
		Check(!PendingRoute(NoticeRecord{}, ctx, false), "pending dialogs also wait while typing");
		ctx.recentTyping = false;
		ctx.hasImePreedit = true;
		Check(!PendingRoute(NoticeRecord{}, ctx, false), "pending dialogs wait for IME composition");
		ctx.hasImePreedit = false;
		ctx.canInterruptWindow = false;
		Check(!PendingRoute(NoticeRecord{}, ctx, false), "pending dialogs wait for the active window");
		ctx.canInterruptWindow = true;
		ctx.inInsertMode = ctx.hasPendingPrefix = true;
		const auto background = NoticeRecord{ .background = true };
		Check(DecideDelivery(NoticeKind::Dialog, true, false, ctx) == DeliveryRoute::OpenDialogNow,
			"disabled modal keys ignore old Insert and prefix state for new background notices");
		Check(PendingRoute(background, ctx, false) == DeliveryRoute::OpenDialogNow,
			"disabling modal keys releases a queued background notice with old modal state");
		ctx.hasBlockingLayerOrBox = true;
		Check(!PendingRoute(background, ctx, false), "a stock layer still holds a background dialog");
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

	// 5. Per-account coalescing, bounded history, and logout clearing.
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
		const auto pendingRemoval = store.takeNextPending(r1.accountKey, {});
		Check(pendingRemoval && pendingRemoval->notice.record.id == removalId
			&& pendingRemoval->notice.record.repeatCount == 2,
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
		Check(store.pendingCount(other.accountKey) == kMaxHistoryPerAccount + 15,
			"history overflow leaves durable pending notices queued");
		const auto modal = DeliveryContext{ .modalEnabled = true };
		const auto oldest = store.takeNextPending(other.accountKey, modal);
		Check(oldest && oldest->notice.record.summary == QStringLiteral("Entry 0")
			&& store.findById(other.accountKey, oldest->notice.record.id).has_value(),
			"modal delivery restores actions and copy history for old pending notices");

		// Clear history also clears pending entries so nothing cleared reappears.
		store.clearHistoryAndPending(QStringLiteral("prod:2002"));
		Check(store.historyFor(QStringLiteral("prod:2002")).empty()
			&& store.pendingCount(QStringLiteral("prod:2002")) == 0,
			"clearHistoryAndPending clears both history and pending queue");
	}

	{
		AccountNoticeStore store;
		const auto a = QStringLiteral("prod:1001");
		const auto b = QStringLiteral("prod:2002");
		const auto stock = DeliveryContext{};
		const auto locked = DeliveryContext{ .passcodeLocked = true };
		const auto hidden = DeliveryContext{ .privacyScreenActive = true };
		const auto normal = DeliveryContext{ .modalEnabled = true, .statusBarVisible = true };
		auto notice = NoticeRecord{
			.accountKey = a,
			.summary = QStringLiteral("Old"),
			.coalesceKey = QStringLiteral("same"),
		};
		int fallback = 0;
		const auto id = store.record(notice, true, [&] { fallback = 1; }, 50);
		notice.summary = QStringLiteral("Latest");
		Check(store.hasPendingKey(notice)
			&& store.record(notice, true, [&] { fallback = 2; }, 100) == id,
			"the latest coalesced event replaces its pending toast callback");
		Check(!store.takeNextPending(a, locked) && !store.takeNextPending(a, hidden)
			&& !store.takeNextPending(b, stock) && fallback == 0,
			"locked, private, and other windows cannot consume an account event");
		auto ack = NoticeRecord{
			.accountKey = a,
			.summary = QStringLiteral("Ack"),
			.kind = NoticeKind::Ack,
		};
		const auto ackId = store.record(ack, true, {}, 110);
		const auto first = store.takeNextPending(a, stock);
		Check(first && first->route == DeliveryRoute::OpenDialogNow
			&& first->notice.record.summary == QStringLiteral("Latest"),
			"after unlock the latest notice opens a dialog with modal keys off");
		if (first && first->notice.fallbackToast) {
			first->notice.fallbackToast();
		}
		Check(fallback == 2 && !store.isPending(a, id),
			"only the latest fallback runs and the notice is consumed once");
		Check(!store.takeNextPending(a, stock, true)
			&& !store.takeNextPending(a, normal, true) && store.isPending(a, ackId),
			"an active presentation prevents a burst even during a mode change");
		const auto second = store.takeNextPending(a, normal);
		Check(second && second->route == DeliveryRoute::StatusLineMessage
			&& second->notice.record.id == ackId && !store.takeNextPending(a, stock),
			"enabling modal keys routes the remaining ack to the status line once");

		store.record(notice, true, {}, 120);
		auto typing = normal;
		typing.inInsertMode = true;
		Check(!store.takeNextPending(a, typing), "modal notice waits while typing");
		const auto afterDisable = store.takeNextPending(a, stock);
		Check(afterDisable && afterDisable->route == DeliveryRoute::OpenDialogNow
			&& !afterDisable->notice.fallbackToast && !store.takeNextPending(a, normal),
			"disabling modal keys still opens one dialog");

		const auto expired = store.record(ack, true, {}, 0);
		const auto fresh = store.record(ack, true, {}, kPendingAckLifetimeMs - 1);
		ack.severity = NoticeSeverity::Warning;
		const auto warning = store.record(ack, true, {}, 0);
		ack.severity = NoticeSeverity::Error;
		const auto error = store.record(ack, true, {}, 0);
		ack.severity = NoticeSeverity::Info;
		ack.persistent = true;
		const auto persistent = store.record(ack, true, {}, 0);
		const auto durable = store.record(notice, true, {}, 0);
		store.forgetExpiredAcks(kPendingAckLifetimeMs);
		Check(!store.isPending(a, expired) && store.isPending(a, fresh)
			&& store.isPending(a, warning) && store.isPending(a, error)
			&& store.isPending(a, persistent) && store.isPending(a, durable),
			"only old transient informational acknowledgements expire");
		store.clearAccount(a);
		Check(!store.takeNextPending(a, stock) && !store.takeNextPending(b, stock),
			"logout drops all pending acknowledgements and notices for its account");
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
