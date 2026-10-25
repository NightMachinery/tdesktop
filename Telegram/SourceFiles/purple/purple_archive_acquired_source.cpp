#include "purple/purple_archive_acquired_source.h"

#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include <algorithm>
#include <deque>
#include <map>

namespace Purple {

class ArchiveStreamBridge final {
public:
	static not_null<Media::Streaming::Loader*> Loader(not_null<Media::Streaming::Reader*> reader) {
		return reader->_loader.get();
	}
};

namespace {

struct StreamBindings {
	base::weak_ptr<Media::Streaming::Reader> reader;
	std::vector<DesktopArchiveObservation> observations;
	std::shared_ptr<bool> seen = std::make_shared<bool>(false);
};
struct DocumentBinding {
	std::weak_ptr<void> document;
	base::weak_ptr<Media::Streaming::Reader> reader;
};
struct Registry {
	std::map<Media::Streaming::Reader*, StreamBindings> streams;
	std::map<Media::Streaming::Document*, DocumentBinding> documents;
	std::deque<DesktopArchiveObservation> observations;
};
std::map<Main::Session*, Registry> Registries;

void Available(not_null<Main::Session*> session, const DesktopArchiveObservation &entry) {
	auto ok = false;
	const auto id = entry.identity.mid(entry.identity.indexOf(':') + 1).toULongLong(&ok);
	if (!ok || !id) {
		return;
	}
	if (entry.kind == u"document"_q) {
		const auto document = session->data().document(id);
		const auto media = document->activeMediaView();
		if (document->size < 0) {
			return;
		}
		const auto bytes = media ? media->bytes() : QByteArray();
		const auto path = document->filepath(false);
		if ((media && media->loaded() && uint64_t(bytes.size()) == uint64_t(document->size))
			|| !path.isEmpty()) {
			const auto proof = !path.isEmpty() && document->location(false).check()
				? DesktopArchiveInspectSource(path, uint64_t(document->size)) : std::nullopt;
			QueueArchiveAvailableMedia(session, entry.key, entry.observation, entry.contentHash,
				entry.identity, bytes, path, uint64_t(document->size), proof);
		}
	} else if (entry.kind == u"photo"_q) {
		const auto media = session->data().photo(id)->activeMediaView();
		const auto bytes = media ? media->imageBytes(Data::PhotoSize::Large) : QByteArray();
		if (!bytes.isEmpty()) {
			QueueArchiveAvailableMedia(session, entry.key, entry.observation, entry.contentHash,
				entry.identity, bytes, {}, uint64_t(bytes.size()));
		}
	}
}

Registry &For(not_null<Main::Session*> session) {
	const auto [i, inserted] = Registries.try_emplace(session.get());
	if (inserted) {
		session->lifetime().add([pointer = session.get()] { Registries.erase(pointer); });
		const auto progress = [session](const QString &identity) {
			const auto i = Registries.find(session.get());
			if (i == Registries.end()) {
				return;
			}
			const auto entries = i->second.observations;
			for (const auto &entry : entries) {
				if (entry.identity == identity) {
					Available(session, entry);
				}
			}
		};
		session->data().documentLoadProgress() | rpl::on_next([progress](not_null<DocumentData*> data) {
			progress(u"document:"_q + QString::number(data->id));
		}, session->lifetime());
		session->data().photoLoadProgress() | rpl::on_next([progress](not_null<PhotoData*> data) {
			progress(u"photo:"_q + QString::number(data->id));
		}, session->lifetime());
	}
	return i->second;
}

void Stream(not_null<Main::Session*> session, const QString &identity,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Reader*> reader) {
	const auto loader = ArchiveStreamBridge::Loader(reader);
	const auto message = std::get_if<Data::FileOriginMessage>(&origin.data);
	if (!message || message->msg.bare <= 0 || loader->size() < 0) {
		return;
	}
	const auto key = ArchiveMessageKey{SessionAccountKey(session), DesktopArchiveChat(message->peer), message->msg.bare};
	const auto observation = ArchiveAcquisitionObservation(session, key, identity);
	if (!observation) {
		QueueArchiveRetentionGap(session, u"stream-unbound"_q);
		return;
	}
	auto &streams = For(session).streams;
	std::erase_if(streams, [](const auto &entry) { return !entry.second.reader; });
	if (!streams.contains(reader.get()) && streams.size() >= 64) {
		QueueArchiveRetentionGap(session, u"stream-binding-full"_q);
		return;
	}
	auto &binding = streams[reader.get()];
	const auto duplicate = std::any_of(binding.observations.begin(), binding.observations.end(), [&](const auto &entry) {
		return entry.key == observation->key && entry.observation == observation->observation
			&& entry.contentHash == observation->contentHash && entry.identity == observation->identity;
	});
	if (duplicate) {
		return;
	}
	if (binding.observations.size() >= 64) {
		QueueArchiveRetentionGap(session, u"stream-binding-full"_q);
		return;
	}
	binding.reader = base::make_weak(reader);
	binding.observations.push_back(*observation);
	if (*binding.seen) {
		QueueArchiveRetentionGap(session, u"stream-binding-after-parts"_q);
	}
	const auto seen = binding.seen;
	const auto weak = binding.reader;
	const auto sessionWeak = base::make_weak(session.get());
	const auto submit = ArchiveStreamSink(session, *observation, uint64_t(loader->size()));
	if (!loader->size()) {
		submit(0, {});
	}
	loader->parts() | rpl::on_next([submit, seen, weak, sessionWeak](const Media::Streaming::LoadedPart &part) {
		if (!weak || !sessionWeak) {
			return;
		}
		*seen = true;
		if (part.offset >= 0 && !part.bytes.isEmpty()) {
			submit(uint64_t(part.offset), part.bytes);
		} else if (part.offset == Media::Streaming::LoadedPart::kFailedOffset) {
			QueueArchiveRetentionGap(sessionWeak.get(), u"stream-input-failed"_q);
		}
	}, session->lifetime());
}

}

void TrackArchiveAcquisition(not_null<Main::Session*> session, const ArchiveEvent &event) {
	if (!event.content || event.key.account != SessionAccountKey(session)) {
		return;
	}
	auto &registry = For(session);
	for (const auto &descriptor : event.content->media) {
		const auto existing = std::find_if(registry.observations.begin(), registry.observations.end(),
			[&](const auto &entry) { return entry.key == event.key && entry.observation == event.id
				&& entry.identity == descriptor.identity; });
		if (existing != registry.observations.end()) {
			continue;
		}
		if (registry.observations.size() == 512) {
			registry.observations.pop_front();
		}
		registry.observations.push_back({event.key, event.id, ArchiveContentHash(*event.content),
			descriptor.identity, descriptor.kind});
		Available(session, registry.observations.back());
	}
}

std::optional<DesktopArchiveObservation> ArchiveAcquisitionObservation(not_null<Main::Session*> session,
	const ArchiveMessageKey &key, const QString &identity) {
	const auto &registry = For(session);
	const auto i = std::find_if(registry.observations.rbegin(), registry.observations.rend(),
		[&](const auto &entry) { return entry.key == key && entry.identity == identity; });
	return i != registry.observations.rend() ? std::optional(*i) : std::nullopt;
}

void ObserveArchiveStream(not_null<Main::Session*> session, not_null<DocumentData*> data,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Reader*> reader) {
	Stream(session, u"document:"_q + QString::number(data->id), origin, reader);
}

void ObserveArchiveStream(not_null<Main::Session*> session, not_null<PhotoData*> data,
	const Data::FileOrigin &origin, not_null<Media::Streaming::Reader*> reader) {
	Stream(session, u"photo:"_q + QString::number(data->id), origin, reader);
}

void BindArchiveStreamDocument(not_null<Main::Session*> session,
		const std::shared_ptr<Media::Streaming::Document> &document,
		const std::shared_ptr<Media::Streaming::Reader> &reader) {
	auto &documents = For(session).documents;
	std::erase_if(documents, [](const auto &entry) { return entry.second.document.expired() || !entry.second.reader; });
	if (documents.size() >= 64 && !documents.contains(document.get())) {
		QueueArchiveRetentionGap(session, u"stream-document-binding-full"_q);
		return;
	}
	documents[document.get()] = {document, base::make_weak(reader.get())};
}

namespace {

void ReusedDocument(not_null<Main::Session*> session, const QString &identity,
		const Data::FileOrigin &origin, not_null<Media::Streaming::Document*> document) {
	auto &documents = For(session).documents;
	const auto i = documents.find(document.get());
	if (i == documents.end() || i->second.document.expired() || !i->second.reader) {
		QueueArchiveRetentionGap(session, u"stream-document-unbound"_q);
		return;
	}
	Stream(session, identity, origin, i->second.reader.get());
}

}

void ObserveArchiveStream(not_null<Main::Session*> session, not_null<DocumentData*> data,
		const Data::FileOrigin &origin, not_null<Media::Streaming::Document*> document) {
	ReusedDocument(session, u"document:"_q + QString::number(data->id), origin, document);
}

void ObserveArchiveStream(not_null<Main::Session*> session, not_null<PhotoData*> data,
		const Data::FileOrigin &origin, not_null<Media::Streaming::Document*> document) {
	ReusedDocument(session, u"photo:"_q + QString::number(data->id), origin, document);
}

}
