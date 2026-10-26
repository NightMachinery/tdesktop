#include "purple/purple_sync_chat_cache.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonParseError>
#include <algorithm>
#include <limits>

namespace Purple {
namespace {

std::optional<uint64_t> Number(const QJsonValue &value) {
	if (!value.isString()) {
		return {};
	}
	const auto text = value.toString();
	if (text.isEmpty() || (text.size() > 1 && text.front() == u'0')) {
		return {};
	}
	auto result = uint64_t(0);
	for (const auto digit : text) {
		if (digit < u'0' || digit > u'9'
			|| result > (uint64_t(std::numeric_limits<int64_t>::max())
						 - digit.unicode() + u'0')
					/ 10) {
			return {};
		}
		result = result * 10 + digit.unicode() - u'0';
	}
	return result;
}

bool DigestValid(const QString &text) {
	return text.size() == 64 && std::all_of(text.begin(), text.end(), [](QChar value) {
		return (value >= u'0' && value <= u'9') || (value >= u'a' && value <= u'f');
	});
}

bool ReadersValid(const SyncChatReaderFacts &readers) {
	return !(readers.privateRecipient && readers.botOwner)
		&& readers.warningRequired
		== (readers.privateRecipient || readers.botOwner
			|| readers.populationUnknown || readers.otherMembers.value_or(0) > 0
			|| readers.otherAdmins.value_or(0) > 0);
}

QJsonValue Count(std::optional<uint64_t> value) {
	return value ? QJsonValue(QString::number(*value))
				 : QJsonValue(QJsonValue::Null);
}

}

bool ReuseDesktopSyncChatDiscovery(
		const DesktopSyncChatDiscoveryReceipt &receipt,
		const DesktopSyncChatDiscoveryScope &current,
		int64_t now) {
	return receipt.completedAt > 0 && now >= receipt.completedAt
		&& now - receipt.completedAt < 15 * 60
		&& ValidDesktopSyncChatOrigin(current.origin)
		&& !current.root.isEmpty() && DigestValid(current.loginBinding)
		&& current.mark && !current.carry
		&& (current.mark->markId > 0 || current.mark->chat == PeerToken{})
		&& receipt.scope == current;
}

bool NewDesktopSyncChatReaders(
		const DesktopSyncChatCacheRecord &previous,
		const DesktopSyncChatPeer &peer) {
	const auto eligibility = DesktopSyncChatEligibility(peer);
	if (previous.mark.chat != peer.chat || !eligibility
		|| eligibility->readers.populationUnknown) {
		return false;
	}
	if (previous.readerCreator == true && peer.creator == false) {
		return true;
	}
	if (previous.readerMembers && peer.readerMembers) {
		return std::any_of(peer.readerMembers->begin(), peer.readerMembers->end(),
			[&](const QString &member) {
				return !std::binary_search(previous.readerMembers->begin(),
					previous.readerMembers->end(), member);
			});
	}
	return previous.readers.otherMembers && eligibility->readers.otherMembers
		&& *eligibility->readers.otherMembers > *previous.readers.otherMembers;
}

QString DesktopSyncChatCacheFileName(const AccountKey &account) {
	return account.environment >= 0 && account.environment <= 1
			&& account.userId > 0
		? u"purple-sync-chat-%1-%2.json"_q.arg(account.environment)
			  .arg(account.userId)
		: QString();
}

DesktopSyncChatCacheParse
ParseDesktopSyncChatCache(const QByteArray &bytes, const AccountKey &account) {
	if (bytes.isEmpty() || bytes.size() > 64 * 1024
		|| DesktopSyncChatCacheFileName(account).isEmpty()) {
		return {};
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return {};
	}
	const auto object = document.object();
	const auto version = Number(object.value(u"version"_q));
	if (!version) {
		return {};
	} else if (*version > 1) {
		return {DesktopSyncChatCacheStatus::Newer};
	} else if (*version != 1) {
		return {};
	}
	const auto environment = Number(object.value(u"environment"_q));
	const auto user = Number(object.value(u"user"_q));
	const auto id = Number(object.value(u"mark_id"_q));
	const auto date = Number(object.value(u"mark_date"_q));
	const auto peer = object.value(u"chat"_q).isString()
		? ParsePeerToken(object.value(u"chat"_q).toString(), account.userId)
		: std::nullopt;
	if (!environment || *environment != uint64_t(account.environment) || !user
		|| *user != uint64_t(account.userId) || !id || !date || !peer
		|| *id > uint64_t(std::numeric_limits<int32_t>::max())
		|| ((*id == 0) != (*date == 0)) || !object.value(u"title"_q).isString()
		|| object.value(u"title"_q).toString().size() > 4096
		|| !object.value(u"reader_consent"_q).isBool()
		|| !object.value(u"readers"_q).isObject()) {
		return {};
	}
	auto record = DesktopSyncChatCacheRecord();
	record.account = account;
	record.mark = {*peer, int32_t(*id), int64_t(*date)};
	record.title = object.value(u"title"_q).toString();
	record.loginBinding = object.value(u"login_binding"_q).toString();
	if (record.loginBinding.size() != 64
		|| record.loginBinding.toLatin1().toLower()
			!= record.loginBinding.toLatin1()
		|| QByteArray::fromHex(record.loginBinding.toLatin1()).size() != 32) {
		return {};
	}
	record.readerConsent = object.value(u"reader_consent"_q).toBool();
	const auto warning = object.value(u"new_readers"_q);
	const auto creator = object.value(u"reader_creator"_q);
	const auto memberSnapshot = object.value(u"reader_members"_q);
	if ((!warning.isUndefined() && !warning.isBool())
		|| (!creator.isUndefined() && !creator.isNull() && !creator.isBool())
		|| (!memberSnapshot.isUndefined() && !memberSnapshot.isNull() && !memberSnapshot.isArray())) {
		return {};
	}
	record.newReaders = warning.toBool();
	if (creator.isBool()) record.readerCreator = creator.toBool();
	if (memberSnapshot.isArray()) {
		const auto array = memberSnapshot.toArray();
		if (array.size() > 200) return {};
		record.readerMembers = std::vector<QString>();
		for (const auto &value : array) {
			const auto member = value.toString();
			if (!value.isString() || !DigestValid(member)
				|| (!record.readerMembers->empty() && record.readerMembers->back() >= member)) {
				return {};
			}
			record.readerMembers->push_back(member);
		}
	}
	const auto readers = object.value(u"readers"_q).toObject();
	for (const auto &name :
		 {u"warning"_q, u"private"_q, u"bot"_q, u"unknown"_q}) {
		if (!readers.value(name).isBool()) {
			return {};
		}
	}
	const auto members = Number(readers.value(u"members"_q));
	const auto admins = Number(readers.value(u"admins"_q));
	if ((!members && !readers.value(u"members"_q).isNull())
		|| (!admins && !readers.value(u"admins"_q).isNull())) {
		return {};
	}
	record.readers = {
		readers.value(u"warning"_q).toBool(),
		readers.value(u"private"_q).toBool(),
		readers.value(u"bot"_q).toBool(),
		readers.value(u"unknown"_q).toBool(),
		members,
		admins};
	if (!ReadersValid(record.readers)) {
		return {};
	}
	if (object.contains(u"carry"_q)) {
		if (!object.value(u"carry"_q).isObject()) {
			return {};
		}
		record.carry = object.value(u"carry"_q).toObject();
	}
	record.preserved = object;
	return {DesktopSyncChatCacheStatus::Valid, record};
}

std::optional<QByteArray> SerializeDesktopSyncChatCache(const DesktopSyncChatCacheRecord &record) {
	if (DesktopSyncChatCacheFileName(record.account).isEmpty()
		|| !ReadersValid(record.readers)) {
		return {};
	}
	const auto chat = PeerTokenText(record.mark.chat, record.account.userId);
	if (!chat) {
		return {};
	}
	auto object = record.preserved;
	object.insert(u"version"_q, u"1"_q);
	object.insert(u"login_binding"_q, record.loginBinding);
	object.insert(
		u"environment"_q, QString::number(record.account.environment));
	object.insert(u"user"_q, QString::number(record.account.userId));
	object.insert(u"chat"_q, *chat);
	object.insert(u"mark_id"_q, QString::number(record.mark.markId));
	object.insert(u"mark_date"_q, QString::number(record.mark.markDate));
	object.insert(u"title"_q, record.title);
	object.insert(u"reader_consent"_q, record.readerConsent);
	object.insert(u"new_readers"_q, record.newReaders);
	object.insert(u"reader_creator"_q, record.readerCreator
		? QJsonValue(*record.readerCreator) : QJsonValue(QJsonValue::Null));
	auto memberSnapshot = QJsonValue(QJsonValue::Null);
	if (record.readerMembers) {
		auto array = QJsonArray();
		for (const auto &member : *record.readerMembers) array.push_back(member);
		memberSnapshot = array;
	}
	object.insert(u"reader_members"_q, memberSnapshot);
	object.insert(
		u"readers"_q,
		QJsonObject{
			{u"warning"_q, record.readers.warningRequired},
			{u"private"_q, record.readers.privateRecipient},
			{u"bot"_q, record.readers.botOwner},
			{u"unknown"_q, record.readers.populationUnknown},
			{u"members"_q, Count(record.readers.otherMembers)},
			{u"admins"_q, Count(record.readers.otherAdmins)}});
	if (record.carry) {
		object.insert(u"carry"_q, *record.carry);
	} else {
		object.remove(u"carry"_q);
	}
	const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
	return ParseDesktopSyncChatCache(bytes, record.account).record
		? std::optional(bytes)
		: std::nullopt;
}

bool DesktopSyncChatCacheCanCommit(
	const std::optional<DesktopSyncChatCacheRecord> &previous,
	const SyncChatResolveInput &input,
	const SyncChatResolution &resolution,
	bool completeDiscovery,
	bool current) {
	if (!current || !completeDiscovery
		|| resolution.action != SyncChatAction::Use || resolution.postingHeld
		|| !resolution.cache || (previous && previous->carry)
		|| (input.settingsBound && input.settingsChat != resolution.cache->chat)
		|| (previous && previous->mark.chat != resolution.cache->chat)) {
		return false;
	}
	return (resolution.cache->markId == 0
			&& resolution.cache->chat == PeerToken{} && input.cachedEligibility
			&& input.cacheRead == SyncChatDirectRead::Valid)
		|| std::any_of(
			   input.marks.begin(),
			   input.marks.end(),
			   [&](const SyncChatMark &mark) {
				   return mark.location == *resolution.cache && mark.freshlyRead
					   && mark.readable && bool(mark.eligibility);
			   });
}

}
