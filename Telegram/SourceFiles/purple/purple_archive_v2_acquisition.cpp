#include "purple/purple_archive_v2_acquisition.h"

#include "purple/purple_archive_files.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>

namespace Purple::ArchiveV2 {

MediaAdmission::MediaAdmission(std::shared_ptr<Lease> lease, QByteArray generation)
: _lease(std::move(lease)), _generation(std::move(generation)) {
}

MediaDecision MediaAdmission::begin(const Policy &policy, MediaRequest request,
		MediaTrigger trigger, bool reference, bool complete) {
	const auto &target = request.target;
	if (target.store.size() != 16 || target.store != _generation || target.source.isEmpty() || target.sequence <= 0
		|| target.ordinal < 0 || _pending) {
		return { MediaAction::Describe, MediaReason::ScopeRetired };
	}
	const auto decision = DecideMedia(policy, request.kind, request.chat, {
		.kind = target.kind, .trigger = trigger, .timed = target.timed,
		.current = _lease && _lease->live.load(), .complete = complete,
		.reference = reference, .size = request.size,
	});
	if (decision.action != MediaAction::Describe) _pending = std::move(request);
	return decision;
}

MediaDecision MediaAdmission::opened(const Policy &policy, MediaRequest request, bool reference,
		bool complete, const std::function<void()> &ordinaryOpen) {
	ordinaryOpen();
	return begin(policy, std::move(request), MediaTrigger::ExplicitOpen, reference, complete);
}

std::optional<BlobInput> MediaAdmission::finish(const Policy &policy, const MediaTarget &target) {
	if (!_pending || _pending->target != target) return std::nullopt;
	auto request = std::exchange(_pending, std::nullopt);
	const auto decision = DecideMedia(policy, request->kind, request->chat, {
		.kind = target.kind, .trigger = MediaTrigger::WholeFileAvailable, .timed = target.timed,
		.current = _lease && _lease->live.load(), .admitted = true, .complete = true,
		.size = request->size,
	});
	return decision.action == MediaAction::Adopt
		? std::optional(BlobInput{ .storeId = target.store, .identity = target.message,
			.sequence = target.sequence, .ordinal = target.ordinal, .size = request->size, .source = target.source })
		: std::nullopt;
}

void MediaAdmission::cancel(const MediaTarget &target) {
	if (_pending && _pending->target == target) _pending.reset();
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
