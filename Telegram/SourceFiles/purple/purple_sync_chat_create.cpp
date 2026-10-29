#include "purple/purple_sync_chat_create.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUuid>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace Purple {

QJsonObject ReaderSnapshot(const DesktopSyncChatPeer &peer);

namespace {

bool Hex(const QString &text, int length) {
	return text.size() == length && std::all_of(text.begin(), text.end(), [](QChar ch) {
		return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
	});
}

QString Hash(const QJsonObject &object) {
	return QString::fromLatin1(QCryptographicHash::hash(
		QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

std::optional<MTPUpdates> Updates(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.size() > 256 * 1024 || bytes.size() % sizeof(mtpPrime)) {
		return {};
	}
	auto words = mtpBuffer(bytes.size() / sizeof(mtpPrime));
	std::memcpy(words.data(), bytes.constData(), bytes.size());
	const auto begin = words.constData();
	auto from = begin;
	auto result = MTPUpdates();
	return result.read(from, begin + words.size()) && from == begin + words.size()
		? std::optional(result) : std::nullopt;
}

bool ValidSnapshot(const QJsonObject &o, PeerToken chat, uint64_t self) {
	if (o.size() != 10 || !o.value(u"chat"_q).isString()
		|| ParsePeerToken(o.value(u"chat"_q).toString(), self) != chat
		|| !o.value(u"kind"_q).isDouble()
		|| o.value(u"kind"_q).toDouble() != o.value(u"kind"_q).toInt(-1)
		|| !o.value(u"members"_q).isArray()) return false;
	for (const auto &name : {u"has_members"_q,u"creator"_q,u"sole_admin"_q,u"private"_q,u"bot"_q}) {
		if (!o.value(name).isBool()) return false;
	}
	const auto kind = SyncChatKind(o.value(u"kind"_q).toInt());
	const auto typed = chat.kind == PeerTokenKind::Self ? kind == SyncChatKind::Self
		: chat.kind == PeerTokenKind::User ? kind == SyncChatKind::Private || kind == SyncChatKind::Bot
		: chat.kind == PeerTokenKind::Chat ? kind == SyncChatKind::BasicGroup
		: kind == SyncChatKind::Supergroup || kind == SyncChatKind::Broadcast;
	if (!typed || o.value(u"private"_q).toBool() != (kind == SyncChatKind::Private)
		|| o.value(u"bot"_q).toBool() != (kind == SyncChatKind::Bot)) return false;
	for (const auto &name : {u"others"_q,u"admins"_q}) {
		const auto value=o.value(name);
		if (!value.isString()) return false;
		auto okay=false; const auto count=value.toString().toULongLong(&okay);
		if (!okay || count > uint64_t(std::numeric_limits<int64_t>::max())
			|| QString::number(count)!=value.toString()) return false;
	}
	const auto members=o.value(u"members"_q).toArray();
	if (members.size()>200 || (!o.value(u"has_members"_q).toBool() && !members.isEmpty())) return false;
	auto previous=QString();
	for (const auto value : members) {
		if (!value.isString() || !Hex(value.toString(),64) || value.toString()<=previous) return false;
		previous=value.toString();
	}
	return true;
}

DesktopSyncChatCacheRecord ReaderBaseline(const DesktopSyncChatCreateJournal &j) {
	auto result=DesktopSyncChatCacheRecord();
	const auto &o=j.readerSnapshot;
	const auto kind=SyncChatKind(o.value(u"kind"_q).toInt());
	result.account=j.account; result.mark.chat=j.chat.value_or(PeerToken{});
	result.readerConsent=true; result.readers.privateRecipient=kind==SyncChatKind::Private;
	result.readers.botOwner=kind==SyncChatKind::Bot;
	if (kind==SyncChatKind::BasicGroup || kind==SyncChatKind::Supergroup || kind==SyncChatKind::Broadcast) {
		result.readers.otherMembers=o.value(u"others"_q).toString().toULongLong();
		result.readers.otherAdmins=o.value(u"admins"_q).toString().toULongLong();
	}
	result.readers.warningRequired=result.readers.privateRecipient || result.readers.botOwner
		|| result.readers.otherMembers.value_or(0)>0 || result.readers.otherAdmins.value_or(0)>0;
	result.readerCreator=o.value(u"creator"_q).toBool();
	if (o.value(u"has_members"_q).toBool()) {
		result.readerMembers=std::vector<QString>();
		for (const auto value:o.value(u"members"_q).toArray()) result.readerMembers->push_back(value.toString());
	}
	result.newReaders=j.newReaders;
	return result;
}

bool Valid(const DesktopSyncChatCreateJournal &j, uint64_t self) {
	const auto step = int(j.step);
	const auto token = j.chat ? PeerTokenText(*j.chat, self) : std::nullopt;
	return self > 0 && self <= uint64_t(std::numeric_limits<int64_t>::max())
		&& j.account.userId == self && (j.account.environment == 0 || j.account.environment == 1)
		&& Hex(j.authority, 64) && Hex(j.operation, 32)
		&& (j.provisionSource.isEmpty() || (Hex(j.provisionSource, 64)
			&& j.choice == DesktopSyncChatSetupChoice::Create && !j.adopted
			&& j.markId == 0 && j.markDate == 0 && !j.decorationAfterCache
			&& (j.step == DesktopSyncChatCreateStep::Create
				|| j.step == DesktopSyncChatCreateStep::ApplyUpdates
				|| j.step == DesktopSyncChatCreateStep::PeerInfo
				|| j.step == DesktopSyncChatCreateStep::Inventory
				|| j.step == DesktopSyncChatCreateStep::Done)))
		&& j.choice >= DesktopSyncChatSetupChoice::Create
		&& j.choice <= DesktopSyncChatSetupChoice::Existing
		&& step >= int(DesktopSyncChatCreateStep::Create)
		&& step <= int(DesktopSyncChatCreateStep::Done)
		&& j.step != DesktopSyncChatCreateStep::ReconcileCreate
		&& j.step != DesktopSyncChatCreateStep::ReconcileMark
		&& j.createAttempts >= 0 && j.createAttempts <= 3 && j.retryAt >= 0
		&& j.randomId && j.randomId <= uint64_t(std::numeric_limits<int64_t>::max())
		&& j.markId >= 0 && j.markDate >= 0 && (!j.markDate || j.markId > 0)
		&& (!j.chat || (token && ParsePeerToken(*token, self) == j.chat))
		&& (j.readerSnapshot.isEmpty()
			? j.readerConsent.isEmpty() && j.markId == 0
				&& (j.step <= DesktopSyncChatCreateStep::PeerInfo || j.adopted)
			: j.chat && ValidSnapshot(j.readerSnapshot,*j.chat,self)
				&& Hex(j.readerConsent,64) && Hash(j.readerSnapshot)==j.readerConsent)
		&& (!j.created || (j.choice == DesktopSyncChatSetupChoice::Create
			&& j.chat && j.chat->kind == PeerTokenKind::Channel
			&& DesktopSyncChatCreatedChannel(j.updates, self) == j.chat))
		&& (j.created || j.updates.isEmpty())
		&& (j.step == DesktopSyncChatCreateStep::Create || j.chat.has_value())
		&& (j.adopted || j.choice != DesktopSyncChatSetupChoice::Self || j.chat == PeerToken{})
		&& (j.adopted || j.choice != DesktopSyncChatSetupChoice::Existing || (j.chat && *j.chat != PeerToken{}))
		&& (j.step != DesktopSyncChatCreateStep::Create
			|| (j.choice == DesktopSyncChatSetupChoice::Create && !j.created
				&& !j.chat && j.markId == 0))
		&& (j.step < DesktopSyncChatCreateStep::Pin || j.adopted || j.markDate > 0
			|| (!j.provisionSource.isEmpty() && j.step == DesktopSyncChatCreateStep::Done))
		&& ((j.step != DesktopSyncChatCreateStep::Pin && j.step != DesktopSyncChatCreateStep::Mute
			&& j.step != DesktopSyncChatCreateStep::Archive && j.step != DesktopSyncChatCreateStep::ApplyUpdates) || j.created)
		&& (j.choice != DesktopSyncChatSetupChoice::Create || j.step == DesktopSyncChatCreateStep::Create || j.created || j.adopted)
		&& (!j.decorationAfterCache || (j.created && j.step == DesktopSyncChatCreateStep::Commit
			&& (*j.decorationAfterCache == DesktopSyncChatCreateStep::Pin
				|| *j.decorationAfterCache == DesktopSyncChatCreateStep::Mute
				|| *j.decorationAfterCache == DesktopSyncChatCreateStep::Archive)))
		&& (!j.adopted || (j.step == DesktopSyncChatCreateStep::Done && !j.pending
			&& !j.created && !j.markId && j.readerSnapshot.isEmpty() && !j.newReaders));
}

}

QString DesktopSyncChatSetupAuthority(const DesktopSyncChatDiscoveryScope &scope) {
	if (!ValidDesktopSyncChatOrigin(scope.origin) || !Hex(scope.loginBinding, 64)
		|| scope.root.isEmpty() || scope.mode < AccountWorkMode::Own
		|| scope.mode > AccountWorkMode::Stock
		|| (scope.owner && !AccountKeyText(scope.owner->environment, scope.owner->userId))) {
		return {};
	}
	return Hash({{u"domain"_q, u"purple-sync-chat-setup-v1"_q},
		{u"account"_q, AccountKeyText(scope.origin.account.environment, scope.origin.account.userId).value_or(QString())},
		{u"login"_q, scope.loginBinding}, {u"root"_q, scope.root},
		{u"binding"_q, QString::fromLatin1(scope.binding.toHex())},
		{u"mode"_q, int(scope.mode)},
		{u"owner"_q, scope.owner ? AccountKeyText(scope.owner->environment, scope.owner->userId).value_or(QString()) : QString()}});
}

QJsonObject ReaderSnapshot(const DesktopSyncChatPeer &peer) {
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (!eligible || !*eligible || eligible->readers.populationUnknown) {
		return {};
	}
	const auto &facts = *peer.facts;
	auto members = QJsonArray();
	if (peer.readerMembers) {
		for (const auto &member : *peer.readerMembers) {
			members.push_back(member);
		}
	}
	return QJsonObject{{u"chat"_q, PeerTokenText(peer.chat, peer.origin.account.userId).value_or(QString())},
		{u"kind"_q, int(peer.kind)}, {u"members"_q, members},
		{u"has_members"_q, peer.readerMembers.has_value()},
		{u"creator"_q, facts.creator}, {u"sole_admin"_q, facts.soleAdmin},
		{u"others"_q, QString::number(eligible->readers.otherMembers.value_or(0))},
		{u"admins"_q, QString::number(eligible->readers.otherAdmins.value_or(0))},
		{u"private"_q, eligible->readers.privateRecipient},
		{u"bot"_q, eligible->readers.botOwner}};
}

QString DesktopSyncChatReaderConsent(const DesktopSyncChatPeer &peer) {
	const auto snapshot = ReaderSnapshot(peer);
	return snapshot.isEmpty() ? QString() : Hash(snapshot);
}

QString DesktopSyncChatMarkText() {
	return u"This account's Purple Telegram apps keep their sync data in this chat: settings and playlists. Deleting this message makes them stop using it.\n#purplechatsync v1"_q;
}

MTPchannels_CreateChannel DesktopSyncChatCreateRequest() {
	return MTPchannels_CreateChannel(MTP_flags(MTPchannels_CreateChannel::Flag::f_broadcast),
		MTP_string("Purple sync"),
		MTP_string("Purple Telegram keeps sync data for your devices here."),
		MTPInputGeoPoint(), MTP_string(), MTP_int(0));
}

std::optional<PeerToken> DesktopSyncChatCreatedChannel(
		const QByteArray &updates, uint64_t selfUserId) {
	const auto parsed = Updates(updates);
	if (!parsed) {
		return {};
	}
	const auto channelOf = [&](const auto &data) -> std::optional<PeerToken> {
		if (data.vchats().v.size() != 1 || data.vchats().v.front().type() != mtpc_channel) {
			return {};
		}
		const auto &channel = data.vchats().v.front().c_channel();
		const auto token = DesktopSyncChatToken(MTP_peerChannel(channel.vid()), selfUserId);
		return token && channel.is_creator() && channel.is_broadcast() && !channel.is_megagroup()
			&& !channel.is_min() && channel.vaccess_hash() && channel.vaccess_hash()->v != 0
			&& channel.vtitle().v == QByteArray("Purple sync")
			? token : std::nullopt;
	};
	return parsed->match([&](const MTPDupdates &data) { return channelOf(data); },
		[&](const MTPDupdatesCombined &data) { return channelOf(data); },
		[](const auto &) -> std::optional<PeerToken> { return {}; });
}

std::optional<QByteArray> SerializeDesktopSyncChatCreateJournal(
		const DesktopSyncChatCreateJournal &j) {
	if (!Valid(j, j.account.userId)) {
		return {};
	}
	auto object = QJsonObject{
		{u"version"_q, j.provisionSource.isEmpty() ? 1 : 2}, {u"environment"_q, j.account.environment}, {u"user_id"_q, QString::number(j.account.userId)}, {u"authority"_q, j.authority}, {u"operation"_q, j.operation},
		{u"choice"_q, int(j.choice)}, {u"step"_q, int(j.step)},
		{u"chat"_q, j.chat ? PeerTokenText(*j.chat).value_or(QString()) : QString()},
		{u"created"_q, j.created}, {u"pending"_q, j.pending},
		{u"cancelled"_q, j.cancelled}, {u"adopted"_q, j.adopted},
		{u"retry_at"_q, QString::number(j.retryAt)}, {u"attempts"_q, j.createAttempts}, {u"random_id"_q, QString::number(j.randomId)},
		{u"mark_id"_q, j.markId}, {u"mark_date"_q, QString::number(j.markDate)},
		{u"updates"_q, QString::fromLatin1(j.updates.toBase64())},
		{u"after_cache"_q, j.decorationAfterCache ? int(*j.decorationAfterCache) : -1},
		{u"reader_snapshot"_q,j.readerSnapshot}, {u"new_readers"_q,j.newReaders}, {u"reader_consent"_q, j.readerConsent}};
	if (!j.provisionSource.isEmpty()) {
		object.insert(u"purpose"_q, u"provision-move"_q);
		object.insert(u"source_fingerprint"_q, j.provisionSource);
	}
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<DesktopSyncChatCreateJournal> ParseDesktopSyncChatCreateJournal(
		const QByteArray &bytes, const QString &authority, const AccountKey &account) {
	const auto selfUserId = account.userId;
	if (bytes.size() > 512 * 1024 || !Hex(authority, 64)) {
		return {};
	}
	const auto doc = QJsonDocument::fromJson(bytes);
	const auto object = doc.object();
	const auto provision = object.value(u"version"_q) == 2;
	if (!doc.isObject() || object.size() != (provision ? 24 : 22)
		|| (!provision && object.value(u"version"_q) != 1)
		|| (provision && (object.value(u"purpose"_q) != u"provision-move"_q
			|| !object.value(u"source_fingerprint"_q).isString()
			|| !Hex(object.value(u"source_fingerprint"_q).toString(), 64)))
		|| object.value(u"authority"_q) != authority
		|| object.value(u"environment"_q) != account.environment
		|| object.value(u"user_id"_q) != QString::number(account.userId)) {
		return {};
	}
	for (const auto &name : {u"authority"_q, u"operation"_q, u"chat"_q,
		u"random_id"_q, u"mark_date"_q, u"retry_at"_q, u"updates"_q, u"reader_consent"_q}) {
		if (!object.value(name).isString()) return {};
	}
	for (const auto &name : {u"created"_q, u"pending"_q, u"cancelled"_q, u"adopted"_q, u"new_readers"_q}) {
		if (!object.value(name).isBool()) return {};
	}
	for (const auto &name : {u"version"_q, u"choice"_q, u"step"_q, u"attempts"_q, u"mark_id"_q, u"after_cache"_q}) {
		const auto value = object.value(name);
		if (!value.isDouble() || value.toDouble() != double(value.toInt(-1))) return {};
	}
	auto j = DesktopSyncChatCreateJournal();
	j.account = account;
	if (provision) j.provisionSource = object.value(u"source_fingerprint"_q).toString();
	j.authority = authority; j.operation = object.value(u"operation"_q).toString();
	j.choice = DesktopSyncChatSetupChoice(object.value(u"choice"_q).toInt());
	j.step = DesktopSyncChatCreateStep(object.value(u"step"_q).toInt());
	const auto token = object.value(u"chat"_q).toString();
	j.chat = token.isEmpty() ? std::nullopt : ParsePeerToken(token, selfUserId);
	if (!token.isEmpty() && !j.chat) return {};
	j.created = object.value(u"created"_q).toBool(); j.pending = object.value(u"pending"_q).toBool();
	j.cancelled = object.value(u"cancelled"_q).toBool(); j.adopted = object.value(u"adopted"_q).toBool();
	j.createAttempts = object.value(u"attempts"_q).toInt();
	j.randomId = object.value(u"random_id"_q).toString().toULongLong();
	j.markId = object.value(u"mark_id"_q).toInt();
	j.markDate = object.value(u"mark_date"_q).toString().toLongLong();
	j.retryAt = object.value(u"retry_at"_q).toString().toLongLong();
	j.updates = QByteArray::fromBase64(object.value(u"updates"_q).toString().toLatin1());
	j.readerConsent = object.value(u"reader_consent"_q).toString();
	if (!object.value(u"reader_snapshot"_q).isObject()) return {};
	j.readerSnapshot=object.value(u"reader_snapshot"_q).toObject();
	j.newReaders=object.value(u"new_readers"_q).toBool();
	const auto afterCache = object.value(u"after_cache"_q).toInt();
	if (afterCache != -1) j.decorationAfterCache = DesktopSyncChatCreateStep(afterCache);
	return Valid(j, selfUserId) && SerializeDesktopSyncChatCreateJournal(j) == bytes
		? std::optional(j) : std::nullopt;
}

std::optional<DesktopSyncChatCreateJournal> BeginDesktopSyncChatCreate(
		const DesktopSyncChatSetupSnapshot &snapshot,
		const DesktopSyncChatSetupSelection &selection) {
	const auto authority = DesktopSyncChatSetupAuthority(snapshot.scope);
	if (selection.choice < DesktopSyncChatSetupChoice::Create
		|| selection.choice > DesktopSyncChatSetupChoice::Existing
		|| authority.isEmpty() || AcceptDesktopSyncChatSetup(snapshot, snapshot, selection)
		!= DesktopSyncChatSetupRefusal::None) {
		return {};
	}
	auto result = DesktopSyncChatCreateJournal();
	result.account = snapshot.scope.origin.account;
	result.authority = authority;
	result.operation = QUuid::createUuid().toString(QUuid::Id128);
	result.choice = selection.choice;
	do { result.randomId = QRandomGenerator::system()->generate64()
		& uint64_t(std::numeric_limits<int64_t>::max()); } while (!result.randomId);
	if (selection.peer) {
		result.chat = selection.peer->chat;
		result.step = DesktopSyncChatCreateStep::PeerInfo;
		result.readerSnapshot=ReaderSnapshot(*selection.peer);
		result.readerConsent=DesktopSyncChatReaderConsent(*selection.peer);
	}
	return result;
}

std::optional<DesktopSyncChatCreateJournal> BeginDesktopSyncChatProvision(
		const DesktopSyncChatSetupSnapshot &snapshot,
		const DesktopSyncChatSetupSelection &selection) {
	const auto fingerprint = DesktopSyncChatMoveFingerprint(snapshot);
	const auto authority = DesktopSyncChatSetupAuthority(snapshot.scope);
	if (fingerprint.isEmpty() || authority.isEmpty() || !selection.provisionOnly
		|| !selection.readerConsent || selection.choice != DesktopSyncChatSetupChoice::Create
		|| selection.peer) { return {}; }
	auto result = DesktopSyncChatCreateJournal();
	result.account = snapshot.scope.origin.account;
	result.authority = authority;
	result.operation = QUuid::createUuid().toString(QUuid::Id128);
	result.provisionSource = fingerprint;
	do {
		result.randomId = QRandomGenerator::system()->generate64()
			& uint64_t(std::numeric_limits<int64_t>::max());
	} while (!result.randomId);
	return result;
}

DesktopSyncChatSetupExecutor::DesktopSyncChatSetupExecutor(
		DesktopSyncChatOrigin origin, DesktopSyncChatCreateJournal journal,
		Fn<bool(const DesktopSyncChatCreateJournal &)> save,
		Fn<bool()> foregroundCurrent)
: _origin(std::move(origin))
, _journal(std::move(journal))
, _save(std::move(save))
, _current(std::move(foregroundCurrent)) {
}

bool DesktopSyncChatSetupExecutor::Save(DesktopSyncChatCreateJournal next) {
	if (!_current || !_current() || !_save || !Valid(next, _origin.account.userId)
		|| !_save(next) || !_current()) {
		Hold(u"journal-write-held"_q);
		return false;
	}
	_journal = std::move(next);
	return true;
}

void DesktopSyncChatSetupExecutor::Hold(QString reason) {
	_issued.reset();
	++_ticket;
	_offer.reset();
	_phase = DesktopSyncChatCreatePhase::Held;
	_reason = std::move(reason);
}

void DesktopSyncChatSetupExecutor::Start(int64_t now) {
	if (_phase == DesktopSyncChatCreatePhase::Running) return;
	if (!_current || !_current() || !ValidDesktopSyncChatOrigin(_origin)
		|| _journal.account != _origin.account || !Valid(_journal, _origin.account.userId) || now <= 0 || now > std::numeric_limits<int64_t>::max() - 86400) {
		Hold(u"foreground-origin-held"_q); return;
	}
	if (now < _journal.retryAt) { Hold(u"retry-backoff"_q); return; }
	if (_journal.step == DesktopSyncChatCreateStep::Done) {
		_phase = DesktopSyncChatCreatePhase::Complete; return;
	}
	auto next = _journal;
	next.cancelled = false;
	if (!Save(std::move(next))) return;
	_issued.reset(); ++_ticket; _peer.reset(); _resolvedMark.reset(); _offer.reset(); _inventoryReady=false;
	_next = DesktopSyncChatCreateStep::Resolve;
	_phase = DesktopSyncChatCreatePhase::Running;
	_reason.clear();
}

void DesktopSyncChatSetupExecutor::Cancel() {
	_offer.reset();
	_issued.reset(); ++_ticket;
	auto next = _journal; next.cancelled = true;
	const auto saved = Save(std::move(next));
	_phase = DesktopSyncChatCreatePhase::Cancelled;
	_reason = saved ? u"cancelled"_q : u"cancelled-journal-held"_q;
}

void DesktopSyncChatSetupExecutor::UseSelf(int64_t now) {
	if (!_journal.provisionSource.isEmpty()
		|| _phase == DesktopSyncChatCreatePhase::Running || _journal.chat
		|| _journal.created || _journal.markId || _journal.step != DesktopSyncChatCreateStep::Create) {
		Hold(u"self-fallback-held"_q); return;
	}
	if (_journal.pending) { Hold(u"ambiguous-create-held"_q); return; }
	auto next = _journal;
	next.choice = DesktopSyncChatSetupChoice::Self;
	next.chat = PeerToken{};
	next.step = DesktopSyncChatCreateStep::PeerInfo;
	if (Save(std::move(next))) Start(now);
}

std::optional<DesktopSyncChatCreateCommand> DesktopSyncChatSetupExecutor::Next() {
	if (_phase != DesktopSyncChatCreatePhase::Running || _issued) return {};
	if (!_current || !_current()) { Hold(u"foreground-origin-held"_q); return {}; }
	auto step = _next;
	if (step == DesktopSyncChatCreateStep::Create && _journal.pending) {
		step = DesktopSyncChatCreateStep::ReconcileCreate;
	} else if (step == DesktopSyncChatCreateStep::PostMark && _journal.pending) {
		step = DesktopSyncChatCreateStep::ReconcileMark;
	}
	if (step == DesktopSyncChatCreateStep::Create && _journal.createAttempts >= 3) {
		Hold(u"create-retry-budget"_q); return {};
	}
	if (step == _journal.step && (step == DesktopSyncChatCreateStep::Create
		|| step == DesktopSyncChatCreateStep::ApplyUpdates || step == DesktopSyncChatCreateStep::PostMark
		|| step == DesktopSyncChatCreateStep::Pin || step == DesktopSyncChatCreateStep::Mute
		|| step == DesktopSyncChatCreateStep::Archive || step == DesktopSyncChatCreateStep::Commit)) {
		auto next = _journal;
		next.pending = true;
		if (step == DesktopSyncChatCreateStep::Create) ++next.createAttempts;
		if (!Save(std::move(next))) return {};
	}
	_issued = DesktopSyncChatCreateCommand{_origin, _journal.operation, ++_ticket, step, _journal, true};
	return _issued;
}

bool DesktopSyncChatSetupExecutor::Renew(
		const DesktopSyncChatCreateCommand &command,
		const DesktopSyncChatSetupResumeProof &proof, int64_t now,
		Fn<bool(const DesktopSyncChatCreateJournal &)> commitCache) {
	const auto bytes = _issued ? SerializeDesktopSyncChatCreateJournal(_issued->journal) : std::nullopt;
	if (!_issued || _phase != DesktopSyncChatCreatePhase::Running
		|| command.origin != _origin || command.operation != _journal.operation
		|| command.ticket != _issued->ticket || command.step != _issued->step
		|| !bytes || SerializeDesktopSyncChatCreateJournal(command.journal) != bytes
		|| !_current || !_current()) {
		return false;
	}
	auto candidate = _journal;
	if (command.step == DesktopSyncChatCreateStep::Create
		|| command.step == DesktopSyncChatCreateStep::PostMark) {
		candidate.pending = false;
	}
	if (command.step == DesktopSyncChatCreateStep::Create) {
		candidate.createAttempts = std::max(0, candidate.createAttempts - 1);
	}
	auto verifier = DesktopSyncChatSetupExecutor(_origin, candidate,
		[](const DesktopSyncChatCreateJournal &) { return true; }, _current);
	verifier.Start(now);
	auto peerIndex = size_t(0);
	for (auto count = 0; count != 12; ++count) {
		const auto next = verifier.Next();
		if (!next) { return false; }
		if (next->step == command.step) {
			if (verifier.journal().chat != _journal.chat
				|| verifier.journal().randomId != _journal.randomId
				|| verifier.journal().markId != _journal.markId
				|| verifier.journal().markDate != _journal.markDate) { return false; }
			auto renewed = _journal;
			renewed.readerConsent = verifier.journal().readerConsent;
			renewed.readerSnapshot = verifier.journal().readerSnapshot;
			renewed.newReaders = verifier.journal().newReaders;
			if (!Save(std::move(renewed))) { return false; }
			_peer = verifier._peer;
			_resolvedMark = verifier._resolvedMark;
			return _current && _current();
		}
		auto reply = DesktopSyncChatCreateReply{
			_origin, next->operation, next->ticket, DesktopSyncChatCreateOutcome::Okay };
		switch (next->step) {
		case DesktopSyncChatCreateStep::Resolve: reply.resolution = proof.resolution; break;
		case DesktopSyncChatCreateStep::PeerInfo:
			if (peerIndex == proof.peers.size()) { return false; }
			reply.peer = proof.peers[peerIndex++];
			break;
		case DesktopSyncChatCreateStep::Inventory: reply.inventory = proof.inventory; break;
		case DesktopSyncChatCreateStep::ReadMark: reply.readback = proof.readback; break;
		case DesktopSyncChatCreateStep::Commit:
			if (!commitCache || !commitCache(verifier.journal())) { return false; }
			break;
		default: return false;
		}
		if (!verifier.Complete(reply, now)) { return false; }
	}
	return false;
}

bool DesktopSyncChatSetupExecutor::AcceptMark(const MTPMessage &message) {
	if (!_peer || message.type() != mtpc_message || !_journal.chat) return false;
	const auto &data = message.c_message();
	if (data.vid().v <= 0 || (_journal.markId && data.vid().v != _journal.markId)
		|| data.vdate().v <= 0 || (_journal.markDate && data.vdate().v != _journal.markDate)
		|| DesktopSyncChatToken(data.vpeer_id(), _origin.account.userId) != _journal.chat
		|| qs(data.vmessage()) != DesktopSyncChatMarkText()
		|| (data.vmedia() && data.vmedia()->type() != mtpc_messageMediaEmpty)
		|| !IsOwnSyncChatMessage(MapDesktopSyncChatMessage(_origin, *_peer, message))) return false;
	if (_resolvedMark && _resolvedMark->markId > 0
		&& (_resolvedMark->chat != *_journal.chat
			|| _resolvedMark->markId != data.vid().v || _resolvedMark->markDate != data.vdate().v)) {
		Hold(u"mark-superseded"_q); return false;
	}
	auto next = _journal;
	next.markId = data.vid().v; next.markDate = data.vdate().v; next.pending = false;
	if (next.newReaders && (next.step == DesktopSyncChatCreateStep::Pin
		|| next.step == DesktopSyncChatCreateStep::Mute
		|| next.step == DesktopSyncChatCreateStep::Archive)) {
		next.decorationAfterCache = next.step;
		next.step = DesktopSyncChatCreateStep::Commit;
	} else if (next.step == DesktopSyncChatCreateStep::PostMark
		|| next.step == DesktopSyncChatCreateStep::ReadMark) {
		next.step = DesktopSyncChatCreateStep::Commit;
	}
	if (!Save(std::move(next))) return false;
	_next = _journal.step;
	return true;
}

bool DesktopSyncChatSetupExecutor::AcceptReadback(
		const MTPmessages_Messages &response, int32_t id) {
	if (!_journal.chat || id <= 0 || !RecognizedDesktopSyncChatRead(response, *_journal.chat)) {
		return false;
	}
	const auto messages = response.match([](const MTPDmessages_messagesNotModified &) {
		return static_cast<const QVector<MTPMessage>*>(nullptr);
	}, [](const auto &data) { return &data.vmessages().v; });
	const auto count = response.match([](const MTPDmessages_messagesSlice &data) {
		return data.vcount().v >= data.vmessages().v.size();
	}, [](const MTPDmessages_channelMessages &data) {
		return data.vcount().v >= data.vmessages().v.size();
	}, [](const auto &) { return true; });
	return count && messages && messages->size() == 1
		&& messages->front().type() == mtpc_message
		&& messages->front().c_message().vid().v == id && AcceptMark(messages->front());
}

void DesktopSyncChatSetupExecutor::ContinueAfterPeer() {
	if (!_journal.provisionSource.isEmpty() && _inventoryReady) {
		auto next = _journal;
		next.pending = false;
		next.step = DesktopSyncChatCreateStep::Done;
		if (Save(std::move(next))) { _phase = DesktopSyncChatCreatePhase::Complete; }
		return;
	}
	_next = _journal.markId ? DesktopSyncChatCreateStep::ReadMark : _journal.step;
	if (_next == DesktopSyncChatCreateStep::PostMark && !_journal.pending && !_inventoryReady) {
		_next = DesktopSyncChatCreateStep::Inventory;
	}
	if (_next == DesktopSyncChatCreateStep::PeerInfo) {
		auto next = _journal;
		next.pending = false; next.step = DesktopSyncChatCreateStep::Inventory;
		if (!Save(std::move(next))) return;
		_next = _journal.step;
	}
}

bool DesktopSyncChatSetupExecutor::Complete(const DesktopSyncChatCreateReply &reply, int64_t now) {
	if (!_issued || reply.origin != _origin || reply.operation != _journal.operation
		|| reply.ticket != _issued->ticket || _phase != DesktopSyncChatCreatePhase::Running) return false;
	const auto step = _issued->step;
	_issued.reset();
	if (now <= 0 || now > std::numeric_limits<int64_t>::max() - 86400) {
		Hold(u"clock-held"_q); return true;
	}
	if (!_current || !_current()) { Hold(u"foreground-origin-held"_q); return true; }
	if (reply.outcome != DesktopSyncChatCreateOutcome::Okay) {
		if (((step == DesktopSyncChatCreateStep::Create || step == DesktopSyncChatCreateStep::PostMark)
			&& reply.outcome != DesktopSyncChatCreateOutcome::Ambiguous)
			|| reply.outcome == DesktopSyncChatCreateOutcome::FloodWait) {
			auto next = _journal;
			if ((step == DesktopSyncChatCreateStep::Create || step == DesktopSyncChatCreateStep::PostMark)
				&& reply.outcome != DesktopSyncChatCreateOutcome::Ambiguous) next.pending = false;
			if (reply.outcome == DesktopSyncChatCreateOutcome::FloodWait) {
				const auto delay = std::max(int64_t(1), reply.retryAfter);
				if (delay > std::numeric_limits<int64_t>::max() - now) {
					Hold(u"clock-held"_q); return true;
				}
				next.retryAt = now + delay;
			}
			if (!Save(std::move(next))) return true;
		}
		Hold(reply.outcome == DesktopSyncChatCreateOutcome::Limit ? u"channel-limit"_q
			: reply.outcome == DesktopSyncChatCreateOutcome::Restricted ? u"user-restricted"_q
			: reply.outcome == DesktopSyncChatCreateOutcome::FloodWait ? u"flood-wait"_q
			: u"outcome-unknown"_q);
		if (reply.outcome == DesktopSyncChatCreateOutcome::Limit) _phase = DesktopSyncChatCreatePhase::Limit;
		return true;
	}
	if (step == DesktopSyncChatCreateStep::Resolve) {
		if (!reply.resolution || reply.resolution->scope.origin != _origin
			|| DesktopSyncChatSetupAuthority(reply.resolution->scope) != _journal.authority) {
			Hold(u"resolution-authority-held"_q); return true;
		}
		const auto &resolved = *reply.resolution;
		if (!_journal.provisionSource.isEmpty()) {
			if (DesktopSyncChatMoveFingerprint(resolved) != _journal.provisionSource) {
				Hold(u"move-source-changed"_q);
				return true;
			}
			_next = _journal.step == DesktopSyncChatCreateStep::Create
				? DesktopSyncChatCreateStep::Create
				: _journal.step == DesktopSyncChatCreateStep::ApplyUpdates
				? DesktopSyncChatCreateStep::ApplyUpdates : DesktopSyncChatCreateStep::PeerInfo;
			return true;
		}
		if (resolved.scope.settingsBound || resolved.input.settingsBound || resolved.scope.carry
			|| !resolved.scope.space.isEmpty()) { Hold(u"bound-stream-held"_q); return true; }
		if (resolved.status.complete && resolved.status.resolution.action == SyncChatAction::Use
			&& resolved.status.resolution.cache
			&& resolved.status.resolution.cache->chat == resolved.status.resolution.readChat
			&& resolved.status.phase == DesktopSyncChatPhase::Use
			&& resolved.status.resolution.readChat) {
			_resolvedMark = resolved.status.resolution.cache;
			const auto pendingMark = _journal.chat && _journal.pending
				&& _journal.step == DesktopSyncChatCreateStep::PostMark && !_journal.markId
				&& _journal.chat == resolved.status.resolution.readChat;
			const auto ownMark = _journal.chat && _journal.markId > 0
				&& resolved.status.resolution.cache
				&& resolved.status.resolution.cache->chat == *_journal.chat
				&& resolved.status.resolution.cache->markId == _journal.markId
				&& (!_journal.markDate || resolved.status.resolution.cache->markDate == _journal.markDate);
			if (ownMark || pendingMark) { _next = DesktopSyncChatCreateStep::PeerInfo; return true; }
			if (!_journal.markId && !_journal.created && !resolved.status.postingHeld
				&& !resolved.status.resolution.postingHeld
				&& !resolved.status.resolution.readers.populationUnknown) {
				auto next = _journal;
				next.adopted = true; next.pending = false;
				next.readerConsent.clear(); next.readerSnapshot = {}; next.newReaders = false;
				next.chat = resolved.status.resolution.readChat;
				next.step = DesktopSyncChatCreateStep::Done;
				if (Save(std::move(next))) _phase = DesktopSyncChatCreatePhase::Complete;
				return true;
			}
			Hold(u"existing-stream-held"_q); return true;
		}
		if (!DesktopSyncChatFreshSetup(resolved)) { Hold(u"fresh-stream-required"_q); return true; }
		_next = _journal.step == DesktopSyncChatCreateStep::Create
			? DesktopSyncChatCreateStep::Create : _journal.step == DesktopSyncChatCreateStep::ApplyUpdates
			? DesktopSyncChatCreateStep::ApplyUpdates : DesktopSyncChatCreateStep::PeerInfo;
		return true;
	}
	if (step == DesktopSyncChatCreateStep::Create || step == DesktopSyncChatCreateStep::ReconcileCreate) {
		const auto chat = DesktopSyncChatCreatedChannel(reply.updates, _origin.account.userId);
		if (reply.creationProof != DesktopSyncChatCreationProof::MatchedRequest || !chat) {
			Hold(u"ambiguous-create-held"_q); return true;
		}
		auto next = _journal;
		next.chat = chat; next.created = true; next.pending = false;
		next.updates = reply.updates; next.step = DesktopSyncChatCreateStep::ApplyUpdates;
		if (Save(std::move(next))) _next = _journal.step;
		return true;
	}
	if (step == DesktopSyncChatCreateStep::PeerInfo) {
		if (!reply.peer || reply.peer->origin != _origin || reply.peer->chat != _journal.chat) {
			Hold(u"peer-info-held"_q); return true;
		}
		const auto eligible = DesktopSyncChatEligibility(*reply.peer);
		const auto consent = DesktopSyncChatReaderConsent(*reply.peer);
		if (!eligible || !*eligible || consent.isEmpty()) {
			Hold(u"eligibility-or-readers-held"_q); return true;
		}
		if (eligible->readers.warningRequired && consent != _journal.readerConsent) {
			const auto alreadyIssued=_journal.markId>0
				|| (_journal.pending && _journal.step==DesktopSyncChatCreateStep::PostMark);
			if (!alreadyIssued || _journal.readerSnapshot.isEmpty()) {
				Hold(u"reader-consent-required"_q);
				if (!_journal.pending && !_journal.markId) {
					_offer=DesktopSyncChatConsentOffer{_origin,_journal.operation,_ticket,*reply.peer};
				}
				return true;
			}
			const auto baseline=ReaderBaseline(_journal);
			if (baseline.readers.privateRecipient!=eligible->readers.privateRecipient
				|| baseline.readers.botOwner!=eligible->readers.botOwner) {
				Hold(u"recipient-kind-changed"_q); return true;
			}
			auto next=_journal;
			next.newReaders=next.newReaders || NewDesktopSyncChatReaders(baseline,*reply.peer);
			if (!Save(std::move(next))) return true;
		} else if (_journal.readerSnapshot.isEmpty()) {
			auto next=_journal;
			next.readerSnapshot=ReaderSnapshot(*reply.peer); next.readerConsent=consent;
			if (!Save(std::move(next))) return true;
		}
		_peer = reply.peer;
		ContinueAfterPeer();
		return true;
	}
	if (step == DesktopSyncChatCreateStep::Inventory) {
		if (!reply.inventory || !_peer || !_journal.chat
			|| !SameDesktopSyncChatDisclosure(*_peer, reply.inventory->route.peer)) {
			Hold(u"target-inventory-held"_q); return true;
		}
		const auto mapped = MapDesktopSyncChatInventoryReply(_origin, reply.ticket,
			*_journal.chat, *reply.inventory);
		if (!mapped.success || mapped.chatReadable != true
			|| mapped.records != SyncChatRecordPresence::Empty) {
			Hold(u"target-inventory-not-empty-or-incomplete"_q); return true;
		}
		auto next = _journal;
		next.pending = false; next.step = _journal.provisionSource.isEmpty()
			? DesktopSyncChatCreateStep::PostMark : DesktopSyncChatCreateStep::Inventory;
		if (Save(std::move(next))) {
			_inventoryReady=true;
			_next=DesktopSyncChatCreateStep::PeerInfo;
		}
		return true;
	}
	if (step == DesktopSyncChatCreateStep::PostMark) {
		if (reply.randomId != _journal.randomId || reply.markId <= 0) {
			Hold(u"mark-receipt-held"_q); return true;
		}
		auto next = _journal;
		next.markId = reply.markId; next.pending = false; next.step = DesktopSyncChatCreateStep::ReadMark;
		if (Save(std::move(next))) _next = _journal.step;
		return true;
	}
	if (step == DesktopSyncChatCreateStep::ReadMark || step == DesktopSyncChatCreateStep::ReconcileMark) {
		if (!reply.readback || (step == DesktopSyncChatCreateStep::ReconcileMark
			&& reply.randomId != _journal.randomId) || !AcceptReadback(*reply.readback,
				step == DesktopSyncChatCreateStep::ReconcileMark ? reply.markId : _journal.markId)) {
			if (_phase != DesktopSyncChatCreatePhase::Held) Hold(u"mark-readback-held"_q);
		}
		return true;
	}
	auto next = _journal;
	next.pending = false;
	if (step == DesktopSyncChatCreateStep::ApplyUpdates) next.step = DesktopSyncChatCreateStep::PeerInfo;
	else if (step == DesktopSyncChatCreateStep::Pin && next.created) next.step = DesktopSyncChatCreateStep::Mute;
	else if (step == DesktopSyncChatCreateStep::Mute && next.created) next.step = DesktopSyncChatCreateStep::Archive;
	else if (step == DesktopSyncChatCreateStep::Archive && next.created) next.step = DesktopSyncChatCreateStep::Done;
	else if (step == DesktopSyncChatCreateStep::Commit) {
		next.step = next.decorationAfterCache.value_or(next.created
			? DesktopSyncChatCreateStep::Pin : DesktopSyncChatCreateStep::Done);
		next.decorationAfterCache.reset();
	}
	else { Hold(u"unexpected-step"_q); return true; }
	if (Save(std::move(next))) {
		_next = _journal.step;
		if (_next == DesktopSyncChatCreateStep::Done) _phase = DesktopSyncChatCreatePhase::Complete;
	}
	return true;
}

std::optional<DesktopSyncChatCacheRecord> DesktopSyncChatSetupCache(
		const DesktopSyncChatCreateJournal &j,
		const DesktopSyncChatDiscoveryScope &scope, QString title) {
	if (!j.provisionSource.isEmpty() || !Valid(j,j.account.userId) || !j.chat || j.markId<=0 || j.markDate<=0
		|| scope.origin.account!=j.account || DesktopSyncChatSetupAuthority(scope)!=j.authority
		|| j.readerSnapshot.isEmpty()) return {};
	auto result=ReaderBaseline(j);
	result.mark={*j.chat,j.markId,j.markDate}; result.title=std::move(title);
	result.loginBinding=scope.loginBinding;
	return SerializeDesktopSyncChatCache(result) ? std::optional(result) : std::nullopt;
}

std::optional<DesktopSyncChatConsentOffer> DesktopSyncChatSetupExecutor::offeredReaders() const {
	return _offer;
}

bool DesktopSyncChatSetupExecutor::Reconsent(const DesktopSyncChatConsentOffer &offer, int64_t now) {
	if (_phase!=DesktopSyncChatCreatePhase::Held || !_offer || !_current || !_current()
		|| now <= 0 || now > std::numeric_limits<int64_t>::max() - 86400
		|| now < _journal.retryAt
		|| offer.origin!=_origin || offer.operation!=_journal.operation || offer.ticket!=_ticket
		|| !_journal.chat || offer.peer.chat!=*_journal.chat || _journal.markId || _journal.pending
		|| !SameDesktopSyncChatDisclosure(offer.peer,_offer->peer)) return false;
	auto next=_journal;
	next.readerSnapshot=ReaderSnapshot(offer.peer); next.readerConsent=DesktopSyncChatReaderConsent(offer.peer);
	if (!Save(std::move(next))) return false;
	Start(now);
	return _phase==DesktopSyncChatCreatePhase::Running;
}

const DesktopSyncChatCreateJournal &DesktopSyncChatSetupExecutor::journal() const { return _journal; }
DesktopSyncChatCreatePhase DesktopSyncChatSetupExecutor::phase() const { return _phase; }
QString DesktopSyncChatSetupExecutor::reason() const { return _reason; }

}
