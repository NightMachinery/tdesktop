#include "purple/purple_sync_chat_cleanup.h"
#include "purple/purple_sync_request_sender.h"

#include <QtCore/QJsonArray>
#include <algorithm>
#include <limits>

namespace Purple {

bool DesktopSyncChatCleanupActivated() { return false; }

DesktopSyncCleanupProgress DesktopSyncChatCleanupProgressOf(
		const SyncCarryJournal &journal, bool pendingRequest, size_t confirmed, bool uncopied) {
	const auto pending = journal.phase == SyncCarryPhase::Cleanup && journal.stateCommitted && journal.cacheCompleted;
	return {pending, pending && !pendingRequest, journal.phase == SyncCarryPhase::CleanupSkipped, uncopied, confirmed};
}

bool DesktopSyncCleanupPermit::operationCurrent() const {
	return _operationLive && _operationLive->load() && _admitted && _admitted->load()
		&& _epoch && _epoch->load() == _openingEpoch;
}
bool DesktopSyncCleanupPermit::current() const { return operationCurrent() && _slot && _slot->load() == _generation; }
const DesktopSyncCleanupItem &DesktopSyncCleanupPermit::item() const { return _item; }
const DesktopSyncChatIo &DesktopSyncCleanupPermit::route() const { return _route; }
void DesktopSyncCleanupPermit::revoke() const {
	if (_slot) { auto expected=_generation; _slot->compare_exchange_strong(expected,_generation+1); }
}
std::optional<DesktopSyncChatDelete> DesktopSyncCleanupPermit::request() const {
	if (!current() || !_claimed || _claimed->exchange(true)) { return {}; }
	return DesktopSyncChatDeleteRequest(_route, _item.reference.messageId);
}
bool DesktopSyncCleanupPermit::successor(const DesktopSyncCleanupPermit &next) const {
	return operationCurrent() && next.current() && _operationLive == next._operationLive && _epoch == next._epoch
		&& _openingEpoch == next._openingEpoch && _slot == next._slot && _item == next._item
		&& _operation == next._operation && _revision == next._revision && next._generation > _generation
		&& CompatibleDesktopSyncChatIo(_route, next._route);
}

DesktopSyncChatCleanup::DesktopSyncChatCleanup(SyncChatCarryStore &store, DesktopSyncChatOrigin origin,
		std::shared_ptr<std::atomic<bool>> admitted)
: _store(store), _origin(std::move(origin)), _admitted(std::move(admitted)) {}
DesktopSyncChatCleanup::~DesktopSyncChatCleanup() { revoke(); }
void DesktopSyncChatCleanup::revoke() { _live->store(false); _slot->fetch_add(1); _permit.reset(); _read.reset(); }

bool DesktopSyncChatCleanup::context(const DesktopSyncCarryFresh &fresh,
		const DesktopSyncChatInventory &destination, const SyncPlaylistCarryReceipt &playlist) {
	if (!_live->load() || !_admitted || !_admitted->load() || !_store.WitnessCleanup()
		|| !fresh.current || !fresh.current() || !fresh.target.peer.facts) { return false; }
	const auto &j = *_store.journal();
	auto readerGuard=DesktopSyncChatCarryIo(_store);
	if (!readerGuard.CheckReaders(fresh)) { return false; }
	const auto eligible = DesktopSyncChatEligibility(fresh.target.peer);
	const auto inventory = DesktopSyncChatCarryInventory(destination);
	return fresh.source.peer.origin == _origin && fresh.target.peer.origin == _origin
		&& fresh.source.peer.chat == j.source && fresh.target.peer.chat == j.target
		&& fresh.loginAuthority == _store._scope.loginAuthority && eligible && *eligible
		&& CheckSyncCarryReaders(j,*eligible) != SyncCarryReaderVerdict::Hold
		&& DesktopSyncChatInventoryMatches(destination,_origin,j.target) && inventory
		&& bool(VerifySyncCarryReadback(*j.plan,*inventory,playlist));
}

bool DesktopSyncChatCleanup::prepare(const DesktopSyncCarryFresh &fresh,
		const DesktopSyncChatInventory &destination, const SyncPlaylistCarryReceipt &playlist) {
	if (!context(fresh,destination,playlist)) { return false; }
	const auto &j = *_store.journal();
	const auto inventory = *DesktopSyncChatCarryInventory(destination);
	_items.clear();
	for (const auto &reference : SyncCarryCleanupCandidates(*j.plan,inventory,playlist)) {
		const auto metadata = std::find_if(j.plan->cleanupCandidates.begin(),j.plan->cleanupCandidates.end(),[&](const auto &v) { return v.reference == reference; });
		if (metadata == j.plan->cleanupCandidates.end() || metadata->date <= 0) { return false; }
		const auto key = SyncCarryRecordHash((u"record:"_q + QString::number(reference.messageId) + u":"_q + reference.recordHash).toUtf8());
		_items.push_back({reference,metadata->date,false,key});
	}
	if (j.removeOldMarkAuthorized && j.oldMark && j.oldMark->markId > 0) {
		const auto key = SyncCarryRecordHash((u"mark:"_q + QString::number(j.oldMark->markId) + u":"_q + QString::number(j.oldMark->markDate)).toUtf8());
		_items.push_back({{j.account,j.source,j.oldMark->markId,{}},j.oldMark->markDate,true,key});
	}
	_uncopied = false;
	const auto carried = [&](const QString &hash) { return hash == j.plan->ownRecordHash || std::find(j.plan->requiredHashes.begin(),j.plan->requiredHashes.end(),hash) != j.plan->requiredHashes.end(); };
	for (const auto &message : inventory.messages) {
		const auto record = ClassifySyncCandidate(message.reference.messageId,message.bytes);
		if (message.ownership==SyncChatMessageOwnership::Own && message.family==SyncCarryFamily::Settings && record.header && record.header->stream == u"config"_q && record.header->writerInstall == j.plan->ownerInstall
			&& record.header->writerDevice == j.plan->ownerDevice && !carried(message.reference.recordHash)) { _uncopied = true; }
	}
	return true;
}

std::optional<DesktopSyncCleanupRead> DesktopSyncChatCleanup::next() {
	if (!_store.WitnessCleanup() || !_live->load() || !_admitted->load()) { return {}; }
	const auto ledger = _store.CleanupLedger(); if (!ledger) { return {}; }
	for (const auto &item : _items) {
		if (ledger->value(item.key).toObject().value(u"state"_q) == QJsonValue(u"absent"_q)) { continue; }
		if (_slot->load() >= std::numeric_limits<uint64_t>::max()-1) { return {}; }
		_permit.reset();
		_read = DesktopSyncCleanupRead{item,_slot->fetch_add(1)+1,DesktopSyncChatReadStamp::Begin(),_identity};
		return _read;
	}
	_read.reset(); return {};
}

bool DesktopSyncChatCleanup::save(const DesktopSyncCleanupItem &item, const QString &state, const QByteArray &body) {
	auto ledger = _store.CleanupLedger(); if (!ledger) { return false; }
	auto entry = ledger->value(item.key).toObject();
	const auto previous = entry.value(u"body_sha256"_q).toString();
	if (!body.isEmpty()) {
		if (!previous.isEmpty() && previous != SyncCarryRecordHash(body)) { return false; }
		const auto stored = _store.StoreObject(item.mark ? u"text"_q : u"record"_q,body); if (!stored) { return false; }
		entry.insert(u"body_sha256"_q,stored.objectHash);
	}
	entry.insert(u"kind"_q,item.mark ? u"mark"_q : u"record"_q); entry.insert(u"id"_q,item.reference.messageId);
	entry.insert(u"date"_q,QString::number(item.date)); entry.insert(u"hash"_q,item.reference.recordHash);
	entry.insert(u"state"_q,state); ledger->insert(item.key,entry);
	return bool(_store.SaveCleanupLedger(*ledger));
}

bool DesktopSyncChatCleanup::inspect(const DesktopSyncCleanupRead &read, const DesktopSyncCarryFresh &fresh,
		const DesktopSyncChatInventory &destination, const SyncPlaylistCarryReceipt &playlist,
		const MTPmessages_Messages &response, const std::optional<DesktopSyncChatRecordReceipt> &record) {
	if (read.owner != _identity || !_read || _read->item != read.item || _read->generation != read.generation || read.generation != _slot->load()
		|| !destination.readStarted.after(read.after) || !context(fresh,destination,playlist)) { return false; }
	const auto item = read.item;
	_read.reset();
	const auto native = DesktopSyncChatRecordMessage(fresh.source,response,item.reference.messageId);
	if (!native) { return false; }
	if (native->type() == mtpc_messageEmpty) {
		if (!native->c_messageEmpty().vpeer_id() || !save(item,u"absent"_q,{})) { return false; }
		_slot->fetch_add(1); _permit.reset(); return true;
	}
	if (!ValidDesktopSyncChatIo(fresh.source) || native->type() != mtpc_message) { return false; }
	const auto &j = *_store.journal();
	const auto inventory = *DesktopSyncChatCarryInventory(destination);
	auto body = QByteArray();
	if (item.mark) {
		const auto &message=native->c_message(); const auto text=qs(message.vmessage());
		const auto own=GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(_origin,fresh.source.peer,*native));
		const auto readback=SyncCarryMarkReadback{SyncCarryMarkOutcome::Valid,j.account,j.source,item.reference.messageId,message.vdate().v,text,bool(message.vmedia()),own};
		if (readback.date != item.date || !CheckSyncCarryOldMarkDeletion(j,readback,inventory,playlist)) { return false; }
		body = text.toUtf8();
	} else {
		if (!record || record->route.peer.origin != _origin || record->route.peer.chat != j.source
			|| record->record.id != item.reference.messageId || !SealDesktopSyncChatRecord(fresh.source,record->response,record->record,record->record.documentId,record->record.editDate)) { return false; }
		auto a=mtpBuffer(),b=mtpBuffer(); response.write(a); record->response.write(b); if (a!=b) { return false; }
		const auto message = MakeSyncCarryMessage(j.account,j.source,item.reference.messageId,native->c_message().vdate().v,record->record.bytes,
			GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(_origin,fresh.source.peer,*native)));
		if (!message || message->reference != item.reference || message->date != item.date
			|| !CheckSyncCarryDeletion(j,*message,inventory,playlist)) { return false; }
		body = message->bytes;
	}
	if (!save(item,u"issued"_q,body)) { return false; }
	auto permit = DesktopSyncCleanupPermit(); permit._item=item; permit._route=fresh.source;
	permit._operation=j.operationId; permit._revision=j.revision; permit._generation=read.generation;
	permit._operationLive=_live; permit._admitted=_admitted; permit._epoch=_store._ioEpoch;
	permit._openingEpoch=_store._ioEpoch->load(); permit._slot=_slot; permit._claimed=std::make_shared<std::atomic<bool>>(false);
	_permit=permit; return true;
}
std::optional<DesktopSyncCleanupPermit> DesktopSyncChatCleanup::permit() const { return _permit && _permit->current() ? _permit : std::nullopt; }
std::optional<std::vector<SyncCarryReference>> DesktopSyncChatCleanup::deleted() const {
	const auto ledger=_store.CleanupLedger(); if (!ledger) { return {}; }
	auto result=std::vector<SyncCarryReference>();
	for (const auto &item:_items) { if (!item.mark && ledger->value(item.key).toObject().value(u"state"_q)==QJsonValue(u"absent"_q)) { result.push_back(item.reference); } }
	return result;
}
SyncCarryMarkReadback DesktopSyncChatCleanup::oldMarkAbsent() const {
	const auto ledger=_store.CleanupLedger(); if (!ledger) { return {}; }
	for (const auto &item:_items) { if (item.mark && ledger->value(item.key).toObject().value(u"state"_q)==QJsonValue(u"absent"_q)) {
		return {SyncCarryMarkOutcome::Absent,_origin.account,item.reference.chat,item.reference.messageId,item.date};
	} }
	return {};
}
bool DesktopSyncChatCleanup::uncopied() const { return _uncopied; }

bool SendDesktopSyncChatCleanup(SyncRequestSender &sender, DesktopSyncCleanupPermit permit,
		DesktopSyncCleanupRenew renew, Fn<void(Fn<void()>)> onSender,
		Fn<void(int, Fn<void()>)> delay, Fn<void(std::optional<MTPmessages_AffectedMessages>)> done) {
	const auto request=permit.request(); if (!request || !renew || !onSender || !delay || !done) { return false; }
	auto wire=mtpBuffer(); std::visit([&](const auto &v) { v.write(wire); },*request);
	const auto active=std::make_shared<DesktopSyncCleanupPermit>(std::move(permit));
	const auto completed=std::make_shared<std::atomic<bool>>(false);
	const auto finish=[done,completed,active](std::optional<MTPmessages_AffectedMessages> response) { if (!completed->exchange(true)) { const auto release=gsl::finally([&] { active->revoke(); }); done(std::move(response)); } };
	std::visit([&](auto value) {
		sender.request(std::move(value)).done([finish](const MTPmessages_AffectedMessages &response) { finish(response); })
		.fail([finish](const MTP::Error &) { finish({}); })
		.resumeEffect([active,renew,onSender,wire,finish,completed,delay](Fn<void(bool)> ready) {
			if (completed->load() || !active->operationCurrent()) { finish({}); ready(false); return; }
			const auto retired=std::make_shared<std::atomic<bool>>(false);
			delay(60000,[retired,finish,ready] { if (!retired->exchange(true)) { finish({}); ready(false); } });
			renew([active,onSender,wire,finish,completed,retired,ready=std::move(ready)](std::optional<DesktopSyncCleanupPermit> next) mutable {
				onSender([active,wire,finish,completed,retired,next=std::move(next),ready=std::move(ready)]() mutable {
					if (retired->exchange(true)) { return; }
					if (completed->load()) { ready(false); return; }
					if (!next || !active->successor(*next)) { finish({}); ready(false); return; }
					const auto request=next->request(); auto actual=mtpBuffer();
					if (request) { std::visit([&](const auto &v) { v.write(actual); },*request); }
					if (!request || actual!=wire || !next->current()) { finish({}); ready(false); return; }
					*active=*next; ready(true);
				});
			});
		}).send();
	},*request);
	return true;
}

}
