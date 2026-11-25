#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>
#include <vector>

namespace Purple {

enum class SyncMergeChoice { Unset, Keep, Drop, Local, Remote };

struct SyncMergeDifference {
	enum class Kind { Addition, Conflict, Order };
	Kind kind;
	QString path;
	QString local;
	QString remote;
};

struct SyncMergeReview {
	QByteArray local;
	QByteArray remote;
	std::vector<SyncMergeDifference> differences;
};

[[nodiscard]] std::optional<SyncMergeReview> ReviewSyncMerge(
	const QByteArray &local,
	const QByteArray &remote);
[[nodiscard]] std::optional<QByteArray> BuildSyncMerge(
	const SyncMergeReview &review,
	const std::vector<SyncMergeChoice> &choices);

} // namespace Purple
