#pragma once

#include "purple/purple_archive_model.h"
#include "data/data_file_origin.h"

class DocumentData;
class PhotoData;
namespace Main { class Session; }
namespace Media::Streaming { class Reader; class Document; }

namespace Purple {

struct DesktopArchiveObservation {
	ArchiveMessageKey key;
	QString observation;
	QString contentHash;
	QString identity;
	QString kind;
};

void InvalidateArchiveAcquisitions(not_null<Main::Session*> session);
void TrackArchiveAcquisition(not_null<Main::Session*> session, const ArchiveEvent &event);
[[nodiscard]] std::optional<DesktopArchiveObservation> ArchiveAcquisitionObservation(
	not_null<Main::Session*> session, const ArchiveMessageKey &key, const QString &identity);
void ObserveArchiveStream(not_null<Main::Session*> session, not_null<DocumentData*> data,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Reader*> reader);
void ObserveArchiveStream(not_null<Main::Session*> session, not_null<PhotoData*> data,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Reader*> reader);

void BindArchiveStreamDocument(not_null<Main::Session*> session,
	const std::shared_ptr<Media::Streaming::Document> &document,
	const std::shared_ptr<Media::Streaming::Reader> &reader);
void ObserveArchiveStream(not_null<Main::Session*> session, not_null<DocumentData*> data,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Document*> document);
void ObserveArchiveStream(not_null<Main::Session*> session, not_null<PhotoData*> data,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Document*> document);

}
