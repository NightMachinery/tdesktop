#include "purple/purple_markdown_current.h"

#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_config.h"
#include <QtCore/QJsonArray>
#include <QtCore/QCryptographicHash>
#include <map>
#include <deque>

namespace Purple {
namespace {

struct RichObservation {
	FullMsgId id;
	MTPMessage message;
	std::optional<MTPMessage> complete;
};
std::map<Main::Session*, std::deque<RichObservation>> RichPreviews;
rpl::event_stream<DesktopRichResponse> RichResponses;

size_t RichObservationBytes(const RichObservation &record) {
	return tl::count_length(record.message)
		+ (record.complete ? tl::count_length(*record.complete) : 0);
}

template <typename T> QByteArray Wire(const T &value) {
	auto data = mtpBuffer();
	value.write(data);
	return QByteArray(reinterpret_cast<const char*>(data.constData()), data.size() * sizeof(mtpPrime));
}

}

void RememberDesktopRichPreview(not_null<Main::Session*> session, const MTPMessage &message) {
	if (message.type() != mtpc_message || !message.c_message().vrich_message()
		|| tl::count_length(message) > 1024 * 1024) {
		return;
	}
	const auto &data = message.c_message();
	const auto id = FullMsgId(peerFromMTP(data.vpeer_id()), data.vid().v);
	if (!IsServerMsgId(id.msg)) { return; }
	const auto [entry, inserted] = RichPreviews.try_emplace(session.get());
	if (inserted) {
		session->lifetime().add([owner = session.get()] { RichPreviews.erase(owner); });
	}
	auto &records = entry->second;
	std::erase_if(records, [&](const auto &record) { return record.id == id; });
	auto bytes = size_t(tl::count_length(message));
	for (const auto &record : records) { bytes += RichObservationBytes(record); }
	while (!records.empty() && (records.size() >= 64 || bytes > 8 * 1024 * 1024)) {
		bytes -= RichObservationBytes(records.front());
		records.pop_front();
	}
	records.push_back({id, message, {}});
}

rpl::producer<DesktopRichResponse> DesktopRichResponses() {
	return RichResponses.events();
}

void ObserveDesktopRichResponse(not_null<Main::Session*> session,
		FullMsgId id, const MTPmessages_Messages &response) {
	const auto registry = RichPreviews.find(session.get());
	if (registry != RichPreviews.end()) {
		auto &records = registry->second;
		for (auto &record : records) {
			if (record.id == id) {
				record.complete = ValidateDesktopRichCopyResponse(record.message, response);
				break;
			}
		}
		auto bytes = size_t(0);
		for (const auto &record : records) { bytes += RichObservationBytes(record); }
		while (!records.empty() && bytes > 8 * 1024 * 1024) {
			bytes -= RichObservationBytes(records.front());
			records.pop_front();
		}
	}
	RichResponses.fire_copy({session.get(), id, response});
}

bool DesktopRichWireExtends(const MTPRichMessage &preview, const MTPRichMessage &full) {
	const auto &before = preview.data();
	const auto &after = full.data();
	if (after.is_part() || before.is_rtl() != after.is_rtl()
		|| before.vblocks().v.size() > after.vblocks().v.size()) {
		return false;
	}
	for (auto i = 0; i != before.vblocks().v.size(); ++i) {
		if (Wire(before.vblocks().v[i]) != Wire(after.vblocks().v[i])) { return false; }
	}
	const auto contains = [](const auto &observed, const auto &received) {
		for (const auto &value : observed) {
			if (std::none_of(received.begin(), received.end(), [&](const auto &candidate) {
				return Wire(value) == Wire(candidate);
			})) { return false; }
		}
		return true;
	};
	return contains(before.vphotos().v, after.vphotos().v)
		&& contains(before.vdocuments().v, after.vdocuments().v);
}


DesktopMarkdownBody DesktopMarkdownBodyForItem(not_null<HistoryItem*> item) {
	auto body = DesktopMarkdownBody();
	body.key = {SessionAccountKey(&item->history()->session()),
		DesktopArchiveChat(item->history()->peer->id), item->id.bare};
	body.content.text = item->originalText().text;
	body.serverTime = item->date();
	const auto edited = item->Get<HistoryMessageEdited>();
	body.editDate = edited ? edited->date : 0;
	const auto wire = Api::EntitiesToMTP(&item->history()->session(),
		item->originalText().entities, Api::ConvertOption::SkipLocal);
	for (const auto &entity : wire.v) {
		auto buffer = mtpBuffer();
		entity.write(buffer);
		body.content.entities.push_back(QJsonObject{{u"tl"_q,
			QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(buffer.constData()),
				buffer.size() * sizeof(mtpPrime)).toBase64())}});
	}
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			body.document = u"document:"_q + QString::number(document->id);
			body.name = document->filename();
			body.mime = document->mimeString();
			body.size = document->size >= 0 ? uint64_t(document->size) : uint64_t(-1);
		} else {
			body.content.opaqueFormat = u"non-document-medium"_q;
			body.content.opaque = "unavailable";
		}
	}
	return body;
}

bool DesktopRichPageExtends(const Iv::RichPage &preview, const Iv::RichPage &full) {
	return !full.part && preview.rtl == full.rtl && preview.url == full.url
		&& preview.blocks.size() <= full.blocks.size()
		&& std::equal(preview.blocks.begin(), preview.blocks.end(), full.blocks.begin());
}

std::optional<MTPMessage> DesktopRichCopyOpening(not_null<Main::Session*> session,
		FullMsgId id) {
	const auto item = session->data().message(id);
	if (!item) { return {}; }
	const auto edited = item->Get<HistoryMessageEdited>();
	const auto date = edited ? edited->date : 0;
	const auto registry = RichPreviews.find(session.get());
	if (registry == RichPreviews.end()) { return {}; }
	for (const auto &entry : registry->second) {
		if (entry.id != id) { continue; }
		const auto &previous = entry.message.c_message();
		const auto observedPage = Iv::ParseRichPage(session, *previous.vrich_message());
		if (previous.vdate().v != item->date() || previous.vedit_date().value_or(0) != date
			|| qs(previous.vmessage()) != item->originalText().text || !observedPage
			|| !item->richPage() || *observedPage != *item->richPage()) { return {}; }
		return entry.message;
	}
	return {};
}

std::optional<MTPMessage> DesktopRichCopyCachedMessage(not_null<Main::Session*> session,
		FullMsgId id) {
	const auto registry = RichPreviews.find(session.get());
	if (registry != RichPreviews.end()) {
		for (const auto &record : registry->second) {
			if (record.id == id) { return record.complete; }
		}
	}
	return {};
}

bool AdmitDesktopRichCopyPage(const Iv::RichPage &preview, const Iv::RichPage &page,
		const std::optional<MTPMessage> &opening, const std::optional<MTPMessage> &complete,
		const std::shared_ptr<const Iv::RichPage> &decoded) {
	if (!opening || !complete || !decoded || *decoded != page
		|| !DesktopRichPageExtends(preview, page)) { return false; }
	const auto response = MTP_messages_messages(MTP_vector<MTPMessage>({*complete}),
		MTP_vector<MTPForumTopic>(), MTP_vector<MTPChat>(), MTP_vector<MTPUser>());
	return ValidateDesktopRichCopyResponse(*opening, response).has_value();
}

std::optional<MTPMessage> ValidateDesktopRichCopyResponse(const MTPMessage &opening,
		const MTPmessages_Messages &response) {
	if (opening.type() != mtpc_message || !opening.c_message().vrich_message()) { return {}; }
	const auto &previous = opening.c_message();
	auto matches = 0;
	auto retained = std::optional<MTPMessage>();
	response.match([](const MTPDmessages_messagesNotModified &) {}, [&](const auto &data) {
		for (const auto &message : data.vmessages().v) {
			if (message.type() != mtpc_message) { continue; }
			const auto &value = message.c_message();
			if (value.vid().v != previous.vid().v) { continue; }
			++matches;
			if (peerFromMTP(value.vpeer_id()) != peerFromMTP(previous.vpeer_id())
				|| value.vdate().v != previous.vdate().v
				|| value.vgrouped_id().value_or(0) != previous.vgrouped_id().value_or(0)
				|| value.vedit_date().value_or(0) != previous.vedit_date().value_or(0)
				|| qs(value.vmessage()) != qs(previous.vmessage()) || !value.vrich_message()
				|| tl::count_length(message) > 1024 * 1024
				|| !DesktopRichWireExtends(*previous.vrich_message(), *value.vrich_message())) { continue; }
			retained = message;
		}
	});
	return matches == 1 ? retained : std::nullopt;
}

}
