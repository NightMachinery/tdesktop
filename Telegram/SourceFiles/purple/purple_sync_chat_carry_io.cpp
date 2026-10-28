#include "purple/purple_sync_chat_carry_io.h"

#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <limits>
#include <set>

namespace Purple {
namespace {

std::optional<uint64_t> Id(const QJsonValue &value) {
	if (!value.isString()) { return {}; }
	const auto text = value.toString();
	auto ok = false;
	const auto number = text.toULongLong(&ok);
	return ok && number && QString::number(number) == text ? std::optional<uint64_t>(number) : std::nullopt;
}

QJsonArray Members(const DesktopSyncChatPeer &peer) {
	auto result = QJsonArray();
	if (peer.readerMembers) {
		auto members = *peer.readerMembers;
		std::sort(members.begin(), members.end());
		members.erase(std::unique(members.begin(), members.end()), members.end());
		for (const auto &member : members) { result.push_back(member); }
	}
	return result;
}

std::optional<QString> TransportHash(const QJsonObject &transport,
		const QJsonObject &envelope, const SyncCarryJournal &journal) {
	const auto source = PeerTokenText(journal.source, journal.account.userId);
	const auto target = PeerTokenText(journal.target, journal.account.userId);
	if (!source || !target) { return {}; }
	const auto scoped = QJsonObject{
		{u"domain"_q,u"purple-sync-carry-transport-v1"_q},
		{u"operation"_q,journal.operationId}, {u"mode"_q,int(journal.mode)},
		{u"source"_q,*source}, {u"target"_q,*target},
		{u"account"_q,envelope.value(u"account"_q)},
		{u"binding_sha256"_q,envelope.value(u"binding_sha256"_q)},
		{u"login_authority"_q,envelope.value(u"login_authority"_q)},
		{u"transport"_q,transport} };
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(scoped).toJson());
	return encoded ? std::optional(SyncCarryRecordHash(encoded.canonical)) : std::nullopt;
}

bool ReceiptCapacity(const QJsonObject &envelope, QJsonObject transport) {
	auto entries = transport.value(u"entries"_q).toObject();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		auto entry = i.value().toObject();
		entry.insert(u"ack_id"_q,std::numeric_limits<int32_t>::max());
		i.value() = entry;
	}
	transport.insert(u"entries"_q,entries);
	auto future = envelope;
	future.insert(u"transport"_q,transport);
	future.insert(u"transport_sha256"_q,QString(64,u'a'));
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(future).toJson());
	return encoded && encoded.canonical.size() <= kSyncCarryJournalStorageLimit;
}

QString Kind(DesktopSyncCarryEffect effect) {
	return effect == DesktopSyncCarryEffect::Forward ? u"forward"_q
		: effect == DesktopSyncCarryEffect::OwnRecord ? u"own"_q : u"mark"_q;
}

bool ExactForward(const SyncCarryJournal &journal, const DesktopSyncCarryFresh &fresh,
		const QString &hash, int32_t id) {
	if (!journal.plan || !fresh.sourceRecord || !ValidDesktopSyncChatIo(fresh.source)
		|| journal.plan->access != SyncCarrySourceAccess::Readable) { return false; }
	const auto &receipt = *fresh.sourceRecord;
	if (receipt.route.peer.origin != fresh.source.peer.origin || receipt.route.peer.chat != journal.source
		|| receipt.record.id != id || SyncCarryRecordHash(receipt.record.bytes) != hash
		|| !SealDesktopSyncChatRecord(fresh.source, receipt.response, receipt.record,
			receipt.record.documentId, receipt.record.editDate)) { return false; }
	const auto native = DesktopSyncChatRecordMessage(fresh.source, receipt.response, id);
	return native && native->type() == mtpc_message
		&& std::any_of(journal.plan->selected.begin(), journal.plan->selected.end(), [&](const auto &value) {
			return value.reference.account == journal.account && value.reference.chat == journal.source
				&& value.reference.messageId == id && value.reference.recordHash == hash
				&& value.date == native->c_message().vdate().v; });
}

bool Entry(const QJsonObject &entry) {
	const auto kind = entry.value(u"kind"_q).toString();
	const auto state = entry.value(u"state"_q).toString();
	return (kind == u"forward"_q || kind == u"own"_q || kind == u"mark"_q)
		&& (state == u"prepared"_q || state == u"issued"_q || state == u"unknown"_q || state == u"verified"_q)
		&& IsSyncCarryHash(entry.value(u"hash"_q).toString())
		&& Id(entry.value(u"random_id"_q))
		&& (kind == u"own"_q ? bool(Id(entry.value(u"file_id"_q))) : entry.value(u"file_id"_q).toString() == u"0"_q)
		&& entry.value(u"source_id"_q).isDouble()
		&& entry.value(u"source_id"_q).toDouble() >= 0
		&& entry.value(u"source_id"_q).toDouble() <= std::numeric_limits<int32_t>::max()
		&& entry.value(u"source_id"_q).toDouble() == entry.value(u"source_id"_q).toInt()
		&& (!entry.contains(u"ack_id"_q) || (entry.value(u"ack_id"_q).isDouble()
			&& entry.value(u"ack_id"_q).toDouble() > 0
			&& entry.value(u"ack_id"_q).toDouble() <= std::numeric_limits<int32_t>::max()
			&& entry.value(u"ack_id"_q).toDouble() == entry.value(u"ack_id"_q).toInt()))
		&& ((kind == u"forward"_q) == (entry.value(u"source_id"_q).toInt() > 0));
}

}

bool DesktopSyncCarryPermit::current() const {
	if (!_epoch || !_active || !_active->load() || _epoch->load() != _openingEpoch) { return false; }
	const auto allowed = _current && _current();
	return allowed && _active->load() && _epoch->load() == _openingEpoch;
}
void DesktopSyncCarryPermit::revoke() const { if (_active) { _active->store(false); } }
const DesktopSyncChatIo &DesktopSyncCarryPermit::source() const { return _source; }
const DesktopSyncChatIo &DesktopSyncCarryPermit::target() const { return _target; }
uint64_t DesktopSyncCarryPermit::randomId() const { return _identity.randomId; }
uint64_t DesktopSyncCarryPermit::fileId() const { return _identity.fileId; }
uint64_t DesktopSyncCarryPermit::renewal() const { return _renewal; }
bool DesktopSyncCarryPermit::readerWarning() const { return _readerWarning; }
bool DesktopSyncCarryPermit::uploadMatches(const QByteArray &bytes) const {
	return current() && _effect == DesktopSyncCarryEffect::OwnRecord
		&& _identity.fileId && SyncCarryRecordHash(bytes) == _hash;
}
QByteArray DesktopSyncCarryPermit::uploadContent() const { return _effect == DesktopSyncCarryEffect::OwnRecord ? _content : QByteArray(); }
bool DesktopSyncCarryPermit::compatible(const DesktopSyncCarryPermit &fresh) const {
	return current() && fresh.current() && _epoch == fresh._epoch && _active == fresh._active
		&& _openingEpoch == fresh._openingEpoch && fresh._renewal > _renewal && _operation == fresh._operation && _revision == fresh._revision
		&& _effect == fresh._effect && _identity.slot == fresh._identity.slot
		&& _identity.randomId == fresh._identity.randomId && _identity.fileId == fresh._identity.fileId
		&& _hash == fresh._hash && _sourceId == fresh._sourceId && _mark == fresh._mark
		&& _source.peer.origin == fresh._source.peer.origin && _source.peer.chat == fresh._source.peer.chat
		&& _target.peer.origin == fresh._target.peer.origin && _target.peer.chat == fresh._target.peer.chat;
}

std::optional<SyncCarryInventory> DesktopSyncChatCarryInventory(const DesktopSyncChatInventory &input) {
	const auto &inventory = input.inventory;
	if (!DesktopSyncChatInventoryMatches(input, input.route.peer.origin, input.route.peer.chat)
		|| inventory.status == SyncAccountInventoryStatus::Incomplete || !inventory.scan.complete()
		|| !inventory.read || inventory.read->status == SyncCandidateReadStatus::Incomplete
		|| inventory.read->records.size() != input.receipts.size()
		|| inventory.scan.candidateIds.size() != input.receipts.size()) { return {}; }
	auto result = SyncCarryInventory{ input.route.peer.origin.account, input.route.peer.chat, true, {} };
	auto seen = std::set<int32_t>();
	for (const auto &receipt : input.receipts) {
		const auto &record = receipt.record;
		if (receipt.route.peer.origin != input.route.peer.origin || receipt.route.peer.chat != input.route.peer.chat
			|| !seen.emplace(record.id).second
			|| std::count(inventory.scan.candidateIds.begin(), inventory.scan.candidateIds.end(), record.id) != 1) { return {}; }
		const auto found = std::find_if(inventory.read->records.begin(), inventory.read->records.end(), [&](const auto &value) { return value.id == record.id; });
		if (found == inventory.read->records.end() || found->status != record.status || found->bytes != record.bytes
			|| found->documentId != record.documentId || found->editDate != record.editDate) { return {}; }
		const auto sealed = SealDesktopSyncChatRecord(receipt.route, receipt.response, record, record.documentId, record.editDate);
		const auto native = DesktopSyncChatRecordMessage(receipt.route, receipt.response, record.id);
		if (!sealed || !native || native->type() != mtpc_message) { return {}; }
		const auto message = MakeSyncCarryMessage(result.account, result.chat, record.id,
			native->c_message().vdate().v, record.bytes, GetSyncChatMessageOwnership(
				MapDesktopSyncChatMessage(input.route.peer.origin, input.route.peer, *native)));
		if (!message) { return {}; }
		result.messages.push_back(*message);
	}
	return SelectSyncCarryHeads(result).status == SyncCarryStatus::Ready ? std::optional(result) : std::nullopt;
}

std::optional<SyncCarryMarkReadback> DesktopSyncChatCarryMarkReadback(
		const DesktopSyncChatIo &route, int32_t id,
		const MTPmessages_Messages &response, const QString &text) {
	const auto selected = DesktopSyncChatRecordMessage(route, response, id);
	if (!selected || selected->type() != mtpc_message || text != DesktopSyncChatMarkText()) { return {}; }
	const auto &message = selected->c_message();
	const auto ownership = GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(route.peer.origin, route.peer, *selected));
	if (message.vmedia() || qs(message.vmessage()) != text || ownership != SyncChatMessageOwnership::Own) { return {}; }
	return SyncCarryMarkReadback{ SyncCarryMarkOutcome::Valid, route.peer.origin.account,
		route.peer.chat, id, message.vdate().v, text, false, ownership };
}

std::optional<int32_t> DesktopSyncCarryUpdatesReceipt(
		const DesktopSyncCarryPermit &permit, const MTPUpdates &updates,
		Fn<void(const MTPUpdates &, uint64_t)> applyUpdates) {
	const auto captured = permit;
	if (!applyUpdates) { return {}; }
	applyUpdates(updates, captured.randomId());
	return captured.current() ? DesktopSyncChatUploadReceipt(captured.target(), updates, captured.randomId()) : std::nullopt;
}

std::optional<MTPmessages_ForwardMessages> DesktopSyncCarryForwardRequest(const DesktopSyncCarryPermit &permit) {
	return permit.current() && permit._effect == DesktopSyncCarryEffect::Forward
		? DesktopSyncChatForwardRequest(permit._source, permit._target, permit._sourceId, permit.randomId()) : std::nullopt;
}

std::optional<MTPmessages_SendMessage> DesktopSyncCarryMarkRequest(const DesktopSyncCarryPermit &permit) {
	if (!permit.current() || permit._effect != DesktopSyncCarryEffect::Mark || !ValidDesktopSyncChatIo(permit._target)) { return {}; }
	return MTPmessages_SendMessage(MTP_flags(MTPmessages_SendMessage::Flag::f_silent | MTPmessages_SendMessage::Flag::f_no_webpage),
		permit._target.input, MTPInputReplyTo(), MTP_string(permit._mark), MTP_long(permit.randomId()), MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(), MTP_int(0), MTP_int(0), MTPInputPeer(), MTPInputQuickReplyShortcut(),
		MTP_long(0), MTP_long(0), MTPSuggestedPost(), MTPInputRichMessage());
}

DesktopSyncChatCarryIo::DesktopSyncChatCarryIo(SyncChatCarryStore &store) : _store(store) {}
DesktopSyncChatCarryIo::~DesktopSyncChatCarryIo() {
	for (const auto &[slot, value] : _active) { if (const auto active = value.second.lock()) { active->store(false); } }
}
bool DesktopSyncChatCarryIo::cleanupEnabled() const { return false; }

bool DesktopSyncChatCarryIo::Context(const DesktopSyncCarryFresh &fresh) const {
	if (!_store.Current() || !_store._journal) { return false; }
	const auto journal = *_store._journal;
	if (!fresh.current || !fresh.current() || !_store.StateMatches() || !_store._journal
		|| SerializeSyncCarryJournal(*_store._journal).canonical != SerializeSyncCarryJournal(journal).canonical) { return false; }
	const auto &origin = fresh.target.peer.origin;
	return ValidDesktopSyncChatOrigin(origin) && fresh.source.peer.origin == origin
		&& origin.account == _store._scope.account && fresh.loginAuthority == _store._scope.loginAuthority
		&& fresh.source.peer.chat == journal.source && fresh.target.peer.chat == journal.target;
}

bool DesktopSyncChatCarryIo::Fresh(const DesktopSyncCarryFresh &fresh) const {
	if (!Context(fresh)) { return false; }
	const auto &journal = *_store._journal;
	const auto resolution = ResolveSyncChat(fresh.resolutionInput);
	const auto eligible = DesktopSyncChatEligibility(fresh.target.peer);
	return ValidDesktopSyncChatIo(fresh.target) && eligible && !eligible->readers.populationUnknown
		&& fresh.resolutionInput.selfUserId == fresh.target.peer.origin.account.userId
		&& (!fresh.resolutionInput.cache || (fresh.resolutionInput.cacheRead != SyncChatDirectRead::Unknown
			&& fresh.resolutionInput.cacheRead != SyncChatDirectRead::Migrated))
		&& resolution.action == journal.expectedAction && resolution.winner == journal.expectedWinner
		&& resolution.cache == journal.oldMark;
}

std::optional<QJsonObject> DesktopSyncChatCarryIo::Transport() const {
	if (!_store.Current() || !_store._journal) { return {}; }
	const auto value = _store._envelope.value(u"transport"_q);
	if (value.isUndefined()) {
		return _store._envelope.contains(u"transport_sha256"_q) ? std::nullopt
			: std::optional(QJsonObject{ {u"version"_q, 1}, {u"entries"_q, QJsonObject()} });
	}
	if (!value.isObject()) { return {}; }
	const auto result = value.toObject();
	if (!result.value(u"version"_q).isDouble() || result.value(u"version"_q).toDouble() != 1
		|| !result.value(u"entries"_q).isObject() || result.value(u"entries"_q).toObject().size() > 1024) { return {}; }
	const auto digest = TransportHash(result,_store._envelope,*_store._journal);
	if (!digest || _store._envelope.value(u"transport_sha256"_q).toString() != *digest) { return {}; }
	const auto entries = result.value(u"entries"_q).toObject();
	auto randoms = std::set<uint64_t>();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		if (!IsSyncCarryHash(i.key()) || !i.value().isObject() || !Entry(i.value().toObject())) { return {}; }
		const auto entry = i.value().toObject();
		const auto key = SyncCarryRecordHash((entry.value(u"kind"_q).toString() + u":"_q
			+ QString::number(entry.value(u"source_id"_q).toInt()) + u":"_q + entry.value(u"hash"_q).toString()).toUtf8());
		if (i.key() != key || !randoms.emplace(*Id(entry.value(u"random_id"_q))).second) { return {}; }
		const auto random = Id(entry.value(u"random_id"_q));
		const auto own = Id(_store._journal->ownRequestId);
		const auto mark = Id(_store._journal->markRequestId);
		const auto kind = entry.value(u"kind"_q).toString();
		if ((kind == u"own"_q && random != own) || (kind == u"mark"_q && random != mark)
			|| (kind == u"forward"_q && (random == own || (mark && random == mark)))) { return {}; }
	}
	if (!entries.isEmpty() && (!result.value(u"readers"_q).isObject() || !result.value(u"reader_warning"_q).isBool())) { return {}; }
	return result;
}

bool DesktopSyncChatCarryIo::Save(QJsonObject transport) {
	if (!_store.Current() || !_store._journal) { return false; }
	const auto digest = TransportHash(transport,_store._envelope,*_store._journal);
	if (!digest) { return false; }
	const auto before = _store._envelope;
	_store._envelope.insert(u"transport"_q, std::move(transport));
	_store._envelope.insert(u"transport_sha256"_q,*digest);
	const auto written = _store.Persist(SerializeSyncCarryJournal(*_store._journal));
	if (!written) { _store._envelope = before; }
	return bool(written);
}

bool DesktopSyncChatCarryIo::Readers(QJsonObject &transport, const DesktopSyncCarryFresh &fresh) {
	const auto eligible = DesktopSyncChatEligibility(fresh.target.peer);
	if (!eligible || !*eligible || eligible->readers.populationUnknown) { return false; }
	const auto readers = eligible->readers;
	const auto snapshot = QJsonObject{ {u"members"_q, Members(fresh.target.peer)},
		{u"other_members"_q, readers.otherMembers ? QString::number(*readers.otherMembers) : QString()},
		{u"other_admins"_q, readers.otherAdmins ? QString::number(*readers.otherAdmins) : QString()},
		{u"warning"_q, readers.warningRequired} };
	if (!transport.contains(u"readers"_q)) {
		if (readers.warningRequired) {
			if (!fresh.readerConsentAccepted || !fresh.consentPeer) { return false; }
			const auto opening = DesktopSyncChatEligibility(*fresh.consentPeer);
			if (!opening || !*opening || opening->readers != readers
				|| fresh.consentPeer->origin != fresh.target.peer.origin || fresh.consentPeer->chat != fresh.target.peer.chat
				|| fresh.consentPeer->kind != fresh.target.peer.kind || Members(*fresh.consentPeer) != Members(fresh.target.peer)) { return false; }
		}
		transport.insert(u"readers"_q, snapshot);
		transport.insert(u"reader_warning"_q, false);
	} else {
		const auto previous = transport.value(u"readers"_q).toObject();
		if (!previous.value(u"members"_q).isArray() || !previous.value(u"other_members"_q).isString()
			|| !previous.value(u"other_admins"_q).isString() || !previous.value(u"warning"_q).isBool()) { return false; }
		const auto members = previous.value(u"members"_q).toArray();
		auto seen = std::set<QString>();
		for (const auto &member : members) { if (!member.isString() || member.toString().isEmpty() || !seen.emplace(member.toString()).second) { return false; } }
		for (const auto &name : {u"other_members"_q, u"other_admins"_q}) {
			const auto text = previous.value(name).toString();
			auto ok = false; const auto count = text.toULongLong(&ok);
			if (!text.isEmpty() && (!ok || QString::number(count) != text)) { return false; }
		}
		auto added = readers.warningRequired && !previous.value(u"warning"_q).toBool();
		for (const auto &member : snapshot.value(u"members"_q).toArray()) { if (!members.contains(member)) { added = true; } }
		for (const auto &name : { u"other_members"_q, u"other_admins"_q }) {
			if (snapshot.value(name).toString().toULongLong() > previous.value(name).toString().toULongLong()) { added = true; }
		}
		if (added) { transport.insert(u"reader_warning"_q, true); }
	}
	return true;
}

std::optional<DesktopSyncCarryIdentity> DesktopSyncChatCarryIo::Reserve(
		DesktopSyncCarryEffect effect, const QString &recordHash,
		const DesktopSyncCarryFresh &fresh, uint64_t proposedRandomId, uint64_t proposedFileId) {
	if (!Fresh(fresh)) { return {}; }
	const auto journal = *_store._journal;
	if (journal.mode == SyncCarryMode::Switch && journal.markText != DesktopSyncChatMarkText()) { return {}; }
	const auto mark = effect == DesktopSyncCarryEffect::Mark;
	if (mark ? (journal.phase != SyncCarryPhase::Mark || journal.markAttempted || !journal.destinationVerified)
		: (journal.phase != SyncCarryPhase::Deliver || !journal.plan)) { return {}; }
	auto sourceId = int32_t(0);
	auto hash = recordHash;
	auto randomId = proposedRandomId;
	if (mark) {
		hash = SyncCarryRecordHash(journal.markText.toUtf8());
		randomId = Id(journal.markRequestId).value_or(0);
	} else if (effect == DesktopSyncCarryEffect::OwnRecord) {
		if (!SyncCarryJournalPostsOwnRecord(journal) || recordHash != journal.plan->ownRecordHash) { return {}; }
		randomId = Id(journal.ownRequestId).value_or(0);
	} else {
		if (!ValidDesktopSyncChatIo(fresh.source) || journal.plan->access != SyncCarrySourceAccess::Readable) { return {}; }
		const auto forwards = SyncCarryJournalForwards(journal);
		const auto found = std::find_if(forwards.begin(), forwards.end(), [&](const auto &reference) { return reference.recordHash == recordHash && fresh.sourceRecord
			&& reference.messageId == fresh.sourceRecord->record.id; });
		if (found == forwards.end()) { return {}; }
		sourceId = found->messageId;
		if (!ExactForward(journal, fresh, recordHash, sourceId)) { return {}; }
	}
	const auto ownId = Id(journal.ownRequestId);
	const auto markId = Id(journal.markRequestId);
	if (!ownId || (journal.mode == SyncCarryMode::Switch && (!markId || ownId == markId))
		|| (effect == DesktopSyncCarryEffect::Forward && (randomId == *ownId || (markId && randomId == *markId)))) { return {}; }
	if (!IsSyncCarryHash(hash) || !randomId || (effect == DesktopSyncCarryEffect::OwnRecord && !proposedFileId)) { return {}; }
	const auto key = SyncCarryRecordHash((Kind(effect) + u":"_q + QString::number(sourceId) + u":"_q + hash).toUtf8());
	auto transport = Transport();
	if (!transport || !Readers(*transport, fresh)) { return {}; }
	auto entries = transport->value(u"entries"_q).toObject();
	if (!mark && journal.mode == SyncCarryMode::Switch) {
		const auto markHash = SyncCarryRecordHash(journal.markText.toUtf8());
		const auto markKey = SyncCarryRecordHash((u"mark:0:"_q + markHash).toUtf8());
		if (!entries.contains(markKey)) {
			if (entries.size() == 1024) { return {}; }
			entries.insert(markKey,QJsonObject{
				{u"kind"_q,u"mark"_q}, {u"hash"_q,markHash}, {u"source_id"_q,0},
				{u"random_id"_q,QString::number(*markId)}, {u"file_id"_q,u"0"_q},
				{u"state"_q,u"prepared"_q} });
		}
	}
	transport->insert(u"entries"_q,entries);
	if (entries.contains(key)) {
		const auto entry = entries.value(key).toObject();
		if (entry.value(u"kind"_q).toString() != Kind(effect) || entry.value(u"hash"_q).toString() != hash
			|| entry.value(u"source_id"_q).toInt() != sourceId
			|| (effect != DesktopSyncCarryEffect::Forward && Id(entry.value(u"random_id"_q)) != randomId)) { return {}; }
		if (!Save(*transport)) { return {}; }
		return DesktopSyncCarryIdentity{ key, *Id(entry.value(u"random_id"_q)), Id(entry.value(u"file_id"_q)).value_or(0) };
	}
	if (entries.size() == 1024) { return {}; }
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		if (Id(i.value().toObject().value(u"random_id"_q)) == randomId) { return {}; }
	}
	entries.insert(key, QJsonObject{ {u"kind"_q, Kind(effect)}, {u"hash"_q, hash}, {u"source_id"_q, sourceId},
		{u"random_id"_q, QString::number(randomId)}, {u"file_id"_q, QString::number(effect == DesktopSyncCarryEffect::OwnRecord ? proposedFileId : 0)},
		{u"state"_q, u"prepared"_q} });
	transport->insert(u"entries"_q, entries);
	return Save(*transport) ? std::optional(DesktopSyncCarryIdentity{ key, randomId, effect == DesktopSyncCarryEffect::OwnRecord ? proposedFileId : 0 }) : std::nullopt;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarryIo::Permit(
		const QString &slot, const QJsonObject &entry, const QJsonObject &transport,
		const DesktopSyncCarryFresh &fresh, std::shared_ptr<std::atomic<bool>> active) {
	if (!Fresh(fresh) || !Entry(entry)
		|| (_store._journal->mode == SyncCarryMode::Switch
			&& _store._journal->markText != DesktopSyncChatMarkText())) { return {}; }
	const auto &journal = *_store._journal;
	const auto kind = entry.value(u"kind"_q).toString();
	const auto mark = kind == u"mark"_q;
	if (mark ? (journal.phase != SyncCarryPhase::Mark || !journal.markAttempted || !journal.destinationVerified)
		: (journal.phase != SyncCarryPhase::VerifyDestination || !journal.plan)) { return {}; }
	if (!mark) {
		auto delivery = journal;
		delivery.phase = SyncCarryPhase::Deliver;
		if (!SerializeSyncCarryJournal(delivery)) { return {}; }
		const auto entries = transport.value(u"entries"_q).toObject();
		const auto reserved = [&](const QString &kind, int32_t id, const QString &hash) {
			const auto key = SyncCarryRecordHash((kind + u":"_q + QString::number(id) + u":"_q + hash).toUtf8());
			const auto saved = entries.value(key).toObject();
			return Entry(saved) && saved.value(u"kind"_q).toString() == kind
				&& saved.value(u"source_id"_q).toInt() == id && saved.value(u"hash"_q).toString() == hash;
		};
		if (journal.mode == SyncCarryMode::Switch
			&& !reserved(u"mark"_q, 0, SyncCarryRecordHash(journal.markText.toUtf8()))) { return {}; }
		for (const auto &reference : SyncCarryJournalForwards(delivery)) {
			if (!reserved(u"forward"_q, reference.messageId, reference.recordHash)) { return {}; }
		}
		if (SyncCarryJournalPostsOwnRecord(delivery)
			&& !reserved(u"own"_q, 0, delivery.plan->ownRecordHash)) { return {}; }
	}
	const auto hash = entry.value(u"hash"_q).toString();
	if (mark ? (hash != SyncCarryRecordHash(journal.markText.toUtf8()) || Id(journal.markRequestId) != Id(entry.value(u"random_id"_q)))
		: kind == u"own"_q ? (hash != journal.plan->ownRecordHash || Id(journal.ownRequestId) != Id(entry.value(u"random_id"_q)))
		: (!ExactForward(journal, fresh, hash, entry.value(u"source_id"_q).toInt()))) { return {}; }
	auto permit = DesktopSyncCarryPermit();
	permit._operation = journal.operationId; permit._revision = journal.revision;
	permit._effect = mark ? DesktopSyncCarryEffect::Mark : kind == u"own"_q ? DesktopSyncCarryEffect::OwnRecord : DesktopSyncCarryEffect::Forward;
	permit._identity = {slot, *Id(entry.value(u"random_id"_q)), Id(entry.value(u"file_id"_q)).value_or(0)};
	if (kind == u"own"_q) { permit._content = journal.plan->ownRecord; }
	permit._sourceId = entry.value(u"source_id"_q).toInt(); permit._hash = hash; permit._mark = journal.markText;
	permit._source = fresh.source; permit._target = fresh.target; permit._current = fresh.current;
	permit._epoch = _store._ioEpoch; permit._openingEpoch = permit._epoch->load(); permit._active = std::move(active);
	permit._readerWarning = transport.value(u"reader_warning"_q).toBool();
	return permit;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarryIo::Authorize(const QString &slot, const DesktopSyncCarryFresh &fresh) {
	if (!Fresh(fresh)) { return {}; }
	auto transport = Transport();
	if (!transport || !Readers(*transport, fresh)) { return {}; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(slot).toObject();
	if (!Entry(entry) || entry.value(u"state"_q).toString() != u"prepared"_q) { return {}; }
	if (!ReceiptCapacity(_store._envelope,*transport)) { return {}; }
	auto active = std::make_shared<std::atomic<bool>>(true);
	const auto permit = Permit(slot, entry, *transport, fresh, active);
	if (!permit) { return {}; }
	entry.insert(u"state"_q, u"issued"_q); entries.insert(slot, entry); transport->insert(u"entries"_q, entries);
	if (!Save(*transport)) { active->store(false); return {}; }
	_active[slot] = { _store._ioEpoch->load(), active };
	_store._ioActive[slot] = _active[slot];
	return permit;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarryIo::Renew(const DesktopSyncCarryPermit &opening, const DesktopSyncCarryFresh &fresh) {
	if (!opening.current() || !Fresh(fresh)) { return {}; }
	auto transport = Transport();
	if (!transport || !Readers(*transport, fresh) || !Save(*transport)) { return {}; }
	const auto entry = transport->value(u"entries"_q).toObject().value(opening._identity.slot).toObject();
	if (entry.value(u"state"_q).toString() != u"issued"_q) { return {}; }
	auto permit = Permit(opening._identity.slot, entry, *transport, fresh, opening._active);
	if (!permit || opening._renewal == std::numeric_limits<uint64_t>::max()) { return {}; }
	permit->_renewal = opening._renewal + 1;
	return opening.compatible(*permit) ? permit : std::nullopt;
}

std::optional<int32_t> DesktopSyncChatCarryIo::RecordReceipt(
		const DesktopSyncCarryPermit &permit, const MTPUpdates &updates) {
	if (!permit.current() || !_store.Current() || !_store._journal
		|| _store._journal->operationId != permit._operation || _store._journal->revision != permit._revision) { return {}; }
	const auto id = DesktopSyncChatUploadReceipt(permit.target(), updates, permit.randomId());
	auto transport = Transport();
	if (!id || !transport) { return {}; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(permit._identity.slot).toObject();
	if (!Entry(entry) || entry.value(u"state"_q).toString() != u"issued"_q
		|| (entry.contains(u"ack_id"_q) && entry.value(u"ack_id"_q).toInt() != *id)) { return {}; }
	entry.insert(u"ack_id"_q,*id); entries.insert(permit._identity.slot,entry); transport->insert(u"entries"_q,entries);
	return Save(*transport) ? id : std::nullopt;
}

bool DesktopSyncChatCarryIo::OutcomeUnknown(const DesktopSyncCarryPermit &permit) {
	if (!permit.current() || !_store.Current() || !_store._journal
		|| _store._journal->operationId != permit._operation || _store._journal->revision != permit._revision) { return false; }
	auto transport = Transport();
	if (!transport) { return false; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(permit._identity.slot).toObject();
	if (!Entry(entry) || entry.value(u"state"_q).toString() != u"issued"_q) { return false; }
	entry.insert(u"state"_q, u"unknown"_q); entries.insert(permit._identity.slot, entry); transport->insert(u"entries"_q, entries);
	permit._active->store(false);
	return Save(*transport);
}

DesktopSyncCarryReconcile DesktopSyncChatCarryIo::Reconcile(const QString &slot, const DesktopSyncChatInventory &input, const DesktopSyncCarryFresh &fresh) {
	if (!Context(fresh) || input.route.peer.origin != fresh.target.peer.origin
		|| input.route.peer.chat != _store._journal->target) { return DesktopSyncCarryReconcile::Held; }
	const auto inventory = DesktopSyncChatCarryInventory(input);
	auto transport = Transport();
	if (!inventory || !transport) { return DesktopSyncCarryReconcile::Held; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(slot).toObject();
	if (!Entry(entry) || entry.value(u"kind"_q).toString() == u"mark"_q) { return DesktopSyncCarryReconcile::Held; }
	if (const auto active = _store._ioActive[slot].second.lock(); active && active->load()
		&& _store._ioActive[slot].first == _store._ioEpoch->load()) { return DesktopSyncCarryReconcile::Held; }
	const auto found = std::any_of(inventory->messages.begin(), inventory->messages.end(), [&](const auto &message) { return message.reference.recordHash == entry.value(u"hash"_q).toString(); });
	entry.insert(u"state"_q, found ? u"verified"_q : u"prepared"_q); entries.insert(slot, entry); transport->insert(u"entries"_q, entries);
	return Save(*transport) ? (found ? DesktopSyncCarryReconcile::Present : DesktopSyncCarryReconcile::Absent) : DesktopSyncCarryReconcile::Held;
}

DesktopSyncCarryReconcile DesktopSyncChatCarryIo::ReconcileMark(
		const QString &slot, const DesktopSyncChatIo &route, int32_t id, const MTPmessages_Messages &response, const DesktopSyncCarryFresh &fresh) {
	if (!Context(fresh) || route.peer.origin != fresh.target.peer.origin
		|| route.peer.chat != _store._journal->target) { return DesktopSyncCarryReconcile::Held; }
	const auto read = DesktopSyncChatCarryMarkReadback(route, id, response, _store._journal->markText);
	auto transport = Transport();
	if (!read || !transport) { return DesktopSyncCarryReconcile::Held; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(slot).toObject();
	if (!Entry(entry) || entry.value(u"kind"_q).toString() != u"mark"_q
		|| entry.value(u"ack_id"_q).toInt() != id) { return DesktopSyncCarryReconcile::Held; }
	if (const auto active = _store._ioActive[slot].second.lock(); active && active->load()
		&& _store._ioActive[slot].first == _store._ioEpoch->load()) { return DesktopSyncCarryReconcile::Held; }
	entry.insert(u"state"_q, u"verified"_q); entries.insert(slot, entry); transport->insert(u"entries"_q, entries);
	return Save(*transport) ? DesktopSyncCarryReconcile::Present : DesktopSyncCarryReconcile::Held;
}

}
