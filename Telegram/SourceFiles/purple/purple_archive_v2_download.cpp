#include "purple/purple_archive_capture.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_queue.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_v2_acquisition.h"
#include "purple/purple_archive_v2_service.h"
#include "purple/purple_archive_v2_ui.h"
#include "storage/file_download_mtproto.h"
#include "storage/storage_account.h"

#include <QtCore/QBitArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QTemporaryFile>
#include <algorithm>
#include <map>

namespace Purple {
namespace {

using namespace ArchiveV2;

auto &FileWrites = *new crl::queue();
std::atomic<int64> QueuedBytes = 0;
constexpr auto kQueuedBytes = 8 * kMiB;

struct FileDestination {
	std::shared_ptr<QString> path;
	int64 size = 0;
	QBitArray parts;
	bool failed = false;

	bool post(int64 offset, const QByteArray &bytes, const std::shared_ptr<FileDestination> &self) {
		constexpr auto part = Storage::kDownloadPartSize;
		const auto count = bytes.size();
		if (!count) return offset >= size;
		if (offset < 0 || offset >= size || offset % part || count != std::min<int64>(part, size - offset)) return false;
		if (QueuedBytes.fetch_add(count) + count > kQueuedBytes) {
			QueuedBytes.fetch_sub(count);
			return false;
		}
		FileWrites.async([self, offset, bytes, count] {
			const auto release = gsl::finally([&] { QueuedBytes.fetch_sub(count); });
			if (self->failed) return;
			auto file = QFile(*self->path);
			if (!file.open(QIODevice::ReadWrite) || !file.seek(offset) || file.write(bytes) != count) {
				self->failed = true;
				return;
			}
			self->parts.setBit(offset / part);
		});
		return true;
	}
};

struct Download {
	MediaRequest request;
	std::shared_ptr<Lease> lease;
	std::unique_ptr<mtpFileLoader> loader;
	StorageFileLocation location;
	FullMsgId origin;
	rpl::lifetime lifetime;
	std::shared_ptr<QString> temporary;
	bool ready = false;
	std::atomic<bool> cancelled = false;

	void cancel() {
		cancelled = true;
		lifetime.destroy();
		if (loader && !loader->finished()) loader->cancel();
		loader.reset();
	}

	bool current(Main::Session *session) const {
		return !cancelled && session && !Core::Quitting() && SessionLease(session) == lease && lease->live.load()
			&& lease->mediaRevision.load() == request.target.revision;
	}
};
std::map<Main::Session*, std::vector<std::shared_ptr<Download>>> Downloads;

void Pump(Main::Session *session);

void Forget(Main::Session *session, const std::shared_ptr<Download> &job) {
	const auto i = Downloads.find(session);
	job->cancel();
	if (i != end(Downloads)) std::erase(i->second, job);
	Pump(session);
}

BlobInput Input(const Download &job) {
	const auto &target = job.request.target;
	return { target.store, target.revision, target.message, target.sequence, target.ordinal,
		job.request.size, {}, target.source, {}, target.timed };
}

void CompletedMain(Main::Session *session, const std::shared_ptr<Download> &job, std::optional<BlobInput> input,
		QString reason = u"copy_incomplete"_q) {
	if (job->current(session) && job->request.target.sequence > 0) {
		if (input) reason = QueueBlob(session, std::move(*input)).reason;
		if (!reason.isEmpty()) {
			const auto failure = Input(*job);
			const auto kind = job->request.target.kind;
			(void)Query(session, [=](Store &store) {
				store.noteMediaFailure(failure, reason);
			});
			crl::async([=] {
				auto attention = Attention();
				(void)LogIncident(attention, IncidentStage::WholeFile, reason, false, kind, 1, failure.size);
			});
		}
	}
	Forget(session, job);
}

std::optional<BlobInput> Complete(BlobInput input, QByteArray data, const QString &path) {
	if (data.isEmpty()) return path.isEmpty() ? std::nullopt : CompleteFile(std::move(input), path);
	if (data.size() != input.size) return std::nullopt;
	input.digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
	input.read = [data = std::move(data), offset = qsizetype(0)](qsizetype length) mutable {
		const auto part = data.mid(offset, length);
		offset += part.size();
		return part;
	};
	return input;
}

void StartDownload(Main::Session *session, const std::shared_ptr<Download> &job) {
	if (!job->current(session)) return Forget(session, job);
	const auto size = job->request.size;
	const auto destination = job->temporary ? std::make_shared<FileDestination>(FileDestination{
		job->temporary, size, QBitArray((size + Storage::kDownloadPartSize - 1) / Storage::kDownloadPartSize),
	}) : nullptr;
	auto sink = mtpFileLoader::Destination();
	if (destination) sink = [destination](int64 offset, const QByteArray &bytes) {
		return destination->post(offset, bytes, destination);
	};
	job->loader = std::make_unique<mtpFileLoader>(session,
		job->location, job->origin, UnknownFileLocation, QString(), size, size,
		destination ? LoadToFileOnly : LoadToCacheAsWell, LoadFromCloudOrLocal, true, 0, std::move(sink));
	const auto weak = base::make_weak(session);
	const auto pending = std::weak_ptr(job);
	job->loader->updates() | rpl::on_next_error_done([] {},
		[=](FileLoader::Error) {
			crl::on_main([=] {
				if (const auto job = pending.lock()) CompletedMain(weak.get(), job, std::nullopt, u"transfer_failed"_q);
			});
		}, [=] {
			const auto job = pending.lock();
			if (!job) return;
			auto complete = [=, bytes = job->loader->bytes()]() mutable {
				const auto full = !destination || (!destination->failed && destination->parts.count(false) == 0);
				auto input = full ? Complete(Input(*job), std::move(bytes),
					job->temporary ? *job->temporary : QString()) : std::nullopt;
				if (input && job->temporary) {
					input->read = [keep = job->temporary, read = std::move(input->read)](qsizetype n) { return read(n); };
				}
				crl::on_main([=, input = std::move(input)]() mutable {
					CompletedMain(weak.get(), job, std::move(input));
				});
			};
			if (destination) FileWrites.async(std::move(complete));
			else crl::async(std::move(complete));
		}, job->lifetime);
	job->loader->start();
}

void Pump(Main::Session *session) {
	const auto i = Downloads.find(session);
	if (i == end(Downloads) || std::any_of(i->second.begin(), i->second.end(),
		[](const auto &job) { return bool(job->loader); })) return;
	const auto next = std::find_if(i->second.begin(), i->second.end(),
		[session](const auto &job) { return job->ready && job->current(session); });
	if (next != i->second.end()) {
		const auto job = *next;
		StartDownload(session, job);
	}
}

}

bool ArchiveV2::Downloading(Main::Session *session, const MessageIdentity &item, int64 sequence, int64 ordinal) {
	const auto i = Downloads.find(session);
	return i != end(Downloads) && std::ranges::any_of(i->second, [&](const auto &job) {
		if (!job->ready || !job->current(session)) return false;
		const auto &target = job->request.target;
		return target.message == item && target.sequence == sequence && target.ordinal == ordinal;
	});
}

void CancelArchiveDownloads(Main::Session *session) {
	const auto jobs = Downloads.extract(session);
	if (!jobs.empty()) {
		for (const auto &job : jobs.mapped()) job->cancel();
	}
}

void CancelArchiveAttachment(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto i = Downloads.find(session);
	if (i == end(Downloads)) return;
	std::erase_if(i->second, [&](const auto &job) {
		if (!job->request.target.timed || job->origin != item->fullId()
			|| (job->loader && job->loader->finished())) return false;
		job->cancel();
		return true;
	});
	Pump(session);
}

void ObserveArchiveAttachment(not_null<HistoryItem*> item, bool explicitOpen) {
	const auto media = item->media();
	const auto timed = media && media->ttlSeconds() > 0;
	const auto session = &item->history()->session();
	const auto lease = SessionLease(session);
	if (!media || !lease || !IsServerMsgId(item->id) || timed != explicitOpen) return;
	const auto peer = item->history()->peer;
	const auto identity = MessageIdentity{ Peer(peer->id), uint64(item->id.bare) };
	const auto chat = PeerTokenText(identity.peer);
	if (!chat) return;
	auto request = MediaRequest{ .target = { .message = identity, .timed = timed, .revision = lease->mediaRevision.load() },
		.kind = Classify(peer), .chat = *chat };
	auto path = QString();
	auto bytes = QByteArray();
	auto location = StorageFileLocation();
	if (const auto photo = media->photo()) {
		const auto size = Data::PhotoSize::Large;
		const auto view = photo->activeMediaView();
		bytes = view ? view->imageBytes(size) : QByteArray();
		request.target.source = "photo:" + QByteArray::number(photo->id);
		request.target.kind = MediaKind::Photo;
		request.size = photo->imageByteSize(size);
		if (const auto ref = std::get_if<StorageFileLocation>(&photo->location(size).file().data)) location = *ref;
	} else if (const auto doc = media->document()) {
		const auto view = doc->activeMediaView();
		bytes = view ? view->bytes() : QByteArray();
		path = doc->filepath(false);
		request.target.source = "document:" + QByteArray::number(doc->id);
		request.target.kind = doc->isVoiceMessage() ? MediaKind::Voice : doc->isVideoMessage()
			? MediaKind::Round : doc->isVideoFile() ? MediaKind::Video : MediaKind::Other;
		request.size = doc->size;
		location = doc->videoPreloadLocation();
	} else {
		return;
	}
	const auto decision = DecideMedia(CurrentPolicy(session), request.kind, *chat, {
		.kind = request.target.kind, .trigger = explicitOpen ? MediaTrigger::ExplicitOpen : MediaTrigger::Arrival,
		.timed = timed, .current = lease->live.load(), .complete = bytes.size() == request.size || !path.isEmpty(),
		.reference = location.valid(), .size = request.size,
	});
	if (decision.action == MediaAction::Describe) return;
	auto &jobs = Downloads[session];
	std::erase_if(jobs, [session](const auto &job) {
		if (job->current(session)) return false;
		job->cancel();
		return true;
	});
	if (jobs.size() == kMediaQueue || std::any_of(jobs.begin(), jobs.end(), [&](const auto &job) {
		return job->request.target.message == identity && job->request.target.source == request.target.source;
	})) return;
	const auto job = std::make_shared<Download>();
	job->request = std::move(request);
	job->lease = lease;
	job->location = location;
	job->origin = item->fullId();
	jobs.push_back(job);
	const auto weak = base::make_weak(session);
	const auto directory = QDir::cleanPath(session->local().tempDirectory());
	if (!Query(session, [=](Store &store) mutable {
		auto &target = job->request.target;
		const auto sequence = store.mediaSequence(target.message, target.source, target.timed);
		if (sequence) {
			target.sequence = *sequence;
			target.store = store.id();
		}
		crl::async([=, bytes = std::move(bytes)]() mutable {
			if (job->cancelled || !job->lease->live.load()) return;
			auto available = sequence ? Complete(Input(*job), std::move(bytes), path) : std::nullopt;
			auto remove = std::shared_ptr<QString>();
			if (sequence && !available && location.valid() && job->request.size > Storage::kMaxFileInMemory
				&& DesktopArchiveDirectory(directory)) {
				auto temp = QTemporaryFile(directory + u"/download-XXXXXX"_q);
				if (temp.open()) {
					remove = std::shared_ptr<QString>(new QString(temp.fileName()), [](QString *path) {
						crl::async([owned = std::shared_ptr<QString>(path)] { (void)DesktopArchiveRemoveFile(*owned); });
					});
					temp.setAutoRemove(false);
				}
			}
			crl::on_main([=, available = std::move(available)]() mutable {
				const auto live = weak.get();
				job->temporary = remove;
				job->ready = sequence && !available && location.valid() && (job->request.size <= Storage::kMaxFileInMemory || remove);
				if (job->ready && job->current(live)) Pump(live);
				else CompletedMain(live, job, std::move(available), location.valid() ? u"copy_incomplete"_q : u"no_source"_q);
			});
		});
	})) Forget(session, job);
}

}
