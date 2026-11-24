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
inline constexpr int kMaxPendingPerAccount = 16;
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
	if (!ctx.modalEnabled) {
		return DeliveryRoute::StockToast;
	}
	if (kind == NoticeKind::Dialog) {
		if (background
			&& (!ctx.canInterruptWindow
				|| ctx.inInsertMode
				|| ctx.hasImePreedit
				|| ctx.hasPendingPrefix
				|| ctx.hasBlockingLayerOrBox)) {
			return DeliveryRoute::QueueDialogWithCue;
		}
		return DeliveryRoute::OpenDialogNow;
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
	return ctx.modalEnabled
		&& !ctx.passcodeLocked
		&& !ctx.privacyScreenActive
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

class AccountNoticeStore final {
public:
	uint64_t record(NoticeRecord rec, bool enqueuePending) {
		const auto key = rec.accountKey;
		auto &bucket = _accounts[key];
		uint64_t assignedId = 0;

		if (!rec.coalesceKey.isEmpty()) {
			auto it = std::find_if(
				bucket.history.begin(),
				bucket.history.end(),
				[&](const NoticeRecord &existing) {
					return existing.coalesceKey == rec.coalesceKey;
				});
			if (it != bucket.history.end()) {
				const auto nextRepeat = it->repeatCount + std::max(1, rec.repeatCount);
				assignedId = it->id;
				*it = rec;
				it->id = assignedId;
				it->repeatCount = nextRepeat;
				if (it != bucket.history.begin()) {
					auto updated = std::move(*it);
					bucket.history.erase(it);
					bucket.history.insert(bucket.history.begin(), std::move(updated));
				}
			}
		}

		if (assignedId == 0) {
			assignedId = ++_nextId;
			rec.id = assignedId;
			bucket.history.insert(bucket.history.begin(), rec);
			if (int(bucket.history.size()) > kMaxHistoryPerAccount) {
				bucket.history.resize(kMaxHistoryPerAccount);
			}
		}

		if (enqueuePending) {
			const auto &latest = bucket.history.front();
			if (!latest.coalesceKey.isEmpty()) {
				auto pit = std::find_if(
					bucket.pending.begin(),
					bucket.pending.end(),
					[&](const NoticeRecord &p) {
						return p.coalesceKey == latest.coalesceKey;
					});
				if (pit != bucket.pending.end()) {
					*pit = latest;
					return assignedId;
				}
			}
			bucket.pending.push_back(latest);
			if (int(bucket.pending.size()) > kMaxPendingPerAccount) {
				bucket.pending.erase(bucket.pending.begin());
				++bucket.droppedPending;
			}
		} else {
			std::erase_if(bucket.pending, [=](const NoticeRecord &pending) {
				return pending.id == assignedId;
			});
		}
		return assignedId;
	}

	[[nodiscard]] int droppedPendingCount(const QString &accountKey) const {
		const auto it = _accounts.find(accountKey);
		return (it == _accounts.end()) ? 0 : it->second.droppedPending;
	}

	[[nodiscard]] std::optional<NoticeRecord> takeNextPending(
			const QString &accountKey) {
		const auto it = _accounts.find(accountKey);
		if (it == _accounts.end() || it->second.pending.empty()) {
			return std::nullopt;
		}
		auto next = std::move(it->second.pending.front());
		it->second.pending.erase(it->second.pending.begin());
		return next;
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
	struct AccountBucket {
		std::vector<NoticeRecord> history;
		std::vector<NoticeRecord> pending;
		int droppedPending = 0;
	};

	uint64_t _nextId = 0;
	std::map<QString, AccountBucket> _accounts;
};

} // namespace Purple::ModalNotices
