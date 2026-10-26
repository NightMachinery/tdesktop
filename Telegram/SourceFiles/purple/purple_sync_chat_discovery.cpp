#include "purple/purple_sync_chat_discovery.h"

#include <algorithm>

namespace Purple {
namespace {

std::optional<SyncChatCache> Position(const DesktopSyncChatOrigin &origin, const MTPMessage &message) {
	if (message.type() != mtpc_message) {
		return {};
	}
	const auto &data = message.c_message();
	const auto chat =
		DesktopSyncChatToken(data.vpeer_id(), origin.account.userId);
	return chat && data.vid().v > 0 && data.vdate().v > 0
		? std::optional(SyncChatCache{*chat, data.vid().v, data.vdate().v})
		: std::nullopt;
}

bool HasMedia(const MTPDmessage &message) {
	return message.vmedia()
		&& message.vmedia()->type() != mtpc_messageMediaEmpty;
}

}

DesktopSyncChatReply MapDesktopSyncChatInventoryReply(
		const DesktopSyncChatOrigin &origin,
		uint64_t ticket,
		const SyncAccountInventoryResult &inventory) {
	auto reply = DesktopSyncChatReply{origin, ticket};
	if (!ValidDesktopSyncChatOrigin(origin)
		|| inventory.accountUserId != origin.account.userId
		|| inventory.status != SyncAccountInventoryStatus::Complete
		|| !inventory.scan.complete()
		|| inventory.scan.candidateIds.size() > inventory.scan.scannedCount || !inventory.read
		|| !inventory.read->complete()
		|| AggregateSyncCandidateRead(inventory.read->records)
			!= SyncCandidateReadStatus::Complete) {
		return reply;
	}
	auto ids = std::set<int32_t>();
	for (const auto id : inventory.scan.candidateIds) {
		if (id <= 0 || !ids.emplace(id).second) {
			return reply;
		}
	}
	for (const auto &record : inventory.read->records) {
		if (record.id <= 0 || !ids.erase(record.id)) {
			return reply;
		}
	}
	if (!ids.empty() || inventory.scan.candidateIds.size() != inventory.read->records.size()
		|| FinishSyncAccountInventory(inventory.accountUserId, inventory.scan,
			inventory.read).status != SyncAccountInventoryStatus::Complete) {
		return reply;
	}
	reply.success = true;
	reply.chatReadable = true;
	reply.records = inventory.read->records.empty()
		? SyncChatRecordPresence::Empty : SyncChatRecordPresence::Present;
	return reply;
}

DesktopSyncChatReply MapDesktopSyncChatInventoryReply(
		const DesktopSyncChatOrigin &origin,
		uint64_t ticket,
		PeerToken chat,
		const DesktopSyncChatInventory &inventory) {
	return DesktopSyncChatInventoryMatches(inventory, origin, chat)
		? MapDesktopSyncChatInventoryReply(origin, ticket, inventory.inventory)
		: DesktopSyncChatReply{ origin, ticket };
}

DesktopSyncChatDiscovery::DesktopSyncChatDiscovery(
	DesktopSyncChatOrigin origin,
	std::optional<DesktopSyncChatCacheRecord> cache,
	bool settingsBound,
	PeerToken settingsChat,
	std::vector<PeerToken> loadedDialogs,
	QString loginBinding)
: _origin(std::move(origin))
, _loginBinding(std::move(loginBinding))
, _cache(std::move(cache))
, _dialogs(std::move(loadedDialogs)) {
	_input.selfUserId = _origin.account.userId;
	_input.settingsBound = settingsBound;
	_input.settingsChat = settingsChat;
	if (_cache) {
		_input.cache = _cache->mark;
	}
	if (_dialogs.size() > 50) {
		_dialogs.resize(50);
	}
}

void DesktopSyncChatDiscovery::Start(int64_t now) {
	if (_command.step != DesktopSyncChatStep::Idle
		|| (_status.phase == DesktopSyncChatPhase::Searching
			&& now < _status.retryAt)) {
		return;
	}
	Begin(now, false);
}

void DesktopSyncChatDiscovery::Begin(int64_t now, bool retry) {
	if (!retry) {
		_retries = 0;
	}
	_status = {};
	_status.reason = u"resolving"_q;
	_status.postingHeld = true;
	if (!ValidDesktopSyncChatOrigin(_origin) || _loginBinding.size() != 64
		|| now <= 0
		|| (_input.settingsBound
			&& !PeerTokenText(_input.settingsChat, _origin.account.userId))
		|| (_cache
			&& (_cache->account != _origin.account
				|| _cache->loginBinding != _loginBinding || _cache->carry
				|| !SerializeDesktopSyncChatCache(*_cache)))) {
		Finish(u"cache-or-origin-held"_q);
		return;
	}
	_input.marks.clear();
	_input.cacheRead = SyncChatDirectRead::Unknown;
	_input.settingsHint = SyncChatSettingsHint::NotRun;
	_input.readerWarningAccepted = false;
	_input.cachedEligibility = {};
	_input.searchRetriesCompleted = _retries;
	_hits.clear();
	_facts.clear();
	_peers.clear();
	_inventories.clear();
	_verified.clear();
	_peerIndex = _dialogIndex = 0;
	_complete = true;
	_globalCapped = _fallbackUsed = _uncertain = _cacheRead = false;
	Search(DesktopSyncChatSearch::Global);
}

void DesktopSyncChatDiscovery::Tick(int64_t now) {
	if (_command.step == DesktopSyncChatStep::Idle
		&& _status.phase == DesktopSyncChatPhase::Searching
		&& _status.retryAt > 0 && now >= _status.retryAt) {
		++_retries;
		Begin(now, true);
	}
}

void DesktopSyncChatDiscovery::Cancel() {
	++_ticket;
	_command = {_origin, _ticket};
	Finish(u"origin-retired"_q);
}

void DesktopSyncChatDiscovery::Issue(
	DesktopSyncChatStep step,
	PeerToken chat,
	std::optional<SyncChatCache> mark) {
	_command = {
		_origin,
		++_ticket,
		step,
		DesktopSyncChatSearch::Global,
		{},
		chat,
		mark};
}

void DesktopSyncChatDiscovery::Search(DesktopSyncChatSearch kind) {
	_pages = std::make_unique<DesktopSyncChatSearchPages>(_origin, kind);
	const auto chat = kind == DesktopSyncChatSearch::Dialog
		? _dialogs[_dialogIndex]
		: PeerToken{};
	Issue(DesktopSyncChatStep::Search, chat);
	_command.search = kind;
}

void DesktopSyncChatDiscovery::Remember(
	const std::vector<MTPMessage> &messages) {
	for (const auto &message : messages) {
		if (message.type() != mtpc_message) {
			continue;
		}
		const auto &data = message.c_message();
		if (ParseSyncChatMark(
				qs(data.vmessage()),
				HasMedia(data),
				SyncChatMessageOwnership::Own)
				.status
			== SyncChatMarkStatus::NotMark) {
			continue;
		}
		if (_hits.size() >= 10000) {
			_complete = false;
			_uncertain = true;
			return;
		}
		_hits.push_back(message);
	}
}

void DesktopSyncChatDiscovery::SearchDone(int64_t now) {
	const auto kind = _command.search;
	if (kind == DesktopSyncChatSearch::Global) {
		Search(DesktopSyncChatSearch::Self);
		return;
	} else if (kind == DesktopSyncChatSearch::Self && _globalCapped) {
		Search(DesktopSyncChatSearch::Broadcasts);
		return;
	} else if (
		kind == DesktopSyncChatSearch::Dialog
		&& ++_dialogIndex < _dialogs.size()) {
		Search(DesktopSyncChatSearch::Dialog);
		return;
	}
	if (!_fallbackUsed && !_dialogs.empty()
		&& (_globalCapped || _hits.empty())) {
		_fallbackUsed = true;
		_dialogIndex = 0;
		Search(DesktopSyncChatSearch::Dialog);
		return;
	}
	_peers.clear();
	const auto add = [&](PeerToken chat) {
		if (std::find(_peers.begin(), _peers.end(), chat) == _peers.end()) {
			_peers.push_back(chat);
		}
	};
	for (const auto &hit : _hits) {
		if (const auto position = Position(_origin, hit)) {
			add(position->chat);
		} else {
			_uncertain = true;
		}
	}
	if (_input.cache) {
		add(_input.cache->chat);
	} else if (_input.settingsBound) {
		add(_input.settingsChat);
	}
	if (_peers.empty()) {
		add({});
	}
	_peerIndex = 0;
	PeerNext(now);
}

void DesktopSyncChatDiscovery::PeerNext(int64_t now) {
	if (_peerIndex < _peers.size()) {
		Issue(DesktopSyncChatStep::PeerInfo, _peers[_peerIndex]);
		return;
	}
	_input.marks.clear();
	for (const auto &hit : _hits) {
		const auto position = Position(_origin, hit);
		if (!position) {
			continue;
		}
		const auto key = *PeerTokenText(position->chat, _origin.account.userId);
		const auto found = _facts.find(key);
		if (found == _facts.end()) {
			_uncertain = true;
			continue;
		}
		const auto ownership = GetSyncChatMessageOwnership(
			MapDesktopSyncChatMessage(_origin, found->second, hit));
		const auto parsed = ParseSyncChatMark(
			qs(hit.c_message().vmessage()),
			HasMedia(hit.c_message()),
			ownership);
		if (ownership == SyncChatMessageOwnership::Unknown) {
			_uncertain = true;
		}
		if (parsed.status == SyncChatMarkStatus::NotMark) {
			continue;
		}
		const auto eligibility = DesktopSyncChatEligibility(found->second);
		if (!eligibility) {
			_uncertain = true;
		}
		_input.marks.push_back(
			{*position,
			 parsed.status,
			 eligibility.value_or(SyncChatEligibilityResult()),
			 SyncChatRecordPresence::Unknown,
			 false,
			 false});
	}
	if (_input.marks.empty() && !_fallbackUsed && !_dialogs.empty()) {
		_fallbackUsed = true;
		_dialogIndex = 0;
		Search(DesktopSyncChatSearch::Dialog);
		return;
	}
	const auto effective = _input.cache ? _input.cache
		: _input.settingsBound
		? std::optional(SyncChatCache{_input.settingsChat, 0, 0})
		: std::nullopt;
	if (effective) {
		const auto found = _facts.find(
			*PeerTokenText(effective->chat, _origin.account.userId));
		if (found != _facts.end()) {
			const auto eligible = DesktopSyncChatEligibility(found->second);
			if (eligible) {
				_input.cachedEligibility = *eligible;
			}
		}
		if (!_cacheRead) {
			_cacheRead = true;
			Issue(DesktopSyncChatStep::ExactMark, effective->chat, effective);
			return;
		}
	} else if (
		_input.marks.empty()
		&& _input.settingsHint == SyncChatSettingsHint::NotRun) {
		Issue(DesktopSyncChatStep::SettingsHint);
		return;
	}
	Resolve(now);
}

bool DesktopSyncChatDiscovery::Complete(
	const DesktopSyncChatReply &reply, int64_t now) {
	if (_command.step == DesktopSyncChatStep::Idle || reply.origin != _origin
		|| reply.ticket != _command.ticket) {
		return false;
	}
	if (_command.step == DesktopSyncChatStep::Search) {
		if (reply.success) {
			Remember(reply.messages);
		}
		const auto state = reply.success && reply.page
			? _pages->Add(*reply.page)
			: DesktopSyncChatPageStatus::Incomplete;
		if (state == DesktopSyncChatPageStatus::Next) {
			const auto kind = _command.search;
			const auto chat = _command.chat;
			Issue(DesktopSyncChatStep::Search, chat);
			_command.search = kind;
			_command.cursor = _pages->cursor();
			return true;
		}
		if (state != DesktopSyncChatPageStatus::Complete) {
			_complete = false;
		}
		if (_command.search == DesktopSyncChatSearch::Global
			&& state == DesktopSyncChatPageStatus::Capped) {
			_globalCapped = true;
		}
		SearchDone(now);
	} else if (_command.step == DesktopSyncChatStep::PeerInfo) {
		if (reply.success && reply.peer && reply.peer->origin == _origin
			&& reply.peer->chat == _command.chat) {
			_facts[*PeerTokenText(_command.chat, _origin.account.userId)] =
				*reply.peer;
		} else {
			_uncertain = true;
		}
		++_peerIndex;
		PeerNext(now);
	} else if (_command.step == DesktopSyncChatStep::ExactMark) {
		Exact(reply, now);
	} else if (_command.step == DesktopSyncChatStep::SettingsHint) {
		_input.settingsHint =
			reply.success ? reply.hint : SyncChatSettingsHint::Unknown;
		if (_input.settingsHint != SyncChatSettingsHint::Found
			&& _input.settingsHint != SyncChatSettingsHint::Absent) {
			Finish(u"settings-hint-unknown"_q);
		} else {
			_inventories.emplace(*PeerTokenText(PeerToken{}, _origin.account.userId));
			Issue(DesktopSyncChatStep::Inventory);
		}
	} else if (_command.step == DesktopSyncChatStep::Inventory) {
		if (!reply.success || reply.records == SyncChatRecordPresence::Unknown
			|| reply.chatReadable != true) {
			Finish(u"winner-inventory-held"_q);
			return true;
		}
		if (_input.marks.empty() && _command.chat == PeerToken{}) {
			_input.settingsHint =
				reply.records == SyncChatRecordPresence::Present
				? SyncChatSettingsHint::Found
				: SyncChatSettingsHint::Absent;
		}
		for (auto &mark : _input.marks) {
			if (mark.location.chat == _command.chat) {
				mark.records = reply.records;
				mark.readable = true;
			}
		}
		Resolve(now);
	}
	return true;
}

void DesktopSyncChatDiscovery::Exact(
	const DesktopSyncChatReply &reply, int64_t now) {
	const auto expected = *_command.mark;
	auto outcome = SyncChatDirectRead::Unknown;
	if (reply.success && reply.peer && reply.peer->origin == _origin
		&& reply.peer->chat == expected.chat && reply.chatReadable == true) {
		const auto eligibility = DesktopSyncChatEligibility(*reply.peer);
		if (!eligibility) {
			_uncertain = true;
		} else {
			_facts[*PeerTokenText(expected.chat, _origin.account.userId)] =
				*reply.peer;
			if ((_input.cache && _input.cache->chat == expected.chat)
				|| (_input.settingsBound
					&& _input.settingsChat == expected.chat)) {
				_input.cachedEligibility = *eligibility;
			}
			outcome = expected.markId == 0 && expected.chat == PeerToken{}
				? SyncChatDirectRead::Valid
				: SyncChatDirectRead::MarkGone;
			if (reply.messages.size() > 1) {
				outcome = SyncChatDirectRead::Unknown;
			}
			for (const auto &message : reply.messages) {
				if (outcome == SyncChatDirectRead::Unknown) {
					break;
				}
				if (message.type() == mtpc_messageEmpty) {
					const auto &empty = message.c_messageEmpty();
					if (empty.vid().v != expected.markId
						|| (empty.vpeer_id()
							&& DesktopSyncChatToken(
								   *empty.vpeer_id(), _origin.account.userId)
								!= expected.chat)) {
						outcome = SyncChatDirectRead::Unknown;
					}
					continue;
				} else if (message.type() == mtpc_messageService) {
					const auto &service = message.c_messageService();
					if (service.vid().v != expected.markId
						|| DesktopSyncChatToken(
							   service.vpeer_id(), _origin.account.userId)
							!= expected.chat) {
						outcome = SyncChatDirectRead::Unknown;
					}
					continue;
				}
				const auto position = Position(_origin, message);
				if (!position || position->chat != expected.chat
					|| position->markId != expected.markId) {
					outcome = SyncChatDirectRead::Unknown;
					break;
				}
				const auto ownership = GetSyncChatMessageOwnership(
					MapDesktopSyncChatMessage(_origin, *reply.peer, message));
				if (ownership == SyncChatMessageOwnership::Unknown) {
					outcome = SyncChatDirectRead::Unknown;
					break;
				}
				const auto parsed = ParseSyncChatMark(
					qs(message.c_message().vmessage()),
					HasMedia(message.c_message()),
					ownership);
				if (parsed.status != SyncChatMarkStatus::NotMark
					&& expected.markDate
					&& expected.markDate != position->markDate) {
					outcome = SyncChatDirectRead::Unknown;
					break;
				}
				if (parsed.status != SyncChatMarkStatus::NotMark) {
					outcome = SyncChatDirectRead::Valid;
					std::erase_if(_input.marks, [&](const SyncChatMark &mark) {
						return mark.location.chat == expected.chat
							&& mark.location.markId == expected.markId;
					});
					_input.marks.push_back(
						{*position,
						 parsed.status,
						 *eligibility,
						 SyncChatRecordPresence::Unknown,
						 true,
						 true});
				}
			}
		}
	} else if (reply.confirmedChatGone && reply.chatReadable == false) {
		outcome = SyncChatDirectRead::ChatGone;
	}
	const auto cached = _input.cache ? _input.cache
		: _input.settingsBound
		? std::optional(SyncChatCache{_input.settingsChat, 0, 0})
		: std::nullopt;
	if (cached && cached->chat == expected.chat
		&& cached->markId == expected.markId) {
		_input.cacheRead = outcome;
	}
	if (outcome == SyncChatDirectRead::Unknown) {
		_uncertain = true;
		Finish(u"exact-read-unknown"_q);
		return;
	} else if (outcome != SyncChatDirectRead::Valid) {
		std::erase_if(_input.marks, [&](const SyncChatMark &mark) {
			return mark.location.chat == expected.chat
				&& mark.location.markId == expected.markId;
		});
	}
	_verified.emplace(
		*PeerTokenText(expected.chat, _origin.account.userId), expected.markId);
	Resolve(now);
}

void DesktopSyncChatDiscovery::Resolve(int64_t now) {
	_input.readerWarningAccepted = false;
	for (const auto &mark : _input.marks) {
		const auto key = std::make_pair(
			*PeerTokenText(mark.location.chat, _origin.account.userId),
			mark.location.markId);
		if (mark.eligibility && !mark.freshlyRead && !_verified.contains(key)) {
			Issue(
				DesktopSyncChatStep::ExactMark,
				mark.location.chat,
				mark.location);
			return;
		}
	}
	if (!_input.cache && !_input.settingsBound && !_input.marks.empty()
		&& std::none_of(
			_input.marks.begin(),
			_input.marks.end(),
			[](const SyncChatMark &mark) {
				return bool(mark.eligibility);
			})) {
		Finish(u"marked-chat-ineligible"_q);
		return;
	}
	auto resolved = ResolveSyncChat(_input);
	if (_cache && resolved.cache && resolved.cache->chat == _cache->mark.chat
		&& _cache->readerConsent && _cache->readers == resolved.readers) {
		_input.readerWarningAccepted = true;
		resolved = ResolveSyncChat(_input);
	}
	_status.resolution = resolved;
	if (resolved.action == SyncChatAction::VerifyMark && resolved.winner) {
		if (!_verified.contains(
				{*PeerTokenText(resolved.winner->chat, _origin.account.userId),
				 resolved.winner->markId})) {
			Issue(
				DesktopSyncChatStep::ExactMark,
				resolved.winner->chat,
				resolved.winner);
			return;
		}
	}
	if (resolved.action == SyncChatAction::ReadWinnerInventory
		&& resolved.winner) {
		const auto key =
			*PeerTokenText(resolved.winner->chat, _origin.account.userId);
		if (_inventories.emplace(key).second) {
			Issue(DesktopSyncChatStep::Inventory, resolved.winner->chat);
			return;
		}
	}
	if (resolved.action == SyncChatAction::Use && resolved.cache
		&& !_input.settingsBound && !_input.cache) {
		const auto key =
			*PeerTokenText(resolved.cache->chat, _origin.account.userId);
		if (_inventories.emplace(key).second) {
			Issue(DesktopSyncChatStep::Inventory, resolved.cache->chat);
			return;
		}
	}
	_status.complete = _complete && !_uncertain;
	_status.postingHeld = true;
	if (!_status.complete) {
		Finish(u"discovery-incomplete"_q);
		return;
	}
	_status.reason.clear();
	if (resolved.action == SyncChatAction::Searching) {
		_status.phase = DesktopSyncChatPhase::Searching;
		_status.retryAt = now + resolved.retryAfterSeconds;
	} else if (resolved.action == SyncChatAction::Lost) {
		_status.phase = DesktopSyncChatPhase::Lost;
	} else if (resolved.action == SyncChatAction::Unset) {
		_status.phase = DesktopSyncChatPhase::Unset;
	} else if (resolved.action == SyncChatAction::Use) {
		_status.phase = DesktopSyncChatPhase::Use;
		_status.postingHeld = resolved.postingHeld;
		if (resolved.cache
			&& DesktopSyncChatCacheCanCommit(
				_cache, _input, resolved, true, true)) {
			auto next = _cache.value_or(DesktopSyncChatCacheRecord());
			next.account = _origin.account;
			next.loginBinding = _loginBinding;
			next.mark = *resolved.cache;
			next.readers = resolved.readers;
			next.readerConsent = _input.readerWarningAccepted;
			_cache = next;
		}
	} else {
		_status.phase = DesktopSyncChatPhase::Paused;
		_status.reason = resolved.action == SyncChatAction::Follow
			? u"follow-required"_q
			: resolved.action == SyncChatAction::Fill ? u"fill-required"_q
			: resolved.action == SyncChatAction::NewerMark
			? u"newer-mark"_q
			: u"resolution-held"_q;
	}
	_command = {_origin, ++_ticket};
}

void DesktopSyncChatDiscovery::Finish(QString reason) {
	_status.phase = DesktopSyncChatPhase::Paused;
	_status.postingHeld = true;
	_status.complete = false;
	if (!_status.resolution.readChat) {
		const auto cached = _input.cache ? _input.cache
			: _input.settingsBound
			? std::optional(SyncChatCache{_input.settingsChat, 0, 0})
			: std::nullopt;
		if (cached) {
			_status.resolution.cache = cached;
			_status.resolution.readChat = cached->chat;
		}
	}
	_status.reason = std::move(reason);
	_status.retryAt = 0;
	_command = {_origin, ++_ticket};
}

DesktopSyncChatCommand DesktopSyncChatDiscovery::command() const {
	return _command;
}
const DesktopSyncChatStatus &DesktopSyncChatDiscovery::status() const {
	return _status;
}
const SyncChatResolveInput &DesktopSyncChatDiscovery::input() const {
	return _input;
}
std::optional<DesktopSyncChatPeer> DesktopSyncChatDiscovery::peer(
		PeerToken chat) const {
	const auto key = PeerTokenText(chat, _origin.account.userId);
	if (!key) {
		return {};
	}
	const auto found = _facts.find(*key);
	return found == _facts.end() ? std::nullopt : std::optional(found->second);
}
auto DesktopSyncChatDiscovery::cache() const
-> const std::optional<DesktopSyncChatCacheRecord> & {
	return _cache;
}

}
