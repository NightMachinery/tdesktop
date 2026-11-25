#pragma once

#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace Purple::ModalNotices {

inline constexpr int kMaxHistoryPerAccount = 64;
inline constexpr int kPendingAckLifetimeMs = 30000;
inline constexpr int kAckDurationInfoMs = 3500;
inline constexpr int kAckDurationErrorMs = 6500;
inline constexpr auto kAppAccountKey = "app";
inline constexpr auto kArchiveRemovalNoticeKey = "archive:removal_pending";

enum class NoticeSeverity {
	Info,
	Warning,
	Error,
};

enum class NoticeKind {
	Dialog,
	Ack,
};

enum class DeliveryRoute {
	StockToast,
	OpenDialogNow,
	QueueDialogWithCue,
	QueueSilentUntilAvailable,
	StatusLineMessage,
	StatusLinePersistent,
};

struct NoticeRow {
	QString label;
	QString before;
	QString after;
	QString tag;
	QString detail;
	bool isWarning = false;
	bool isInactive = false;
	bool hasConfigPeerIds = false;
};

struct NoticeRecord {
	uint64_t id = 0;
	QString accountKey;
	QString accountLabel;
	QString scope;
	QString title;
	QString summary;
	QString primaryActionLabel;
	QString coalesceKey;
	NoticeSeverity severity = NoticeSeverity::Info;
	NoticeKind kind = NoticeKind::Dialog;
	bool background = false;
	bool persistent = false;
	qint64 timestampMs = 0;
	int repeatCount = 1;
	std::vector<NoticeRow> rows;

	[[nodiscard]] QString formattedTitle() const {
		if (!title.isEmpty()) {
			return title;
		}
		if (!accountLabel.isEmpty() && !scope.isEmpty()) {
			return accountLabel + QStringLiteral("  ·  ") + scope;
		}
		return !scope.isEmpty() ? scope : accountLabel;
	}

	[[nodiscard]] QString plainText() const {
		QStringList out;
		const auto hdr = formattedTitle();
		if (!hdr.isEmpty()) {
			out.push_back(repeatCount > 1
				? (hdr + QStringLiteral(" (x%1)").arg(repeatCount))
				: hdr);
		}
		if (!summary.isEmpty()) {
			out.push_back(summary);
		}
		for (const auto &row : rows) {
			QString line;
			if (!row.label.isEmpty()) {
				line = row.label;
			}
			if (!row.before.isEmpty() || !row.after.isEmpty()) {
				const auto change = row.before + QStringLiteral(" -> ") + row.after;
				line = line.isEmpty() ? change : (line + QStringLiteral(": ") + change);
			}
			if (!row.detail.isEmpty()) {
				line = line.isEmpty() ? row.detail : (line + QStringLiteral(" — ") + row.detail);
			}
			if (!row.tag.isEmpty()) {
				line += QStringLiteral(" [") + row.tag + QStringLiteral("]");
			}
			if (!line.isEmpty()) {
				out.push_back(QStringLiteral("- ") + line);
			}
		}
		return out.join(QChar('\n'));
	}
};

struct DeliveryContext {
	bool modalEnabled = false;
	bool passcodeLocked = false;
	bool privacyScreenActive = false;
	bool inInsertMode = false;
	bool hasImePreedit = false;
	bool hasPendingPrefix = false;
	bool recentTyping = false;
	bool hasBlockingLayerOrBox = false;
	bool inZenMode = false;
	bool statusBarVisible = false;
	bool canInterruptWindow = true;
};

[[nodiscard]] inline int AckDurationMs(NoticeSeverity severity) {
	return (severity == NoticeSeverity::Info)
		? kAckDurationInfoMs
		: kAckDurationErrorMs;
}

[[nodiscard]] inline DeliveryRoute DecideDelivery(
		NoticeKind kind,
		bool background,
		bool persistent,
		const DeliveryContext &ctx) {
	// Privacy/lock check comes FIRST: never show a revealing dialog, status cue,
	// or stock toast fallback while passcode-locked or under the privacy screen.
	if (ctx.passcodeLocked || ctx.privacyScreenActive) {
		return DeliveryRoute::QueueSilentUntilAvailable;
	}
	if (kind == NoticeKind::Dialog) {
		if (!ctx.canInterruptWindow || ctx.recentTyping || ctx.hasImePreedit
			|| (background
				&& (ctx.inInsertMode
				|| ctx.hasPendingPrefix
				|| ctx.hasBlockingLayerOrBox))) {
			return DeliveryRoute::QueueDialogWithCue;
		}
		return DeliveryRoute::OpenDialogNow;
	}
	if (!ctx.modalEnabled) {
		return DeliveryRoute::StockToast;
	}
	// Group B (Ack / single-line error / persistent status cue).
	if (!ctx.statusBarVisible && !ctx.inZenMode) {
		return DeliveryRoute::StockToast;
	}
	return persistent
		? DeliveryRoute::StatusLinePersistent
		: DeliveryRoute::StatusLineMessage;
}

[[nodiscard]] inline bool ShouldFlushPendingDialog(const DeliveryContext &ctx) {
	return !ctx.passcodeLocked
		&& !ctx.privacyScreenActive
		&& !ctx.recentTyping
		&& !ctx.inInsertMode
		&& !ctx.hasImePreedit
		&& !ctx.hasPendingPrefix
		&& !ctx.hasBlockingLayerOrBox
		&& ctx.canInterruptWindow;
}

[[nodiscard]] inline bool HistoryAccessible(const DeliveryContext &ctx) {
	return ctx.modalEnabled
		&& !ctx.passcodeLocked
		&& !ctx.privacyScreenActive;
}

[[nodiscard]] inline std::optional<DeliveryRoute> PendingRoute(
		const NoticeRecord &record,
		const DeliveryContext &ctx,
		bool presentationBusy) {
	if (presentationBusy || ctx.passcodeLocked || ctx.privacyScreenActive) {
		return std::nullopt;
	}
	if (record.kind == NoticeKind::Dialog
		&& (!ctx.canInterruptWindow || ctx.recentTyping || ctx.hasImePreedit
			|| ((ctx.modalEnabled || record.background) && !ShouldFlushPendingDialog(ctx)))) {
		return std::nullopt;
	}
	return DecideDelivery(record.kind, false, record.persistent, ctx);
}

[[nodiscard]] inline bool IsPeerIdConfigKey(
		const QString &table,
		const QString &key) {
	const auto k = key.toCaseFolded();
	const auto t = table.toCaseFolded();
	return (k == QStringLiteral("members") && t.startsWith(QStringLiteral("lists.")))
		|| (k == QStringLiteral("pinned") && t.startsWith(QStringLiteral("presets.")));
}

// Replaces numeric PeerId tokens in peer-list config values using a caller-provided
// local lookup callback. The caller must resolve ONLY from the notice's own account
// session and return std::nullopt for unknown or Work-Mode-hidden chats so they
// safely fall back to their raw id.
[[nodiscard]] inline QString ResolvePeerIdsInConfigValue(
		const QString &rawValue,
		const std::function<std::optional<QString>(uint64_t)> &lookup) {
	if (rawValue.contains(u'"') || rawValue.contains(u'\'')) {
		return rawValue;
	}
	static const auto kTokenRe = QRegularExpression(
		QStringLiteral(R"((?<=[\[,])\s*\K\+?[0-9]+(?:_[0-9]+)*(?=\s*[,\]]))"));
	QString out;
	out.reserve(rawValue.size() + 32);
	qsizetype lastPos = 0;
	auto it = kTokenRe.globalMatch(rawValue);
	while (it.hasNext()) {
		const auto m = it.next();
		const auto token = m.capturedView();
		const auto start = m.capturedStart();
		out.append(QStringView(rawValue).mid(lastPos, start - lastPos));
		bool ok = false;
		auto digits = token.toString();
		const auto id = digits.remove(u'_').toULongLong(&ok);
		const auto resolved = (ok && id) ? lookup(id) : std::nullopt;
		out.append(resolved ? QStringView(*resolved) : token);
		lastPos = start + token.size();
	}
	out.append(QStringView(rawValue).mid(lastPos));
	return out;
}

struct PendingNotice {
	NoticeRecord record;
	std::function<void()> fallbackToast;
	qint64 enqueuedAtMs = 0;
};

struct PendingDelivery {
	PendingNotice notice;
	DeliveryRoute route = DeliveryRoute::StockToast;
};

struct NoticeSubmission {
	DeliveryRoute route = DeliveryRoute::StockToast;
	uint64_t id = 0;
	bool queued = false;
};

class AccountNoticeStore final {
public:
	[[nodiscard]] NoticeSubmission submit(
			const NoticeRecord &rec,
			const DeliveryContext &ctx,
			bool canPresent,
			std::function<void()> fallback,
			qint64 now) {
		const auto route = DecideDelivery(rec.kind, rec.background, rec.persistent, ctx);
		const auto queued = !canPresent || hasPendingKey(rec)
			|| route == DeliveryRoute::QueueDialogWithCue
			|| route == DeliveryRoute::QueueSilentUntilAvailable;
		const auto id = (queued || ctx.modalEnabled || rec.kind == NoticeKind::Dialog)
			? record(rec, queued, queued ? std::move(fallback) : std::function<void()>(), now)
			: uint64_t(0);
		return { route, id, queued };
	}

	uint64_t record(
			NoticeRecord rec,
			bool enqueuePending,
			std::function<void()> fallbackToast = {},
			qint64 now = 0) {
		const auto key = rec.accountKey;
		auto &bucket = _accounts[key];
		auto pending = bucket.pending.end();
		if (!rec.coalesceKey.isEmpty()) {
			const auto matches = [&](const NoticeRecord &existing) {
				return existing.coalesceKey == rec.coalesceKey;
			};
			pending = std::find_if(bucket.pending.begin(), bucket.pending.end(),
				[&](const PendingNotice &item) { return matches(item.record); });
			const auto previous = std::find_if(bucket.history.begin(), bucket.history.end(), matches);
			if (pending != bucket.pending.end()) {
				rec.id = pending->record.id;
				rec.repeatCount += pending->record.repeatCount;
			} else if (previous != bucket.history.end()) {
				rec.id = previous->id;
				rec.repeatCount += previous->repeatCount;
			}
		}
		if (!rec.id) {
			rec.id = ++_nextId;
		}
		remember(rec);

		if (enqueuePending) {
			auto item = PendingNotice{ rec, std::move(fallbackToast), now };
			if (pending != bucket.pending.end()) {
				*pending = std::move(item);
			} else {
				bucket.pending.push_back(std::move(item));
			}
		} else if (pending != bucket.pending.end()) {
			bucket.pending.erase(pending);
		}
		return rec.id;
	}

	void forgetExpiredAcks(qint64 now) {
		for (auto &[key, bucket] : _accounts) {
			std::erase_if(bucket.pending, [&](const PendingNotice &item) {
				return item.record.kind == NoticeKind::Ack
					&& item.record.severity == NoticeSeverity::Info
					&& !item.record.persistent
					&& now - item.enqueuedAtMs >= kPendingAckLifetimeMs;
			});
		}
	}

	[[nodiscard]] std::optional<PendingDelivery> takeNextPending(
			const QString &accountKey,
			const DeliveryContext &ctx,
			bool presentationBusy = false) {
		const auto appKey = QString::fromLatin1(kAppAccountKey);
		for (const auto &key : { accountKey, appKey }) {
			const auto it = _accounts.find(key);
			if (it == _accounts.end() || it->second.pending.empty()) {
				continue;
			}
			auto &pending = it->second.pending;
			const auto route = PendingRoute(pending.front().record, ctx, presentationBusy);
			if (!route) {
				continue;
			}
			auto next = std::move(pending.front());
			pending.erase(pending.begin());
			if (ctx.modalEnabled && !findById(key, next.record.id)) {
				remember(next.record);
			}
			return PendingDelivery{ std::move(next), *route };
		}
		return std::nullopt;
	}

	[[nodiscard]] bool isPending(const QString &accountKey, uint64_t id) const {
		const auto it = _accounts.find(accountKey);
		return it != _accounts.end() && std::any_of(
			it->second.pending.begin(), it->second.pending.end(),
			[=](const PendingNotice &item) { return item.record.id == id; });
	}

	[[nodiscard]] bool hasPendingKey(const NoticeRecord &record) const {
		const auto it = _accounts.find(record.accountKey);
		return !record.coalesceKey.isEmpty() && it != _accounts.end()
			&& std::any_of(it->second.pending.begin(), it->second.pending.end(),
				[&](const PendingNotice &item) {
					return item.record.coalesceKey == record.coalesceKey;
				});
	}

	[[nodiscard]] int pendingCount(const QString &accountKey) const {
		const auto it = _accounts.find(accountKey);
		return (it == _accounts.end()) ? 0 : int(it->second.pending.size());
	}

	[[nodiscard]] const std::vector<NoticeRecord> &historyFor(
			const QString &accountKey) const {
		static const auto kEmpty = std::vector<NoticeRecord>();
		const auto it = _accounts.find(accountKey);
		return (it == _accounts.end()) ? kEmpty : it->second.history;
	}

	[[nodiscard]] std::optional<NoticeRecord> findById(
			const QString &accountKey,
			uint64_t id) const {
		const auto it = _accounts.find(accountKey);
		if (it == _accounts.end()) {
			return std::nullopt;
		}
		for (const auto &rec : it->second.history) {
			if (rec.id == id) {
				return rec;
			}
		}
		return std::nullopt;
	}

	void clearAccount(const QString &accountKey) {
		_accounts.erase(accountKey);
	}

	void clearHistoryAndPending(const QString &accountKey) {
		_accounts.erase(accountKey);
	}

private:
	void remember(const NoticeRecord &record) {
		auto &history = _accounts[record.accountKey].history;
		std::erase_if(history, [&](const NoticeRecord &item) { return item.id == record.id; });
		history.insert(history.begin(), record);
		if (int(history.size()) > kMaxHistoryPerAccount) {
			history.resize(kMaxHistoryPerAccount);
		}
	}

	struct AccountBucket {
		std::vector<NoticeRecord> history;
		std::vector<PendingNotice> pending;
	};

	uint64_t _nextId = 0;
	std::map<QString, AccountBucket> _accounts;
};

} // namespace Purple::ModalNotices
