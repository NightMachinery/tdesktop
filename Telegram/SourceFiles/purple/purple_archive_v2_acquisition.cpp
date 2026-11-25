#include "purple/purple_archive_v2_acquisition.h"

#include "purple/purple_archive_files.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <algorithm>

namespace Purple::ArchiveV2 {

MediaAdmission::MediaAdmission(std::shared_ptr<Lease> lease, QByteArray generation)
: _lease(std::move(lease)), _generation(std::move(generation)) {
}

MediaDecision MediaAdmission::begin(const Policy &policy, MediaRequest request,
		MediaTrigger trigger, bool reference, bool complete) {
	const auto &target = request.target;
	const auto revision = _lease ? _lease->mediaRevision.load() : 0;
	std::erase_if(_pending, [&](const MediaRequest &old) { return old.target.revision != revision; });
	if (target.store.size() != 16 || target.store != _generation || target.source.isEmpty() || target.sequence <= 0
		|| target.ordinal < 0) {
		return { MediaAction::Describe, MediaReason::ScopeRetired };
	}
	const auto decision = DecideMedia(policy, request.kind, request.chat, {
		.kind = target.kind, .trigger = trigger, .timed = target.timed,
		.current = _lease && _lease->live.load() && _lease->mediaRevision.load() == target.revision, .complete = complete,
		.reference = reference, .size = request.size,
	});
	if (decision.action == MediaAction::Describe) return decision;
	if (std::any_of(_pending.begin(), _pending.end(), [&](const auto &old) { return old.target == target; })) return {};
	if (_pending.size() >= kMediaQueue) return { MediaAction::Describe, MediaReason::QueueFull };
	_pending.push_back(std::move(request));
	return decision;
}

MediaDecision MediaAdmission::opened(const Policy &policy, MediaRequest request, bool reference,
		bool complete, const std::function<void()> &ordinaryOpen) {
	ordinaryOpen();
	return begin(policy, std::move(request), MediaTrigger::ExplicitOpen, reference, complete);
}

std::optional<BlobInput> MediaAdmission::finish(const Policy &policy, const MediaTarget &target) {
	const auto found = std::find_if(_pending.begin(), _pending.end(), [&](const auto &old) { return old.target == target; });
	if (found == _pending.end()) return std::nullopt;
	const auto request = std::move(*found);
	_pending.erase(found);
	const auto decision = DecideMedia(policy, request.kind, request.chat, {
		.kind = target.kind, .trigger = MediaTrigger::WholeFileAvailable, .timed = target.timed,
		.current = _lease && _lease->live.load() && _lease->mediaRevision.load() == target.revision, .admitted = true, .complete = true,
		.size = request.size,
	});
	return decision.action == MediaAction::Adopt
		? std::optional(BlobInput{ .storeId = target.store, .revision = target.revision, .identity = target.message,
			.sequence = target.sequence, .ordinal = target.ordinal, .size = request.size, .source = target.source, .timed = target.timed })
		: std::nullopt;
}

void MediaAdmission::cancel(const MediaTarget &target) {
	std::erase_if(_pending, [&](const auto &old) { return old.target == target; });
}

std::optional<BlobInput> CompleteFile(BlobInput input, const QString &path) {
	if (input.size <= 0) return std::nullopt;
	const auto identity = DesktopArchiveInspectSource(path, uint64_t(input.size));
	auto file = std::make_shared<QFile>();
	if (!identity || !DesktopArchiveOpenSource(path, *identity, *file)) return std::nullopt;
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	if (!hash.addData(file.get()) || file->pos() != input.size
		|| !DesktopArchiveSourceMatches(*file, *identity) || !file->seek(0)) return std::nullopt;
	input.digest = hash.result();
	input.read = [file, identity = *identity](qsizetype length) {
		return DesktopArchiveSourceMatches(*file, identity) ? file->read(length) : QByteArray(1, char(0));
	};
	return input;
}

}
