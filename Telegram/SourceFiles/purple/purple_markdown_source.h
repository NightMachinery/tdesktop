#pragma once

#include "purple/purple_archive_source.h"
#include "purple/purple_desktop_archive_store.h"

namespace Purple {

inline constexpr auto kDesktopMarkdownSourceLimit = 1024 * 1024;

struct DesktopMarkdownSourceTarget {
	ArchiveCopyRequest request;
	ArchiveStoreEpoch epoch;
};

[[nodiscard]] std::optional<DesktopMarkdownSourceTarget> DesktopMarkdownTarget(
	DesktopArchiveStore &store,
	const ArchiveMessageKey &key,
	const QString &version = {});
[[nodiscard]] bool AttachDesktopMarkdownSource(
	DesktopArchiveStore &store,
	const DesktopMarkdownSourceTarget &target,
	ArchiveSource source,
	std::function<bool()> current);
[[nodiscard]] bool CaptureDesktopMarkdownFile(
	DesktopArchiveStore &store,
	const ArchiveMessageKey &key,
	const QString &version,
	const QString &identity);
[[nodiscard]] bool IsDesktopMarkdownFile(const ArchiveMediaDescriptor &document);
[[nodiscard]] std::optional<QString> DecodeDesktopMarkdownSource(const QByteArray &bytes);

}
