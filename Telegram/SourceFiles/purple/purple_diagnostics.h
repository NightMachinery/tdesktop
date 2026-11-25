#pragma once

#include <QtCore/QString>

#include <mutex>
#include <span>

namespace Purple::ModalNotices {
struct NoticeRecord;
} // namespace Purple::ModalNotices

namespace Purple::Diagnostics {

inline constexpr auto kLogFile = "purple-diagnostics.log";

class Journal final {
public:
	explicit Journal(qint64 segmentBytes = 1024 * 1024);

	void start(const QString &directory);
	bool append(QStringView line);
	[[nodiscard]] QString copy(QStringView scope, QStringView notice);
	[[nodiscard]] QString exportText(QStringView notices);
	bool clear();

private:
	[[nodiscard]] QString read() const;

	std::mutex _mutex;
	QString _path;
	const qint64 _segmentBytes;
};

void Start(const QString &directory);
void CaptureMainLog(QStringView line);
[[nodiscard]] QString ForNotice(const ModalNotices::NoticeRecord &notice);
[[nodiscard]] QString ExportText(QStringView notices);
[[nodiscard]] QString ExportNotices(
	std::span<const ModalNotices::NoticeRecord> account,
	std::span<const ModalNotices::NoticeRecord> app);
bool Clear();

} // namespace Purple::Diagnostics
