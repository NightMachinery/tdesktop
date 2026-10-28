#pragma once

#include <QtCore/QtGlobal>
#include <map>
#include <optional>
#include <vector>

namespace Purple {

enum class EmojiResolveAdmission { Ready, Cooldown, Continue };

class EmojiExportQueue final {
public:
	explicit EmojiExportQueue(int budget = 100, qint64 cooldownMs = 60000);
	void begin(const std::vector<quint64> &ids, const std::vector<quint64> &extra);
	void clear();
	[[nodiscard]] std::optional<size_t> current() const;
	void next();
	void prioritize(size_t index);
	void continueSlice();
	[[nodiscard]] qint64 metadataRetryAt() const;
	[[nodiscard]] bool cooling(quint64 id, qint64 now) const;
	[[nodiscard]] EmojiResolveAdmission resolve(quint64 id, qint64 now);
	void failed(quint64 id, qint64 now);
	void succeeded(quint64 id);
	void transient(qint64 retryAtUnix, qint64 unixNow, qint64 elapsedNow,
		qint64 minimumDelayMs = 0);
	[[nodiscard]] std::optional<qint64> retryAt(qint64 now = 0) const;

private:
	int _budget = 0;
	qint64 _cooldownMs = 0;
	qint64 _resolveRetryAt = 0;
	int _used = 0;
	size_t _cursor = 0;
	std::vector<size_t> _order;
	std::map<quint64, qint64> _retry;

};

}
