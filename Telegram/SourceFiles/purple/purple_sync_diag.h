#pragma once

#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <list>
#include <map>
#include <mutex>
#include <vector>

namespace Purple {

// WHY: words come from a fixed vocabulary and numbers are clamped, so no id,
// name or free text can reach a line; see docs/purple/sync_chat.md.

struct SyncDiagField {
	QString key;
	QString value;
};

[[nodiscard]] QString SyncDiagToken(const QString &value);
[[nodiscard]] bool SyncDiagWordShape(const QString &value);
[[nodiscard]] const std::vector<QString> &SyncDiagVocabulary();

[[nodiscard]] SyncDiagField SyncDiagCode(const QString &key, const QString &value);
[[nodiscard]] SyncDiagField SyncDiagReason(const QString &code);
[[nodiscard]] SyncDiagField SyncDiagCount(const QString &key, int64_t value);
[[nodiscard]] SyncDiagField SyncDiagSeconds(int64_t value);
[[nodiscard]] SyncDiagField SyncDiagFlag(const QString &key, bool value);

inline constexpr auto kSyncDiagLineLimit = 512;
[[nodiscard]] QString SyncDiagLine(
	const QString &area,
	const QString &event,
	std::initializer_list<SyncDiagField> fields);

class SyncDiagRepeats final {
public:
	static constexpr int64_t kWindow = 10 * 60 * 1000;
	static constexpr int kBurst = 3;
	static constexpr int kKeys = 64;

	struct Summary {
		QString area;
		QString event;
		int dropped = 0;
	};
	struct Decision {
		bool write = false;
		std::vector<Summary> summaries;
	};

	[[nodiscard]] Decision offer(
		int64_t now,
		const QString &body,
		const QString &area,
		const QString &event);
	[[nodiscard]] std::vector<Summary> flush();

private:
	struct Window {
		QString body;
		QString area;
		QString event;
		int64_t start = 0;
		int written = 0;
		int dropped = 0;
	};

	std::list<Window> _windows; // Most recently offered first.
	std::map<QString, std::list<Window>::iterator> _index;

};

[[nodiscard]] QString SyncDiagSummaryLine(const SyncDiagRepeats::Summary &summary);

// Lines are dropped until the app installs its log.txt sink.
class SyncDiagLog final {
public:
	using Sink = std::function<void(const QString &line)>;
	using Clock = std::function<int64_t()>;

	SyncDiagLog(Sink sink, Clock clock);

	void write(
		const QString &area,
		const QString &event,
		std::initializer_list<SyncDiagField> fields);
	void flush();

private:
	void emit(const std::vector<SyncDiagRepeats::Summary> &summaries);

	std::mutex _mutex;
	Sink _sink;
	Clock _clock;
	SyncDiagRepeats _repeats;

};

void SetSyncDiagSink(SyncDiagLog::Sink sink);
void SyncDiag(
	const QString &area,
	const QString &event,
	std::initializer_list<SyncDiagField> fields = {});

} // namespace Purple
