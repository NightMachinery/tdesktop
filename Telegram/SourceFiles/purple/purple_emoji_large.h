#pragma once

#include <QtCore/QSize>
#include <QtCore/QString>
#include <deque>
#include <map>
#include <optional>
#include <set>

namespace Purple {

inline constexpr auto kEmojiLargePixels = 512;
inline constexpr auto kEmojiLargeMinimumBytes = qint64(100) * 1024 * 1024;
inline constexpr auto kEmojiLargeDefaultBytes = qint64(2) * 1024 * 1024 * 1024;
inline constexpr auto kEmojiLargeMaximumBytes = qint64(8) * 1024 * 1024 * 1024;

enum class EmojiExportSource { Tgs, Webm, Webp };
enum class EmojiLargeAdmission { Queued, Duplicate, Cooldown, Full, Invalid };

[[nodiscard]] std::optional<quint64> EmojiLargeRequestId(const QString &name);
[[nodiscard]] QSize EmojiLargeRenderSize(EmojiExportSource source, QSize native);
[[nodiscard]] QString EmojiExportSourceName(EmojiExportSource source);
[[nodiscard]] qint64 NormalizeEmojiLargeCap(qint64 bytes);

class EmojiLargeRequestQueue final {
public:
	explicit EmojiLargeRequestQueue(size_t maximum = 64, qint64 spacingMs = 250);
	[[nodiscard]] EmojiLargeAdmission offer(quint64 id, qint64 now);
	[[nodiscard]] std::optional<quint64> take(qint64 now);
	void complete(quint64 id, bool success, qint64 now);
	void clear();
	[[nodiscard]] size_t size() const;
	[[nodiscard]] bool pending() const;

private:
	const size_t _maximum;
	const qint64 _spacingMs;
	qint64 _nextAt = 0;
	std::deque<quint64> _pending;
	std::optional<quint64> _active;
	std::set<quint64> _present;
	std::map<quint64, qint64> _failed;

};

struct EmojiSetTitle {
	QString title;
	QString shortName;
};

class EmojiSetTitleCache final {
public:
	explicit EmojiSetTitleCache(size_t maximum = 101025);
	void known(quint64 id, EmojiSetTitle title);
	[[nodiscard]] std::optional<EmojiSetTitle> lookup(quint64 id) const;
	[[nodiscard]] bool canBegin(quint64 id, qint64 now) const;
	[[nodiscard]] bool begin(quint64 id, qint64 now);
	void defer(quint64 id);
	[[nodiscard]] std::optional<qint64> retryAt(qint64 now) const;
	void complete(quint64 id, EmojiSetTitle title);
	void failed(quint64 id, qint64 now);
	void clear();
	[[nodiscard]] std::optional<quint64> active() const;

private:
	struct Entry {
		std::optional<EmojiSetTitle> title;
		qint64 retryAt = 0;
	};
	const size_t _maximum;
	std::optional<quint64> _active;
	std::map<quint64, Entry> _sets;

};

}
