#include "purple/purple_webp_paste_route.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "data/data_chat_participant_status.h"
#include "boxes/send_files_box.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QMimeData>
#include <cstdio>
#include "webp-paste-flow.inc"

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	if (argc != 2) return 2;
	auto file = QFile(QString::fromLocal8Bit(argv[1]));
	if (!file.open(QIODevice::ReadOnly)) return 2;
	const auto bytes = file.readAll();
	auto mime = QMimeData();
	mime.setData(u"application/x-purple-emoji-export"_q,
		R"({"version":1,"format":"webp","intent":"sticker","preserveAlpha":true})");
	mime.setData(u"image/webp"_q, bytes);
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::printf("FAIL %s\n", name); }
	};
	const auto marked = [&] {
		auto result = Purple::PrepareMarkedWebpPaste(&mime, 128);
		if (!result) std::abort();
		return std::move(*result);
	};
	const auto ordinary = [&] {
		auto result = marked();
		auto &item = result.files.front();
		item.preserveOriginalBytes = false;
		item.type = Ui::PreparedFile::Type::Photo;
		item.information->filemime = u"image/png"_q;
		return result;
	};
	for (const auto compress : { false, true }) {
		auto list = marked();
		auto &item = list.files.front();
		check(item.type == Ui::PreparedFile::Type::None && item.isSticker(), "standalone sticker classification");
		check(item.albumType(compress) == Ui::AlbumType::None
			&& !item.canBeInAlbumType(Ui::AlbumType::PhotoVideo)
			&& !item.canBeInAlbumType(Ui::AlbumType::File), "sticker never claims album eligibility");
		check(Purple::ClipboardWebpPreview(item, compress), "sticker-style preview for both checkbox states");
		check(Purple::ClipboardWebpRestriction(item) == ChatRestriction::SendStickers, "actual sticker permissions");
		check(!list.canAddCaption(compress) && !Purple::ClipboardWebpCaption(item, true), "sticker caption policy ignores compression checkbox");
		check(!ProbeContextEditor(item, compress), "draw action disabled for standalone sticker");
		check(!ProbePreviewPhoto(ProbePreviewBackground(
			Core::IsMimeSticker(item.information->filemime)), false, false), "sticker preview emits no photo-editor request");
		auto &image = std::get<Ui::PreparedFileInformation::Image>(item.information->media);
		const auto originalImage = image.data;
		const auto originalPreview = item.preview;
		const auto accepted = ProbeEditorAdmission(item);
		if (accepted) {
			image.modifications.crop = QRect(0, 0, 256, 256);
			item.preview = image.data.copy(image.modifications.crop);
		}
		check(!accepted, "accepted-crop reproducer cannot enter the marked editor");
		check(!Storage::ApplyModifications(list) && item.content == bytes
			&& image.data == originalImage && item.preview == originalPreview,
			"preview and immutable upload remain identical after refused edit");
		image.modifications.crop = QRect(0, 0, 256, 256);
		check(!ProbeEditorAdmission(item), "even a stale modification cannot open standalone sticker editor");
		for (auto mixed = 0; mixed != 3; ++mixed) {
			auto first = mixed == 2 ? ordinary() : marked();
			auto second = mixed == 1 ? ordinary() : marked();
			check(first.canBeSentInSlowmode() && !first.canBeSentInSlowmodeWith(second),
				"incremental slow-mode admission refuses marked second message");
			first.files.push_back(std::move(second.files.front()));
			check(!first.canBeSentInSlowmode(), "two-item slow-mode admission refuses marked or mixed pair");
			auto way = Ui::SendFilesWay();
			way.setHasCompressedStickers(true);
			way.setGroupFiles(true);
			way.setSendImagesAsPhotos(compress);
			auto groups = Ui::DivideByGroups(std::move(first), way, true);
			check(groups.size() == 2 && groups[0].list.files.size() == 1
				&& groups[1].list.files.size() == 1, "each marked independent send has a separate group");
			auto bundle = Ui::PreparedBundle();
			bundle.groups = std::move(groups);
			check(ProbeFinalSlowmode(bundle), "final safeguard refuses the real multiple-message bundle");
			auto confirmation = ProbeConfirmation();
			auto initial = mixed == 2 ? ordinary() : marked();
			confirmation.addFile(std::move(initial.files.front()));
			check(confirmation._list.files.size() == 1, "first item accepted in existing slow-mode confirmation");
			auto appended = mixed == 1 ? ordinary() : marked();
			confirmation.addFile(std::move(appended.files.front()));
			check(confirmation._list.files.size() == 1, "adding a second marked or mixed item is refused");
		}
	}
	auto control = ordinary();
	auto &item = control.files.front();
	check(ProbeEditorAdmission(item) && ProbeContextEditor(item, true), "ordinary photo editor remains enabled");
	auto &image = std::get<Ui::PreparedFileInformation::Image>(item.information->media);
	image.modifications.crop = QRect(0, 0, 256, 256);
	check(Storage::ApplyModifications(control) && image.data.size() == QSize(256, 256)
		&& item.content.isEmpty(), "ordinary accepted crop updates the actual image");
	auto second = ordinary();
	control.files.push_back(std::move(second.files.front()));
	check(control.canBeSentInSlowmode(), "ordinary photos remain slow-mode album eligible");
	auto way = Ui::SendFilesWay();
	way.setGroupFiles(true);
	way.setSendImagesAsPhotos(true);
	const auto groups = Ui::DivideByGroups(std::move(control), way, true);
	check(groups.size() == 1 && groups[0].type == Ui::AlbumType::PhotoVideo,
		"ordinary photos retain one real album");
	auto confirmation = ProbeConfirmation();
	for (auto i = 0; i != 2; ++i) {
		auto next = ordinary();
		confirmation.addFile(std::move(next.files.front()));
	}
	check(confirmation._list.files.size() == 2, "ordinary album additions remain accepted");
	std::printf("webp confirmation flows: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
