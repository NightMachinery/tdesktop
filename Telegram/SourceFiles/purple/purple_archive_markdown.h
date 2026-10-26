#pragma once

#include "purple/purple_markdown_source.h"
#include "iv/iv_rich_page.h"
#include "purple/purple_archive_sql.h"

namespace Purple {

struct DesktopArchiveMarkdownRequest {
	ArchiveStoreEpoch epoch;
	ArchiveMessageKey key;
	QString version;
	QString digest;
	ArchiveGeneration generation;
};

[[nodiscard]] QString DesktopArchiveMarkdownDigest(const ArchiveVersionSnapshot &snapshot);
[[nodiscard]] bool DropDesktopArticleMarkdown(ArchiveEvent &event);
[[nodiscard]] ArchiveSqlCapturePlan PlanDesktopArchiveMarkdownCapture(
	const ArchiveSqlSlice &slice, ArchiveEvent event, const ArchiveSqlSecurity &security,
	const ArchiveStoreEpoch *currentEpoch = nullptr);
void AttachDesktopArticleMarkdown(ArchiveEvent &event, const Iv::RichPage &page);
[[nodiscard]] std::optional<QString> RenderDesktopArchiveMarkdown(const ArchiveVersionSnapshot &snapshot);
[[nodiscard]] std::optional<QString> ReadDesktopArchiveMarkdown(
	DesktopArchiveStore &store, const DesktopArchiveMarkdownRequest &request);

}
