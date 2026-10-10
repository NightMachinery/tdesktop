/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_user_privacy.h"

#include "data/data_peer.h"
#include "data/data_peer_id.h"

#include <range/v3/algorithm/any_of.hpp>

namespace Purple {

bool Representable(
		const MTPVector<MTPPrivacyRule> &rules,
		const Api::UserPrivacy::Rule &parsed) {
	using UserPrivacy = Api::UserPrivacy;
	const auto containsUser = [](const auto &exceptions, MTPlong id) {
		return ranges::any_of(exceptions.peers, [&](const auto peer) {
			return peerIsUser(peer->id)
				&& peerToUser(peer->id).bare == id.v;
		});
	};
	const auto containsChat = [](const auto &exceptions, MTPlong id) {
		return ranges::any_of(exceptions.peers, [&](const auto peer) {
			return !peerIsUser(peer->id)
				&& peerToBareMTPInt(peer->id).v == id.v;
		});
	};
	auto options = 0;
	auto valid = true;
	auto stage = 0;
	for (const auto &rule : rules.v) {
		const auto nextStage = [&] {
			switch (rule.type()) {
			case mtpc_privacyValueAllowUsers:
			case mtpc_privacyValueAllowChatParticipants:
			case mtpc_privacyValueAllowPremium:
			case mtpc_privacyValueAllowBots: return 0;
			case mtpc_privacyValueDisallowUsers:
			case mtpc_privacyValueDisallowChatParticipants:
			case mtpc_privacyValueDisallowBots:
			case mtpc_privacyValueDisallowContacts: return 1;
			default: return 2;
			}
		}();
		valid = valid && nextStage >= stage;
		stage = nextStage;
		rule.match([&](const MTPDprivacyValueAllowAll &) {
			++options;
			valid = valid && parsed.option == UserPrivacy::Option::Everyone;
		}, [&](const MTPDprivacyValueAllowContacts &) {
			++options;
			valid = valid && parsed.option == UserPrivacy::Option::Contacts;
		}, [&](const MTPDprivacyValueAllowCloseFriends &) {
			++options;
			valid = valid && parsed.option == UserPrivacy::Option::CloseFriends;
		}, [&](const MTPDprivacyValueDisallowAll &) {
			++options;
			valid = valid && parsed.option == UserPrivacy::Option::Nobody;
		}, [&](const MTPDprivacyValueDisallowContacts &) {
			valid = false;
		}, [&](const MTPDprivacyValueAllowPremium &) {
			valid = valid && parsed.always.premiums
				&& parsed.option != UserPrivacy::Option::Everyone;
		}, [&](const MTPDprivacyValueAllowBots &) {
			valid = valid && parsed.always.miniapps
				&& parsed.option != UserPrivacy::Option::Everyone;
		}, [&](const MTPDprivacyValueDisallowBots &) {
			valid = valid && parsed.never.miniapps
				&& parsed.option != UserPrivacy::Option::Nobody;
		}, [&](const MTPDprivacyValueAllowUsers &data) {
			for (const auto &id : data.vusers().v) {
				valid = valid && containsUser(parsed.always, id);
			}
		}, [&](const MTPDprivacyValueDisallowUsers &data) {
			for (const auto &id : data.vusers().v) {
				valid = valid && containsUser(parsed.never, id);
			}
		}, [&](const MTPDprivacyValueAllowChatParticipants &data) {
			for (const auto &id : data.vchats().v) {
				valid = valid && containsChat(parsed.always, id);
			}
		}, [&](const MTPDprivacyValueDisallowChatParticipants &data) {
			for (const auto &id : data.vchats().v) {
				valid = valid && containsChat(parsed.never, id);
			}
		});
	}
	return valid && options == 1;
}

} // namespace Purple
