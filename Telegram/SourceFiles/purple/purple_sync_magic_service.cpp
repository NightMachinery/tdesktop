#include "purple/purple_sync_magic_service.h"

#include "base/weak_ptr.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_chat_carry_gate.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_sync_magic_telegram.h"

#include <QtCore/QDir>
#include <QtCore/QUuid>

namespace Purple {
namespace {

rpl::event_stream<Main::Session*> Changes;

class Holder final : public base::has_weak_ptr {
public:
	explicit Holder(not_null<Main::Session*> session);
	~Holder();

	[[nodiscard]] bool matches() const;
	[[nodiscard]] bool current() const;
	[[nodiscard]] SyncMagicChat *chat() const;
	[[nodiscard]] const QString &syncDirectory() const;
	[[nodiscard]] DesktopSyncChatStatus status() const;
	[[nodiscard]] std::optional<DesktopSyncChatLease> lease(PeerToken chat);
	[[nodiscard]] bool owns(const DesktopSyncChatIo &route) const;

private:
	[[nodiscard]] std::optional<DesktopSyncChatPeer> peer(PeerToken chat) const;
	[[nodiscard]] PeerData *native(PeerToken chat) const;
	[[nodiscard]] bool usingChat(PeerToken chat) const;
	void refreshHidden(bool clear = false);

	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatOrigin _origin;
	QString _root;
	std::optional<AccountKey> _owner;
	QString _syncDirectory;
	std::unique_ptr<SyncMagicChat> _chat;

};

std::map<Main::Session*, PeerId> &HiddenPeers() {
	static auto result = std::map<Main::Session*, PeerId>();
	return result;
}

std::map<Main::Session*, std::unique_ptr<Holder>> &Holders() {
	static auto result = std::map<Main::Session*, std::unique_ptr<Holder>>();
	return result;
}

Holder &For(not_null<Main::Session*> session) {
	auto &holders = Holders();
	auto i = holders.find(session);
	if (i != end(holders) && !i->second->matches()) {
		holders.erase(i);
		i = end(holders);
	}
	if (i == end(holders)) {
		static auto watched = std::set<Main::Session*>();
		i = holders.emplace(session, std::make_unique<Holder>(session)).first;
		if (watched.emplace(session).second) {
			session->lifetime().add([raw = session.get()] {
				watched.erase(raw);
				HiddenPeers().erase(raw);
				Holders().erase(raw);
			});
		}
	}
	return *i->second;
}

Holder::Holder(not_null<Main::Session*> session)
: _session(session)
, _origin{
	SessionAccountKey(session),
	QUuid::createUuid().toString(QUuid::WithoutBraces) }
, _root(ConfigDirectory())
, _owner(SettingsStartupOwner()) {
	const auto files = ResolveAccountFiles(
		_origin.account,
		_owner,
		AccountFileLayout::Desktop);
	if (!files) {
		return;
	}
	_syncDirectory = QDir(_root).filePath(files->syncDirectory);
	const auto path = QDir(_syncDirectory)
		.filePath(QString::fromLatin1(kSyncMagicBindingFile));
	const auto weak = base::make_weak(this);
	_chat = std::make_unique<SyncMagicChat>(MakeSyncMagicTelegramPort(
		session,
		path,
		[=] {
			const auto self = weak.get();
			return self && self->current();
		}));
	_chat->setChanged([=] {
		const auto self = weak.get();
		if (const auto session = self ? self->_session.get() : nullptr) {
			self->refreshHidden();
			Changes.fire_copy(session);
		}
	});
}

Holder::~Holder() {
	refreshHidden(true);
}

void Holder::refreshHidden(bool clear) {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	const auto token = (!clear && _chat && current())
		? _chat->hidden()
		: std::nullopt;
	const auto next = (token && token->kind == PeerTokenKind::Channel)
		? peerFromChannel(ChannelId(token->id))
		: PeerId();
	auto &hidden = HiddenPeers();
	const auto i = hidden.find(session);
	const auto was = (i != end(hidden)) ? i->second : PeerId();
	if (was == next) {
		return;
	} else if (next) {
		hidden[session] = next;
	} else {
		hidden.erase(i);
	}
	for (const auto id : { was, next }) {
		if (const auto history = id ? session->data().historyLoaded(id) : nullptr) {
			history->updateChatListExistence();
		}
	}
}

bool Holder::matches() const {
	const auto session = _session.get();
	return session
		&& SessionAccountKey(session) == _origin.account
		&& ConfigDirectory() == _root
		&& SettingsStartupOwner() == _owner;
}

bool Holder::current() const {
	const auto session = _session.get();
	return session
		&& matches()
		&& !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& AccountSyncOperationsAllowed(session);
}

SyncMagicChat *Holder::chat() const {
	return _chat.get();
}

const QString &Holder::syncDirectory() const {
	return _syncDirectory;
}

PeerData *Holder::native(PeerToken chat) const {
	const auto session = _session.get();
	if (!session) {
		return nullptr;
	} else if (chat == PeerToken{}) {
		return session->user();
	}
	return (chat.kind == PeerTokenKind::Channel)
		? session->data().channelLoaded(ChannelId(chat.id))
		: nullptr;
}

std::optional<DesktopSyncChatPeer> Holder::peer(PeerToken chat) const {
	auto result = DesktopSyncChatPeer();
	result.origin = _origin;
	result.chat = chat;
	if (chat == PeerToken{}) {
		auto facts = SyncChatFacts();
		facts.kind = SyncChatKind::Self;
		facts.canSendFiles = true;
		result.kind = SyncChatKind::Self;
		result.facts = facts;
		return result;
	}
	const auto channel = native(chat) ? native(chat)->asChannel() : nullptr;
	if (!channel || !_chat) {
		return std::nullopt;
	}
	auto candidate = SyncMagicLocalCandidate(channel);
	if (!candidate.member || candidate.facts.kind != SyncChatKind::Supergroup) {
		return std::nullopt;
	}
	const auto &binding = _chat->binding();
	const auto verified = binding
		&& binding->mode == SyncChatBindingMode::Magic
		&& binding->chat == chat
		&& binding->verifiedAt > 0;
	if (verified) {
		candidate.facts.soleAdmin = true;
		candidate.facts.otherMembers = 0;
		candidate.facts.otherAdmins = 0;
	} else if (channel->membersCountKnown()) {
		candidate.facts.otherMembers = uint64_t(channel->membersCount() - 1);
		candidate.facts.otherAdmins = uint64_t(std::max(channel->adminsCount() - 1, 0));
		candidate.facts.soleAdmin = (channel->adminsCount() <= 1);
	}
	result.kind = SyncChatKind::Supergroup;
	result.creator = candidate.facts.creator;
	result.soleAdmin = candidate.facts.soleAdmin;
	result.facts = candidate.facts;
	return result;
}

bool Holder::usingChat(PeerToken chat) const {
	return _chat
		&& current()
		&& _chat->last().kind == SyncMagicResult::Kind::Use
		&& _chat->last().chat == chat;
}

DesktopSyncChatStatus Holder::status() const {
	auto result = DesktopSyncChatStatus();
	if (!_chat || !current()) {
		result.reason = u"account-not-available"_q;
		return result;
	}
	const auto &last = _chat->last();
	switch (last.kind) {
	case SyncMagicResult::Kind::Use: {
		const auto chat = last.chat.value_or(PeerToken());
		const auto mapped = peer(chat);
		const auto eligible = mapped
			? DesktopSyncChatEligibility(*mapped)
			: std::nullopt;
		result.phase = DesktopSyncChatPhase::Use;
		result.complete = true;
		result.postingHeld = !eligible
			|| DesktopSyncCarryGateFor(_origin.account)->postingHeld();
		result.resolution.action = SyncChatAction::Use;
		result.resolution.readChat = chat;
		result.resolution.postingHeld = result.postingHeld;
		if (eligible) {
			result.resolution.readers = eligible->readers;
		}
		result.reason = last.manual ? u"manual"_q : u"magic"_q;
	} break;
	case SyncMagicResult::Kind::Wait:
		result.phase = DesktopSyncChatPhase::Searching;
		result.retryAt = last.retryAt;
		result.reason = u"telegram-transient"_q;
		break;
	case SyncMagicResult::Kind::Problem:
		result.phase = DesktopSyncChatPhase::Paused;
		result.reason = SyncChatProblemCode(last.problem);
		break;
	case SyncMagicResult::Kind::Unavailable:
		result.phase = DesktopSyncChatPhase::Unset;
		result.reason = u"looking"_q;
		break;
	}
	return result;
}

std::optional<DesktopSyncChatLease> Holder::lease(PeerToken chat) {
	if (!usingChat(chat) || DesktopSyncCarryGateFor(_origin.account)->postingHeld()) {
		return std::nullopt;
	}
	const auto mapped = peer(chat);
	const auto target = native(chat);
	if (!mapped || !target) {
		return std::nullopt;
	}
	const auto route = DesktopSyncChatIo{ *mapped, (chat == PeerToken{})
		? MTPInputPeer(MTP_inputPeerSelf())
		: target->input() };
	const auto eligible = DesktopSyncChatEligibility(*mapped);
	if (!ValidDesktopSyncChatIo(route)
		|| !eligible
		|| eligible->readers.populationUnknown) {
		return std::nullopt;
	}
	const auto weak = base::make_weak(this);
	return DesktopSyncChatLease{ route, [weak, chat] {
		const auto self = weak.get();
		if (!self || !self->usingChat(chat)) {
			return false;
		}
		const auto session = self->_session.get();
		const auto target = self->native(chat);
		return session
			&& target
			&& Data::CanSend(target, ChatRestriction::SendFiles)
			&& session->sendAsPeers().resolveChosen(target) == session->user();
	} };
}

bool Holder::owns(const DesktopSyncChatIo &route) const {
	const auto session = _session.get();
	return session
		&& ValidDesktopSyncChatIo(route)
		&& route.peer.origin == _origin
		&& SessionAccountKey(session) == _origin.account;
}

} // namespace

DesktopSyncChatStatus SyncMagicStatusOf(not_null<Main::Session*> session) {
	return For(session).status();
}

void SyncMagicLookup(
		not_null<Main::Session*> session,
		SyncMagicTrigger trigger,
		bool fresh,
		Fn<void(DesktopSyncChatStatus)> done) {
	auto &holder = For(session);
	if (!holder.chat() || !holder.current()) {
		if (done) {
			done(holder.status());
		}
		return;
	}
	const auto weak = base::make_weak(&holder);
	holder.chat()->lookup(trigger, fresh, [=](SyncMagicResult) {
		if (done) {
			const auto self = weak.get();
			done(self ? self->status() : DesktopSyncChatStatus());
		}
	});
}

std::optional<DesktopSyncChatLease> SyncMagicLease(
		not_null<Main::Session*> session,
		PeerToken chat) {
	return For(session).lease(chat);
}

bool SyncMagicOwnsRoute(
		not_null<Main::Session*> session,
		const DesktopSyncChatIo &route) {
	return For(session).owns(route);
}

SyncMagicChat *SyncMagicChatOf(not_null<Main::Session*> session) {
	auto &holder = For(session);
	return holder.current() ? holder.chat() : nullptr;
}

std::optional<PeerToken> SyncMagicBound(not_null<Main::Session*> session) {
	const auto chat = SyncMagicChatOf(session);
	return (chat && chat->last().kind == SyncMagicResult::Kind::Use)
		? chat->last().chat
		: std::nullopt;
}

PeerData *SyncMagicBoundPeer(not_null<Main::Session*> session) {
	const auto chat = SyncMagicBound(session);
	if (!chat) {
		return nullptr;
	} else if (*chat == PeerToken{}) {
		return session->user().get();
	}
	return (chat->kind == PeerTokenKind::Channel)
		? session->data().channelLoaded(ChannelId(chat->id))
		: nullptr;
}

bool SyncMagicPaused(not_null<Main::Session*> session) {
	const auto chat = SyncMagicChatOf(session);
	return chat && chat->last().kind == SyncMagicResult::Kind::Problem;
}

void SyncMagicJoin(not_null<Main::Session*> session, Fn<void(bool)> done) {
	auto &holder = For(session);
	const auto chat = holder.current() ? holder.chat() : nullptr;
	if (!chat
		|| chat->last().kind != SyncMagicResult::Kind::Problem
		|| chat->last().problem != SyncChatProblem::SpaceMismatch
		|| holder.syncDirectory().isEmpty()) {
		done(false);
		return;
	}
	{
		auto store = SyncLocalStore(holder.syncDirectory());
		const auto token = AccountSyncBindingToken(session->account());
		if (!store.Open(true, token) || store.SetAside() != SyncStoreStatus::Ready) {
			done(false);
			return;
		}
	}
	chat->joined([=](SyncMagicResult) { done(true); });
}

bool SyncMagicHidden(not_null<const PeerData*> peer) {
	if (!peer->isChannel()) {
		return false;
	}
	const auto &hidden = HiddenPeers();
	const auto i = hidden.find(&peer->session());
	return (i != end(hidden)) && (i->second == peer->id);
}

rpl::producer<Main::Session*> SyncMagicChanges() {
	return Changes.events();
}

} // namespace Purple
