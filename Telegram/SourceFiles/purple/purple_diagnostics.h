#pragma once

#include <QtCore/QString>

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
void Clear();

} // namespace Purple::Diagnostics
