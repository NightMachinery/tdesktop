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
	void continueSlice();
	[[nodiscard]] bool cooling(quint64 id, qint64 now) const;
	[[nodiscard]] EmojiResolveAdmission resolve(quint64 id, qint64 now);
	void failed(quint64 id, qint64 now);
	void succeeded(quint64 id);
	[[nodiscard]] std::optional<qint64> retryAt() const;

private:
	int _budget = 0;
	qint64 _cooldownMs = 0;
	int _used = 0;
	size_t _cursor = 0;
	std::vector<size_t> _order;
	std::map<quint64, qint64> _retry;

};

}
