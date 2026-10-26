#include "purple/purple_compose_receipt.h"

#include "purple/purple_archive_capture.h"
#include "data/data_peer_id.h"

namespace Purple {

std::optional<ArchiveEvent> DesktopComposeAcknowledgement(
		const ArchiveMessageKey &opening,
		uint64 random,
		const ArchiveContent &expected,
		const MTPUpdates &updates) {
	if (!IsArchiveMessageKey(opening) || !random) {
		return {};
	}
	const auto vector = updates.match([](const MTPDupdates &data) {
		return data.vupdates().v;
	}, [](const MTPDupdatesCombined &data) {
		return data.vupdates().v;
	}, [](const auto &) { return QVector<MTPUpdate>(); });
	auto server = MsgId();
	auto acknowledgements = 0;
	for (const auto &update : vector) {
		if (update.type() == mtpc_updateMessageID
			&& uint64(update.c_updateMessageID().vrandom_id().v) == random) {
			server = update.c_updateMessageID().vid().v;
			++acknowledgements;
		}
	}
	if (acknowledgements != 1 || !IsServerMsgId(server)) {
		return {};
	}
	auto observed = std::optional<ArchiveEvent>();
	for (const auto &update : vector) {
		const auto message = update.match([](const MTPDupdateNewMessage &data) {
			return std::optional(data.vmessage());
		}, [](const MTPDupdateNewChannelMessage &data) {
			return std::optional(data.vmessage());
		}, [](const auto &) { return std::optional<MTPMessage>(); });
		if (!message || message->type() != mtpc_message) {
			continue;
		}
		const auto &data = message->c_message();
		if (data.vid().v != server.bare || DesktopArchiveChat(peerFromMTP(data.vpeer_id())) != opening.chat) {
			continue;
		}
		if (observed || data.vedit_date() || data.vgrouped_id() || data.vrich_message()
			|| (data.vmedia() && data.vmedia()->type() != mtpc_messageMediaEmpty)) {
			return {};
		}
		observed = DesktopArchiveSnapshotData(opening.account, data,
			ArchiveOrigin::Baseline, u"compose-receipt"_q, data.vdate().v);
	}
	if (!observed || !observed->content || *observed->content != expected
		|| observed->completeness != ArchiveCompleteness::Complete) {
		return {};
	}
	return observed;
}

}
