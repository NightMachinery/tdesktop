#include "purple/purple_sync_chat_mark_transport.h"
#include "purple/purple_sync_request_sender.h"

namespace Purple {

bool SendDesktopSyncChatMarkDelete(SyncRequestSender &sender,
		DesktopSyncChatMarkPermit permit, DesktopSyncChatMarkRenewal renew, Fn<void(Fn<void()>)> onSender,
		Fn<void(std::optional<MTPmessages_AffectedMessages>)> done) {
	const auto request = permit.request();
	if (!request || !renew || !onSender || !done) { return false; }
	auto wire = mtpBuffer();
	std::visit([&](const auto &value) { value.write(wire); }, *request);
	const auto active = std::make_shared<DesktopSyncChatMarkPermit>(std::move(permit));
	std::visit([&](auto value) {
		sender.request(std::move(value)).done([done](const MTPmessages_AffectedMessages &response) {
			done(response);
		}).fail([done](const MTP::Error &) {
			done({});
		}).resumeEffect([active, renew, wire, done, onSender](Fn<void(bool)> ready) {
			if (!active->current()) { done({}); ready(false); return; }
			renew([active, wire, done, onSender, ready = std::move(ready)](std::optional<DesktopSyncChatMarkPermit> permit) mutable {
				onSender([active, wire, done, permit = std::move(permit), ready = std::move(ready)]() mutable {
				const auto next = permit ? permit->request() : std::nullopt;
				auto nextWire = mtpBuffer();
				if (next) { std::visit([&](const auto &value) { value.write(nextWire); }, *next); }
				if (!next || nextWire != wire || !permit->current()) { done({}); ready(false); return; }
				*active = *permit;
				ready(true);
				});
			});
		}).send();
	}, *request);
	return true;
}

}
