#include "purple/purple_markdown_source.h"

#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "history/history_item.h"
#include "purple/purple_archive_files.h"
#include <QtCore/QFile>
#include <QtCore/QStringDecoder>

namespace Purple {

bool IsDesktopMarkdownFile(const DesktopMarkdownBody &document) {
	const auto name = document.name;
	const auto mime = document.mime
		.section(';', 0, 0).trimmed().toLower();
	return !document.document.isEmpty()
		&& (name.endsWith(u".md"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".markdown"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mkd"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mdown"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mkdn"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mdwn"_q, Qt::CaseInsensitive)
			|| mime == u"text/markdown"_q
			|| mime == u"text/x-markdown"_q
			|| mime == u"text/x-web-markdown"_q);
}

void ReadDesktopMarkdownFile(not_null<HistoryItem*> item, Fn<void(std::optional<QString>)> done) {
	const auto doc = item->media() ? item->media()->document() : nullptr;
	if (!doc || doc->size < 0 || doc->size > kDesktopMarkdownSourceLimit) return done(std::nullopt);
	const auto view = doc->activeMediaView();
	crl::async([bytes = view ? view->bytes() : QByteArray(), path = doc->filepath(false), size = doc->size,
			done = std::move(done)]() mutable {
		if (bytes.size() != size && !path.isEmpty()) {
			const auto proof = DesktopArchiveInspectSource(path, size);
			auto file = QFile(path);
			if (proof && DesktopArchiveOpenSource(path, *proof, file)) {
				auto loaded = file.read(kDesktopMarkdownSourceLimit + 1);
				if (loaded.size() == size && DesktopArchiveSourceMatches(file, *proof)) bytes = std::move(loaded);
			}
		}
		auto decoder = QStringDecoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
		auto text = QString(decoder(bytes));
		crl::on_main([done, text = bytes.size() == size && !decoder.hasError()
			? std::optional(std::move(text)) : std::nullopt]() mutable { done(std::move(text)); });
	});
}

}
