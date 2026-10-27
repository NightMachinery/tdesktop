#include "purple/purple_webp_paste_route.h"

#include "data/data_chat_participant_status.h"
#include "purple/purple_webp_paste.h"
#include "storage/localimageloader.h"

namespace Purple {

std::optional<Ui::PreparedList> PrepareMarkedWebpPaste(
		const QMimeData *data,
		int previewWidth) {
	auto read = ReadMarkedWebpPaste(data);
	if (!read || previewWidth <= 0) return std::nullopt;
	auto list = Ui::PreparedList();
	auto file = Ui::PreparedFile(QString());
	file.content = std::move(read->content);
	file.size = file.content.size();
	file.displayName = u"emoji.webp"_q;
	file.information = std::make_unique<Ui::PreparedFileInformation>();
	file.information->filemime = u"image/webp"_q;
	file.originalDimensions = read->image.size();
	file.shownDimensions = file.originalDimensions.scaled(
		previewWidth, previewWidth, Qt::KeepAspectRatio);
	file.preview = read->image.scaled(file.shownDimensions,
		Qt::KeepAspectRatio, Qt::SmoothTransformation);
	file.information->media = Ui::PreparedFileInformation::Image{
		.data = std::move(read->image),
		.bytes = file.content,
		.format = "webp",
	};
	file.type = Ui::PreparedFile::Type::None;
	file.preserveOriginalBytes = true;
	list.overrideSendImagesAsPhotos = false;
	list.files.push_back(std::move(file));
	return list;
}

SendMediaType ClipboardWebpSendType(
		const Ui::PreparedFile &file,
		SendMediaType requested) {
	return file.preserveOriginalBytes ? SendMediaType::File : requested;
}

Ui::AlbumType ClipboardWebpAlbumType(
		const Ui::PreparedFile &file,
		Ui::AlbumType requested) {
	return file.preserveOriginalBytes ? Ui::AlbumType::None : requested;
}

std::optional<bool> ClipboardWebpCompression(
		const Ui::PreparedFile &file,
		std::optional<bool> requested) {
	return file.preserveOriginalBytes ? std::optional(false) : requested;
}

bool ClipboardWebpSeparateGroup(
		const Ui::PreparedFile &previous,
		const Ui::PreparedFile &next) {
	return previous.preserveOriginalBytes || next.preserveOriginalBytes;
}

bool ClipboardWebpPreview(const Ui::PreparedFile &file, bool requested) {
	return file.preserveOriginalBytes || requested;
}

bool ClipboardWebpCaption(const Ui::PreparedFile &file, bool requested) {
	return !file.preserveOriginalBytes && requested;
}

std::optional<ChatRestriction> ClipboardWebpRestriction(
		const Ui::PreparedFile &file) {
	return file.preserveOriginalBytes
		? std::optional(ChatRestriction::SendStickers) : std::nullopt;
}

bool KeepClipboardWebpOriginal(const Ui::PreparedFile &file) {
	return file.preserveOriginalBytes;
}

}
