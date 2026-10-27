#pragma once

#include "purple/purple_webp_paste.h"
#include "ui/chat/attach/attach_prepare.h"
#include <optional>

class QMimeData;
enum class SendMediaType;
enum class ChatRestriction;

namespace Purple {

[[nodiscard]] std::optional<Ui::PreparedList> PrepareMarkedWebpPaste(
	const QMimeData *data,
	int previewWidth);
[[nodiscard]] SendMediaType ClipboardWebpSendType(
	const Ui::PreparedFile &file,
	SendMediaType requested);
[[nodiscard]] Ui::AlbumType ClipboardWebpAlbumType(
	const Ui::PreparedFile &file,
	Ui::AlbumType requested);
[[nodiscard]] std::optional<bool> ClipboardWebpCompression(
	const Ui::PreparedFile &file,
	std::optional<bool> requested);
[[nodiscard]] bool ClipboardWebpSeparateGroup(
	const Ui::PreparedFile &previous,
	const Ui::PreparedFile &next);
[[nodiscard]] bool ClipboardWebpPreview(
	const Ui::PreparedFile &file,
	bool requested);
[[nodiscard]] bool ClipboardWebpCaption(
	const Ui::PreparedFile &file,
	bool requested);
[[nodiscard]] std::optional<ChatRestriction> ClipboardWebpRestriction(
	const Ui::PreparedFile &file);
[[nodiscard]] bool KeepClipboardWebpOriginal(const Ui::PreparedFile &file);

}
