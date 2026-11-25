#pragma once

#include <QtCore/QString>

#include <span>

namespace Purple::ModalNotices {
struct NoticeRecord;
} // namespace Purple::ModalNotices

namespace Purple::Diagnostics {

enum class Subsystem {
	General,
	Settings,
	Sync,
	Archive,
	Recording,
	Music,
	Privacy,
	Translation,
};

void CaptureMainLog(QStringView line);
void RecordNotice(QStringView scope, QStringView key, bool warning);
[[nodiscard]] QString ForNotice(QStringView scope, QStringView key, bool error);
[[nodiscard]] QString ExportText();
[[nodiscard]] QString ExportNotices(
	std::span<const ModalNotices::NoticeRecord> account,
	std::span<const ModalNotices::NoticeRecord> app);
void Clear();

} // namespace Purple::Diagnostics
