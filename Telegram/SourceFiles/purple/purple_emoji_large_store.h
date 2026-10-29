#pragma once

#include "purple/purple_emoji_export_store.h"
#include <memory>
#include <set>

class QDirIterator;

namespace Purple {

inline constexpr auto kEmojiExportMaintenancePage = size_t(256);
inline constexpr auto kEmojiLargeMaximumFiles = size_t(2048);
inline constexpr auto kEmojiLargeMaximumRequests = size_t(1024);

struct EmojiExportMaintenanceStats {
	size_t examined = 0;
	size_t removed = 0;
	size_t retained = 0;
	bool pending = false;
};

[[nodiscard]] EmojiExportMaintenanceStats EmojiLargePruneStats();

enum class EmojiExportCleanupResult { Done, Pending, Failed };

class EmojiExportCleanup final {
public:
	EmojiExportCleanup();
	~EmojiExportCleanup();
	[[nodiscard]] EmojiExportCleanupResult step(
		const QString &directory,
		const std::function<bool()> &current = {});
	[[nodiscard]] EmojiExportMaintenanceStats stats() const;

private:
	struct State;
	std::unique_ptr<State> _state;

};

[[nodiscard]] bool RecordEmojiExportCleanup(const QString &directory);
[[nodiscard]] bool EmojiExportCleanupRecorded(const QString &directory);

[[nodiscard]] QString EmojiLargePngName(quint64 id, int pixels);
[[nodiscard]] bool EnsureEmojiLargeDirectories(const QString &directory);
[[nodiscard]] bool WriteEmojiLargePng(
	const QString &directory,
	quint64 id,
	QImage image,
	std::optional<QColor> tint,
	const std::function<bool()> &current = {});
[[nodiscard]] bool TouchEmojiLargePng(
	const QString &directory,
	const QString &relative,
	const std::function<bool()> &current = {});
[[nodiscard]] bool PruneEmojiLargeExport(
	const QString &directory,
	qint64 maximumBytes,
	const std::function<bool()> &current = {},
	bool removeAll = false,
	QDateTime now = QDateTime::currentDateTimeUtc());
[[nodiscard]] bool AcknowledgeEmojiLargeRequest(
	const QString &directory,
	quint64 id,
	const std::function<bool()> &current = {});
[[nodiscard]] std::vector<EmojiExportEntry> MergeEmojiLargePublication(
	const std::vector<EmojiExportEntry> &previous,
	const std::vector<std::optional<EmojiExportEntry>> &ready,
	bool finished,
	const std::function<bool()> &current = {},
	const std::set<quint64> &retired = {});
[[nodiscard]] bool PublishEmojiLargeRequests(
	const QString &directory,
	std::vector<EmojiExportEntry> &entries,
	const QDateTime &generatedAt,
	const std::set<quint64> &acknowledge,
	const std::function<bool()> &current = {},
	const QString &controlSocket = {});
void ValidateEmojiLargeEntries(
	const QString &directory,
	std::vector<EmojiExportEntry> &entries);

[[nodiscard]] std::vector<quint64> ScanEmojiLargeRequests(
	const QString &directory,
	quint64 generation,
	const std::function<bool()> &current = {});

class EmojiLargeRequestScanner final {
public:
	EmojiLargeRequestScanner();
	~EmojiLargeRequestScanner();
	[[nodiscard]] std::vector<quint64> scan(
		const QString &directory,
		const std::function<bool()> &current = {},
		size_t examined = 256,
		size_t admitted = 16);
	void reset();
	[[nodiscard]] EmojiExportMaintenanceStats stats() const;
	[[nodiscard]] bool overflow() const;

private:
	QString _directory;
	std::unique_ptr<QDirIterator> _iterator;
	size_t _entries = 0;
	bool _overflow = false;
	bool _counting = true;
	EmojiExportMaintenanceStats _stats;

};

}
