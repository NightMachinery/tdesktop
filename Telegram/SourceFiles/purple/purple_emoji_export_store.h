#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <optional>
#include <functional>
#include <vector>
#include <utility>
#include <cstddef>

namespace Purple {

inline constexpr auto kEmojiExportPixels = 128;

struct EmojiExportEntry {
	quint64 id = 0;
	QString alt;
	quint64 setId = 0;
	QString setTitle;
	QString setShortName;
	bool textColor = false;
	std::optional<int> recentRank;
	QString large;
	QString sourceKind;
	std::optional<int> sourcePx;
	std::optional<bool> installed;
};

template <typename Range, typename Project>
void AppendBoundedEmojiExportEntries(
		std::vector<EmojiExportEntry> &entries,
		const Range &range,
		size_t maximum,
		Project project) {
	for (const auto &source : range) {
		if (entries.size() >= maximum) break;
		if (auto entry = project(source)) entries.push_back(std::move(*entry));
	}
}

class EmojiExportEpoch final {
public:
	[[nodiscard]] quint64 begin();
	void revoke();
	[[nodiscard]] bool accepts(quint64 token) const;

private:
	quint64 _generation = 0;
	bool _active = false;

};

[[nodiscard]] QString EmojiExportDirectory(
	const QString &home,
	const QString &workdir,
	const QString &defaultWorkdir);
[[nodiscard]] QString EmojiExportPngName(quint64 id);
[[nodiscard]] std::vector<EmojiExportEntry> UniqueEmojiExportEntries(
	const std::vector<EmojiExportEntry> &entries);
[[nodiscard]] QJsonObject EmojiExportCatalog(
	const std::vector<EmojiExportEntry> &entries,
	const QDateTime &generatedAt,
	const QString &controlSocket = {});
[[nodiscard]] QImage EmojiExportFrame(QImage frame, std::optional<QColor> tint);
[[nodiscard]] bool WriteEmojiExportPng(
	const QString &directory,
	quint64 id,
	const QImage &image,
	const std::function<bool()> &current = {});
[[nodiscard]] bool PublishEmojiExportCatalog(
	const QString &directory,
	const std::vector<EmojiExportEntry> &entries,
	const QDateTime &generatedAt,
	const std::function<bool()> &current = {},
	const QString &controlSocket = {});
[[nodiscard]] bool ClearEmojiExport(
	const QString &directory,
	const std::function<bool()> &current = {});

}
