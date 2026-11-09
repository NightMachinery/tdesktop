#include "purple/purple_media_reply_adapter.h"

#include "apiwrap.h"
#include "api/api_updates.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_media_reply.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>

#include <map>

namespace Purple {
namespace {

struct State {
	MediaReplyJournal journal;
	MediaReplySettings policy;
	std::unique_ptr<QLockFile> lock;
	QString path;
	int pending = 0;
	int inFlight = 0;
	bool loaded = false;
	bool failed = false;
};
std::map<Main::Session*, std::shared_ptr<State>> States;

QString AccountText(not_null<Main::Session*> session) {
	const auto key = SessionAccountKey(session);
	return AccountKeyText(key.environment, key.userId).value_or(QString());
}

PeerToken Token(not_null<PeerData*> peer) {
	if (const auto id = peerToUser(peer->id)) return {PeerTokenKind::User, id.bare};
	if (const auto id = peerToChat(peer->id)) return {PeerTokenKind::Chat, id.bare};
	return {PeerTokenKind::Channel, peerToChannel(peer->id).bare};
}

void Activate(not_null<Main::Session*> session, const std::shared_ptr<State> &state) {
	state->policy = AccountSettings(session).mediaReply;
	state->journal.account = AccountText(session);
	state->journal.cutoff = base::unixtime::now();
	++state->journal.revision;
}

std::shared_ptr<State> Ensure(not_null<Main::Session*> session) {
	if (const auto i = States.find(session.get()); i != States.end()) return i->second;
	const auto state = std::make_shared<State>();
	States.emplace(session.get(), state);
	Activate(session, state);
	session->lifetime().add([=] { States.erase(session.get()); });
	SettingsChanges() | rpl::on_next([=] {
		if (state->policy != AccountSettings(session).mediaReply) Activate(session, state);
	}, session->lifetime());
	return state;
}

bool Persist(not_null<Main::Session*> session, const std::shared_ptr<State> &state,
		const MediaReplyAction &action) {
	const auto target = AccountSettingsTarget(session);
	if (!target || !ValidateAccountSettingsTarget(session, *target)) return false;
	const auto path = QFileInfo(target->path).absolutePath() + u"/media-reply-journal.json"_q;
	if (!state->loaded) {
		const auto parent = QFileInfo(path).absolutePath();
		if (QFileInfo(parent).canonicalFilePath() != QDir(parent).absolutePath()
			|| QFileInfo(path).isSymLink()) return false;
		state->lock = std::make_unique<QLockFile>(path + u".lock"_q);
		state->lock->setStaleLockTime(0);
		if (!state->lock->tryLock(0)) return false;
		state->path = path;
		if (QFileInfo::exists(path)) {
			auto file = QFile(path);
			if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) return false;
			const auto decoded = DecodeMediaReplyJournal(file.readAll(), state->journal.account);
			if (!decoded) return false;
			state->journal.reserved = decoded->reserved;
		}
		state->loaded = true;
	}
	if (state->path != path || !state->lock || !state->lock->isLocked()
		|| QFileInfo(path).isSymLink() || !ReserveMediaReply(state->journal, action)) return false;
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) { state->failed = true; return false; }
	if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) { state->failed = true; return false; }
	const auto bytes = EncodeMediaReplyJournal(state->journal);
	if (bytes.size() > 32 * 1024 * 1024) { state->failed = true; return false; }
	if (file.write(bytes) != bytes.size() || !file.commit()) { state->failed = true; return false; }
	return ValidateAccountSettingsTarget(session, *target);
}

void Observe(not_null<HistoryItem*> item, MediaReplyOrigin origin) {
	const auto session = &item->history()->session();
	const auto state = Ensure(session);
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || (!document->isVoiceMessage() && !document->isVideoMessage())
		|| !IsServerMsgId(item->id) || item->isScheduled() || item->isBusinessShortcut()
		|| state->failed || state->pending >= 32) return;
	const auto peer = item->history()->peer;
	const auto message = MediaReplyMessage{
		AccountText(session), Token(peer), KindOf(peer), int(item->id.bare),
		int(item->topicRootId().bare), item->date(),
		document->isVoiceMessage() ? MediaReplyMedia::Voice : MediaReplyMedia::Round,
		origin, item->out(), true, false };
	if (state->policy != AccountSettings(session).mediaReply) Activate(session, state);
	const auto action = PlanMediaReply(AccountSettings(session), message,
		state->journal.account, state->journal.cutoff, state->journal.revision);
	if (!action) return;
	++state->pending;
	const auto weak = base::make_weak(session);
	const auto peerId = peer->id;
	crl::on_main(session, [=] {
		--state->pending;
		const auto live = weak.get();
		if (!live || live->account().loggingOut() || live->account().maybeSession() != live
			|| state->inFlight >= 8 || !AccountAutomaticOperationsAllowed(live)
			|| !MediaReplyStillCurrent(AccountSettings(live), *action, AccountText(live),
				state->journal.cutoff, state->journal.revision)) return;
		const auto original = live->data().message(peerId, MsgId(action->message.message));
		if (!original || original->isScheduled()) return;
		const auto peer = original->history()->peer;
		const auto topic = original->topic();
		if (!(topic ? Data::CanSendTexts(topic) : Data::CanSendTexts(peer))) return;
		if (!Persist(live, state, *action) || !MediaReplyStillCurrent(AccountSettings(live),
			*action, AccountText(live), state->journal.cutoff, state->journal.revision)) return;
		const auto replyFlags = action->message.topic
			? MTPDinputReplyToMessage::Flag::f_top_msg_id : MTPDinputReplyToMessage::Flags();
		const auto reply = MTP_inputReplyToMessage(MTP_flags(replyFlags), MTP_int(action->message.message),
			MTP_int(action->message.topic), MTPInputPeer(), MTPstring(), MTPVector<MTPMessageEntity>(),
			MTPint(), MTPInputPeer(), MTPint(), MTPbytes(), MTPVector<MTPRichText>());
		++state->inFlight;
		live->api().request(MTPmessages_SendMessage(
			MTP_flags(MTPmessages_SendMessage::Flag::f_reply_to | MTPmessages_SendMessage::Flag::f_no_webpage),
			peer->input(), reply, MTP_string(action->text), MTP_long(action->randomId),
			MTPReplyMarkup(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(),
			MTPInputPeer(), MTPInputQuickReplyShortcut(), MTPlong(), MTPlong(),
			MTPSuggestedPost(), MTPInputRichMessage()
		)).done([=](const MTPUpdates &result) {
			--state->inFlight;
			if (const auto current = weak.get(); current && !current->account().loggingOut()
				&& current->account().maybeSession() == current && AccountText(current) == action->message.account) {
				current->api().applyUpdates(result);
			}
		}).fail([=](const MTP::Error &) { --state->inFlight; }).handleFloodErrors().send();
	});
}

} // namespace

void ObserveMediaReplyMessage(
		not_null<Main::Session*> session, const MTPMessage &message, bool replay) {
	if (replay || message.type() != mtpc_message) return;
	const auto &data = message.c_message();
	if (data.is_from_scheduled()) return;
	const auto peer = peerFromMTP(data.vpeer_id());
	if (const auto item = session->data().message(peer, data.vid().v)) {
		Observe(item, MediaReplyOrigin::LiveNew);
	}
}

void ObserveMediaReplyAck(not_null<HistoryItem*> item) {
	if (item->out()) Observe(item, MediaReplyOrigin::OutgoingAck);
}

void StartMediaReplyServices() {
	static auto started = false;
	if (std::exchange(started, true)) return;
	static auto lifetime = rpl::lifetime();
	static auto accounts = rpl::lifetime();
	const auto refresh = [] {
		accounts.destroy();
		for (const auto &entry : Core::App().domain().accounts()) {
			entry.account->sessionChanges() | rpl::on_next([](Main::Session *session) {
				if (session) Ensure(session);
			}, accounts);
			if (const auto session = entry.account->maybeSession()) Ensure(session);
		}
	};
	Core::App().domain().accountsChanges() | rpl::on_next(refresh, lifetime);
	Core::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);
	refresh();
}

} // namespace Purple
