#include "purple/purple_sync_chat_carry_io.h"

#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_chat_cache.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <limits>
#include <set>

namespace Purple {
namespace {

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

bool ReceiptCapacity(const QJsonObject &envelope, QJsonObject transport) {
	auto entries = transport.value(u"entries"_q).toObject();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		auto entry = i.value().toObject();
		entry.insert(u"ack_id"_q,std::numeric_limits<int32_t>::max());
		entry.insert(u"generation"_q,QString::number(std::numeric_limits<uint64_t>::max()));
		i.value() = entry;
	}
	transport.insert(u"entries"_q,entries);
	auto future = envelope;
	future.insert(u"transport"_q,transport);
	future.insert(u"transport_sha256"_q,QString(64,u'a'));
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(future).toJson(QJsonDocument::Compact));
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

}

bool DesktopSyncCarryPermit::current() const {
	if (!_epoch || !_active || !_active->load() || _epoch->load() != _openingEpoch) { return false; }
	const auto allowed = _current && _current();
	return allowed && _active->load() && _epoch->load() == _openingEpoch;
}
bool DesktopSyncCarryPermit::claimUpload() const {
	return current() && (_effect == DesktopSyncCarryEffect::OwnRecord || _effect == DesktopSyncCarryEffect::Pending)
		&& _uploadClaim && !_uploadClaim->exchange(true);
}
void DesktopSyncCarryPermit::revoke() const { if (_active) { _active->store(false); } }
const DesktopSyncChatIo &DesktopSyncCarryPermit::source() const { return _source; }
const DesktopSyncChatIo &DesktopSyncCarryPermit::target() const { return _target; }
uint64_t DesktopSyncCarryPermit::randomId() const { return _identity.randomId; }
uint64_t DesktopSyncCarryPermit::fileId() const { return _identity.fileId; }
uint64_t DesktopSyncCarryPermit::renewal() const { return _renewal; }
bool DesktopSyncCarryPermit::readerWarning() const { return _readerWarning; }
bool DesktopSyncCarryPermit::uploadMatches(const QByteArray &bytes) const {
	return current() && (_effect == DesktopSyncCarryEffect::OwnRecord || _effect == DesktopSyncCarryEffect::Pending)
		&& _identity.fileId && SyncCarryRecordHash(bytes) == _hash;
}
QByteArray DesktopSyncCarryPermit::uploadContent() const { return (_effect == DesktopSyncCarryEffect::OwnRecord || _effect == DesktopSyncCarryEffect::Pending) ? _content : QByteArray(); }
bool DesktopSyncCarryPermit::compatible(const DesktopSyncCarryPermit &fresh) const {
	return current() && fresh.current() && _epoch == fresh._epoch && _active == fresh._active
		&& _openingEpoch == fresh._openingEpoch && fresh._renewal > _renewal && _operation == fresh._operation && _revision == fresh._revision
		&& _effect == fresh._effect && _uploadClaim == fresh._uploadClaim && _identity.slot == fresh._identity.slot
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

std::optional<bool> DesktopSyncChatCarryIo::PendingIssued() const {
	return _store.Current() && _store._journal
		? DesktopSyncCarryPendingIssued(_store._envelope, *_store._journal) : std::nullopt;
}

bool DesktopSyncChatCarryIo::RememberPendingIssued() {
	return _store.RememberPendingIssued();
}

bool DesktopSyncChatCarryIo::Context(const DesktopSyncCarryFresh &fresh) const {
	if (!_store.Current() || !_store._journal || _store.foreignLogin()) { return false; }
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
		&& resolution.cache == journal.oldMark
		&& (journal.mode != SyncCarryMode::RepairMark || journal.markAttempted
			|| (fresh.resolutionInput.cacheRead == SyncChatDirectRead::MarkGone
				&& ValidDesktopSyncChatIo(fresh.source) && journal.source == journal.target));
}

bool DesktopSyncChatCarryIo::PendingFresh(const DesktopSyncCarryFresh &fresh) const {
	if (!Fresh(fresh) || !_store._journal->oldMark || _store._journal->phase != SyncCarryPhase::ResolvePending
		|| !ValidDesktopSyncChatIo(fresh.source) || !fresh.resolutionInput.cache
		|| fresh.resolutionInput.cache->chat != _store._journal->source
		|| fresh.resolutionInput.cacheRead != SyncChatDirectRead::Valid) { return false; }
	const auto eligible = DesktopSyncChatEligibility(fresh.source.peer);
	const auto cache = ParseDesktopSyncChatCache(_store._cacheFile.bytes, _store._scope.account);
	const auto current = CaptureSyncPrivateFile(_store._scope.cacheFile, 64 * 1024);
	return eligible && *eligible && !eligible->readers.populationUnknown
		&& cache.record && cache.record->loginBinding == fresh.loginAuthority
		&& cache.record->mark == *_store._journal->oldMark
		&& (!eligible->readers.warningRequired || cache.record->readerConsent)
		&& current.status == SyncStoreStatus::Ready && current.bytes == _store._cacheFile.bytes
		&& current.identity == _store._cacheFile.identity && current.parentIdentity == _store._cacheFile.parentIdentity;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarryIo::PendingPermit(
		const DesktopSyncCarryFresh &fresh, const DesktopSyncChatInventory &input,
		uint64_t proposedRandomId, uint64_t proposedFileId) {
	if (!PendingFresh(fresh) || !proposedRandomId || !proposedFileId
		|| input.route.peer.origin != fresh.source.peer.origin || input.route.peer.chat != _store._journal->source) { return {}; }
	const auto inventory = DesktopSyncChatCarryInventory(input);
	const auto staged = _store.Pending();
	if (!inventory || !staged || staged.staged.isEmpty()
		|| SyncCarryRecordHash(staged.staged) != _store._envelope.value(u"opening_pending_sha256"_q).toString()) { return {}; }
	const auto hash = SyncCarryRecordHash(staged.staged);
	if (std::any_of(inventory->messages.begin(), inventory->messages.end(), [&](const auto &message) { return message.reference.recordHash == hash; })) { return {}; }
	auto transport = Transport();
	if (!transport || !Readers(*transport, fresh)) { return {}; }
	const auto cache = ParseDesktopSyncChatCache(_store._cacheFile.bytes, _store._scope.account);
	transport->insert(u"pending_reader_warning"_q, transport->value(u"pending_reader_warning"_q).toBool()
		|| cache.record->newReaders || NewDesktopSyncChatReaders(*cache.record, fresh.source.peer));
	auto entries = transport->value(u"entries"_q).toObject();
	const auto slot = SyncCarryRecordHash((u"pending:0:"_q + hash).toUtf8());
	if (!ReadFresh(slot, input)) { return {}; }
	if (const auto active = _store._ioActive[slot].second.lock(); active && active->load()) { return {}; }
	auto entry = entries.value(slot).toObject();
	if (entries.contains(slot)) {
		if (!DesktopSyncCarryTransportEntry(entry) || entry.value(u"kind"_q).toString() != u"pending"_q
			|| entry.value(u"hash"_q).toString() != hash || entry.contains(u"ack_id"_q)) { return {}; }
	} else {
		if (entries.size() == 1024 || DesktopSyncCarryTransportId(_store._journal->ownRequestId) == proposedRandomId
			|| DesktopSyncCarryTransportId(_store._journal->markRequestId) == proposedRandomId) { return {}; }
		const auto retained = _store.RetainedImages();
		if (!retained) { return {}; }
		for (const auto &image : *retained) {
			const auto saved = QJsonDocument::fromJson(image).object();
			const auto old = ParseSyncCarryJournal(QJsonDocument(saved.value(u"journal"_q).toObject()).toJson(QJsonDocument::Compact));
			if (!old || (old.journal.operationId != _store._journal->operationId
				&& (DesktopSyncCarryTransportId(old.journal.ownRequestId) == proposedRandomId || DesktopSyncCarryTransportId(old.journal.markRequestId) == proposedRandomId))) { return {}; }
			for (const auto &value : saved.value(u"transport"_q).toObject().value(u"entries"_q).toObject()) {
				const auto effect = value.toObject();
				if (DesktopSyncCarryTransportId(effect.value(u"random_id"_q)) == proposedRandomId || DesktopSyncCarryTransportId(effect.value(u"file_id"_q)) == proposedFileId) { return {}; }
			}
		}
		entry = {{u"kind"_q,u"pending"_q},{u"hash"_q,hash},{u"source_id"_q,0},
			{u"random_id"_q,QString::number(proposedRandomId)},{u"file_id"_q,QString::number(proposedFileId)}};
	}
	entry.insert(u"state"_q,u"issued"_q); entries.insert(slot,entry); transport->insert(u"entries"_q,entries);
	if (!ReceiptCapacity(_store._envelope,*transport) || !SaveGeneration(slot,entry,*transport)) { return {}; }
	auto active = std::make_shared<std::atomic<bool>>(true);
	auto permit = Permit(slot,entry,*transport,fresh,active);
	if (!permit) { return {}; }
	permit->_uploadClaim = std::make_shared<std::atomic<bool>>(false);
	_active[slot] = {_store._ioEpoch->load(),active}; _store._ioActive[slot] = _active[slot];
	return permit;
}

std::optional<QJsonObject> DesktopSyncChatCarryIo::Transport() const {
	return _store.Current() && _store._journal
		? DesktopSyncCarryTransportOf(_store._envelope, *_store._journal) : std::nullopt;
}

bool DesktopSyncChatCarryIo::Save(QJsonObject transport) {
	if (!_store.Current() || !_store._journal) { return false; }
	const auto digest = DesktopSyncCarryTransportHash(transport,_store._envelope,*_store._journal);
	if (!digest) { return false; }
	const auto before = _store._envelope;
	_store._envelope.insert(u"transport"_q, std::move(transport));
	_store._envelope.insert(u"transport_sha256"_q,*digest);
	const auto written = _store.Persist(SerializeSyncCarryJournal(*_store._journal));
	if (!written) { _store._envelope = before; }
	return bool(written);
}

bool DesktopSyncChatCarryIo::ReadFresh(const QString &slot, const DesktopSyncChatInventory &inventory) const {
	const auto transport = Transport();
	if (!transport || !inventory.readStarted.after(_store._ioReadOpened)) { return false; }
	const auto entry = transport->value(u"entries"_q).toObject().value(slot).toObject();
	const auto generation = DesktopSyncCarryTransportGeneration(entry);
	if (!generation) { return false; }
	const auto fence = _store._ioReadFences.find(slot);
	return fence == _store._ioReadFences.end()
		|| (fence->second.first == *generation && inventory.readStarted.after(fence->second.second));
}

bool DesktopSyncChatCarryIo::SaveGeneration(const QString &slot, QJsonObject entry, QJsonObject transport) {
	const auto generation = DesktopSyncCarryTransportGeneration(entry);
	const auto fence = DesktopSyncChatReadStamp::Begin();
	if (!generation || *generation == std::numeric_limits<uint64_t>::max() || !fence.valid()) { return false; }
	entry.insert(u"generation"_q, QString::number(*generation + 1));
	auto entries = transport.value(u"entries"_q).toObject();
	entries.insert(slot, entry); transport.insert(u"entries"_q, entries);
	if (!Save(std::move(transport))) { return false; }
	_store._ioReadFences[slot] = {*generation + 1, fence};
	return true;
}

bool DesktopSyncChatCarryIo::ConsumePendingRead(const DesktopSyncChatInventory &inventory) {
	if (!_store.Current() || !_store._journal || _store._journal->phase != SyncCarryPhase::ResolvePending
		|| inventory.route.peer.origin.account != _store._scope.account
		|| inventory.route.peer.chat != _store._journal->source) { return false; }
	const auto slot = SyncCarryRecordHash((u"pending:0:"_q
		+ _store._envelope.value(u"opening_pending_sha256"_q).toString()).toUtf8());
	if (!ReadFresh(slot, inventory)) { return false; }
	const auto transport = Transport();
	const auto entries = transport->value(u"entries"_q).toObject();
	if (entries.contains(slot)) { return SaveGeneration(slot, entries.value(slot).toObject(), *transport); }
	const auto fence = DesktopSyncChatReadStamp::Begin();
	if (!fence.valid()) { return false; }
	_store._ioReadFences[slot] = {0, fence};
	return true;
}

bool DesktopSyncChatCarryIo::AcceptReaders(const DesktopSyncChatPeer &peer, const QString &stream) {
	if (!_store.Current() || !_store._journal || _store.foreignLogin()
		|| (stream != u"config"_q && stream != u"library"_q)
		|| peer.origin.account != _store._scope.account || peer.chat != _store._journal->target
		|| _store._journal->deliveryAttempted || _store._journal->markAttempted) { return false; }
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (!eligible || !*eligible || eligible->readers.populationUnknown
		|| !_store._journal->readerBaseline || *_store._journal->readerBaseline != eligible->readers) { return false; }
	auto transport = Transport();
	if (!transport || transport->contains(u"readers"_q)) { return false; }
	auto readers = QJsonObject{{u"members"_q, Members(peer)},
		{u"members_known"_q, peer.readerMembers.has_value()}, {u"admin"_q, peer.facts->admin}, {u"kind"_q, int(peer.kind)},
		{u"other_members"_q, eligible->readers.otherMembers ? QString::number(*eligible->readers.otherMembers) : QString()},
		{u"other_admins"_q, eligible->readers.otherAdmins ? QString::number(*eligible->readers.otherAdmins) : QString()},
		{u"warning"_q, eligible->readers.warningRequired}, {u"stream"_q, stream}, {u"settings_disclosed"_q, true}};
	if (peer.creator) { readers.insert(u"creator"_q, *peer.creator); }
	transport->insert(u"readers"_q, readers);
	transport->insert(u"reader_warning"_q, false);
	return Save(*transport);
}

bool DesktopSyncChatCarryIo::CheckReaders(const DesktopSyncCarryFresh &fresh) {
	auto transport = Transport();
	if (!transport) { return false; }
	if (!transport->contains(u"readers"_q)) { return !_store._envelope.value(u"consent_required"_q).toBool(); }
	const auto before = *transport;
	return Readers(*transport, fresh) && (before == *transport || Save(*transport));
}

bool DesktopSyncChatCarryIo::Readers(QJsonObject &transport, const DesktopSyncCarryFresh &fresh) {
	const auto eligible = DesktopSyncChatEligibility(fresh.target.peer);
	if (!eligible || !*eligible || eligible->readers.populationUnknown) { return false; }
	const auto verdict = _store._journal->version == 2 ? CheckSyncCarryReaders(*_store._journal, *eligible) : SyncCarryReaderVerdict::Proceed;
	if (verdict == SyncCarryReaderVerdict::Hold
		|| (verdict == SyncCarryReaderVerdict::WarnAndProceed
			&& !_store._journal->deliveryAttempted && !_store._journal->markAttempted
			&& _store._journal->mode != SyncCarryMode::RepairMark)) { return false; }
	const auto readers = _store._journal->readerBaseline.value_or(eligible->readers);
	auto snapshot = QJsonObject{ {u"members"_q, Members(fresh.target.peer)},
		{u"members_known"_q, fresh.target.peer.readerMembers.has_value()},
		{u"admin"_q, fresh.target.peer.facts->admin}, {u"kind"_q, int(fresh.target.peer.kind)},
		{u"stream"_q, u"config"_q}, {u"settings_disclosed"_q, true},
		{u"other_members"_q, readers.otherMembers ? QString::number(*readers.otherMembers) : QString()},
		{u"other_admins"_q, readers.otherAdmins ? QString::number(*readers.otherAdmins) : QString()},
		{u"warning"_q, readers.warningRequired} };
	if (!transport.contains(u"readers"_q)) {
		if (_store._envelope.value(u"consent_required"_q).toBool()) { return false; }
		if (readers.warningRequired) {
			if (!fresh.readerConsentAccepted || !fresh.consentPeer) { return false; }
			const auto opening = DesktopSyncChatEligibility(*fresh.consentPeer);
			if (!opening || !*opening || opening->readers != readers
				|| fresh.consentPeer->origin != fresh.target.peer.origin || fresh.consentPeer->chat != fresh.target.peer.chat
				|| fresh.consentPeer->kind != fresh.target.peer.kind || (_store._journal->version == 1 && Members(*fresh.consentPeer) != Members(fresh.target.peer))) { return false; }
		}
		if (_store._journal->version == 2) {
			const auto consent = fresh.consentPeer ? DesktopSyncChatEligibility(*fresh.consentPeer) : std::nullopt;
			if (consent && consent->readers == readers && fresh.consentPeer->origin == fresh.target.peer.origin
				&& fresh.consentPeer->chat == fresh.target.peer.chat) {
				snapshot.insert(u"members"_q, Members(*fresh.consentPeer));
				snapshot.insert(u"members_known"_q, fresh.consentPeer->readerMembers.has_value());
				if (fresh.consentPeer->creator) { snapshot.insert(u"creator"_q, *fresh.consentPeer->creator); }
			} else if (readers.otherMembers.value_or(0) == 0) { snapshot.insert(u"members"_q, QJsonArray()); }
			else { return false; }
		}
		transport.insert(u"readers"_q, snapshot);
		transport.insert(u"reader_warning"_q, verdict == SyncCarryReaderVerdict::WarnAndProceed);
	} else {
		const auto previous = transport.value(u"readers"_q).toObject();
		if (!previous.value(u"members"_q).isArray() || !previous.value(u"other_members"_q).isString()
			|| !previous.value(u"other_admins"_q).isString() || !previous.value(u"warning"_q).isBool()) { return false; }
		const auto known = previous.value(u"members_known"_q);
		if (!DesktopSyncCarryReaderAuthority(previous, fresh.target.peer)) { return false; }
		const auto members = previous.value(u"members"_q).toArray();
		auto seen = std::set<QString>();
		for (const auto &member : members) { if (!member.isString() || member.toString().isEmpty() || !seen.emplace(member.toString()).second) { return false; } }
		for (const auto &name : {u"other_members"_q, u"other_admins"_q}) {
			const auto text = previous.value(name).toString();
			auto ok = false; const auto count = text.toULongLong(&ok);
			if (!text.isEmpty() && (!ok || QString::number(count) != text)) { return false; }
		}
		auto added = verdict == SyncCarryReaderVerdict::WarnAndProceed
			|| (eligible->readers.warningRequired && !previous.value(u"warning"_q).toBool());
		if (known == QJsonValue(true)) {
			for (const auto &member : Members(fresh.target.peer)) { if (!members.contains(member)) { added = true; } }
		}
		for (const auto &name : { u"other_members"_q, u"other_admins"_q }) {
			if ((name == u"other_members"_q ? eligible->readers.otherMembers.value_or(0) : eligible->readers.otherAdmins.value_or(0)) > previous.value(name).toString().toULongLong()) { added = true; }
		}
		if (added && !_store._journal->deliveryAttempted && !_store._journal->markAttempted
			&& _store._journal->mode != SyncCarryMode::RepairMark) { return false; }
		if (added) { transport.insert(u"reader_warning"_q, true); }
	}
	return true;
}

std::optional<DesktopSyncCarryIdentity> DesktopSyncChatCarryIo::Reserve(
		DesktopSyncCarryEffect effect, const QString &recordHash,
		const DesktopSyncCarryFresh &fresh, uint64_t proposedRandomId, uint64_t proposedFileId) {
	if (effect == DesktopSyncCarryEffect::Pending || !Fresh(fresh)) { return {}; }
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
		randomId = DesktopSyncCarryTransportId(journal.markRequestId).value_or(0);
	} else if (effect == DesktopSyncCarryEffect::OwnRecord) {
		if (!SyncCarryJournalPostsOwnRecord(journal) || recordHash != journal.plan->ownRecordHash) { return {}; }
		randomId = DesktopSyncCarryTransportId(journal.ownRequestId).value_or(0);
	} else {
		if (!ValidDesktopSyncChatIo(fresh.source) || journal.plan->access != SyncCarrySourceAccess::Readable) { return {}; }
		const auto forwards = SyncCarryJournalForwards(journal);
		const auto found = std::find_if(forwards.begin(), forwards.end(), [&](const auto &reference) { return reference.recordHash == recordHash && fresh.sourceRecord
			&& reference.messageId == fresh.sourceRecord->record.id; });
		if (found == forwards.end()) { return {}; }
		sourceId = found->messageId;
		if (!ExactForward(journal, fresh, recordHash, sourceId)) { return {}; }
	}
	const auto ownId = DesktopSyncCarryTransportId(journal.ownRequestId);
	const auto markId = DesktopSyncCarryTransportId(journal.markRequestId);
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
			|| (effect != DesktopSyncCarryEffect::Forward && DesktopSyncCarryTransportId(entry.value(u"random_id"_q)) != randomId)) { return {}; }
		if (!Save(*transport)) { return {}; }
		return DesktopSyncCarryIdentity{ key, *DesktopSyncCarryTransportId(entry.value(u"random_id"_q)), DesktopSyncCarryTransportId(entry.value(u"file_id"_q)).value_or(0) };
	}
	if (entries.size() == 1024) { return {}; }
	const auto retained = _store.RetainedImages();
	if (!retained) { return {}; }
	for (const auto &bytes : *retained) {
		const auto envelope = QJsonDocument::fromJson(bytes).object();
		if (envelope.value(u"operation"_q).toString() == journal.operationId) { continue; }
		const auto previous = envelope.value(u"journal"_q).toObject();
		const auto oldJournal = ParseSyncCarryJournal(QJsonDocument(previous).toJson(QJsonDocument::Compact));
		if (!oldJournal || DesktopSyncCarryTransportId(oldJournal.journal.ownRequestId) == randomId || DesktopSyncCarryTransportId(oldJournal.journal.markRequestId) == randomId) { return {}; }
		const auto oldEntries = envelope.value(u"transport"_q).toObject().value(u"entries"_q).toObject();
		for (const auto &saved : oldEntries) {
			const auto old = saved.toObject();
			if (DesktopSyncCarryTransportId(old.value(u"random_id"_q)) == randomId
				|| (effect == DesktopSyncCarryEffect::OwnRecord && DesktopSyncCarryTransportId(old.value(u"file_id"_q)) == proposedFileId)) { return {}; }
		}
	}
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		if (DesktopSyncCarryTransportId(i.value().toObject().value(u"random_id"_q)) == randomId) { return {}; }
	}
	entries.insert(key, QJsonObject{ {u"kind"_q, Kind(effect)}, {u"hash"_q, hash}, {u"source_id"_q, sourceId},
		{u"random_id"_q, QString::number(randomId)}, {u"file_id"_q, QString::number(effect == DesktopSyncCarryEffect::OwnRecord ? proposedFileId : 0)},
		{u"state"_q, u"prepared"_q} });
	transport->insert(u"entries"_q, entries);
	return ReceiptCapacity(_store._envelope,*transport) && Save(*transport)
		? std::optional(DesktopSyncCarryIdentity{ key, randomId, effect == DesktopSyncCarryEffect::OwnRecord ? proposedFileId : 0 }) : std::nullopt;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarryIo::Permit(
		const QString &slot, const QJsonObject &entry, const QJsonObject &transport,
		const DesktopSyncCarryFresh &fresh, std::shared_ptr<std::atomic<bool>> active) {
	if (!Fresh(fresh) || !DesktopSyncCarryTransportEntry(entry)
		|| (_store._journal->mode == SyncCarryMode::Switch
			&& _store._journal->markText != DesktopSyncChatMarkText())) { return {}; }
	const auto &journal = *_store._journal;
	const auto kind = entry.value(u"kind"_q).toString();
	const auto mark = kind == u"mark"_q;
	const auto pending = kind == u"pending"_q;
	if (pending ? !PendingFresh(fresh) : mark ? (journal.phase != SyncCarryPhase::Mark || !journal.markAttempted || !journal.destinationVerified)
		: (journal.phase != SyncCarryPhase::VerifyDestination || !journal.plan)) { return {}; }
	if (!mark && !pending) {
		auto delivery = journal;
		delivery.phase = SyncCarryPhase::Deliver;
		if (!SerializeSyncCarryJournal(delivery)) { return {}; }
		const auto entries = transport.value(u"entries"_q).toObject();
		const auto reserved = [&](const QString &kind, int32_t id, const QString &hash) {
			const auto key = SyncCarryRecordHash((kind + u":"_q + QString::number(id) + u":"_q + hash).toUtf8());
			const auto saved = entries.value(key).toObject();
			return DesktopSyncCarryTransportEntry(saved) && saved.value(u"kind"_q).toString() == kind
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
	if (pending ? (hash != _store._envelope.value(u"opening_pending_sha256"_q).toString() || !_store.Pending() || SyncCarryRecordHash(_store.Pending().staged) != hash)
		: mark ? (hash != SyncCarryRecordHash(journal.markText.toUtf8()) || DesktopSyncCarryTransportId(journal.markRequestId) != DesktopSyncCarryTransportId(entry.value(u"random_id"_q)))
		: kind == u"own"_q ? (hash != journal.plan->ownRecordHash || DesktopSyncCarryTransportId(journal.ownRequestId) != DesktopSyncCarryTransportId(entry.value(u"random_id"_q)))
		: (!ExactForward(journal, fresh, hash, entry.value(u"source_id"_q).toInt()))) { return {}; }
	auto permit = DesktopSyncCarryPermit();
	permit._operation = journal.operationId; permit._revision = journal.revision;
	permit._effect = pending ? DesktopSyncCarryEffect::Pending : mark ? DesktopSyncCarryEffect::Mark : kind == u"own"_q ? DesktopSyncCarryEffect::OwnRecord : DesktopSyncCarryEffect::Forward;
	permit._identity = {slot, *DesktopSyncCarryTransportId(entry.value(u"random_id"_q)), DesktopSyncCarryTransportId(entry.value(u"file_id"_q)).value_or(0)};
	if (kind == u"own"_q) { permit._content = journal.plan->ownRecord; }
	if (pending) { permit._content = _store.Pending().staged; }
	permit._sourceId = entry.value(u"source_id"_q).toInt(); permit._hash = hash; permit._mark = journal.markText;
	permit._source = fresh.source; permit._target = pending ? fresh.source : fresh.target; permit._current = fresh.current;
	permit._epoch = _store._ioEpoch; permit._openingEpoch = permit._epoch->load(); permit._active = std::move(active);
	if (permit._effect == DesktopSyncCarryEffect::OwnRecord || pending) {
		permit._uploadClaim = std::make_shared<std::atomic<bool>>(false);
	}
	permit._readerWarning = transport.value(pending ? u"pending_reader_warning"_q : u"reader_warning"_q).toBool();
	return permit;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarryIo::Authorize(const QString &slot, const DesktopSyncCarryFresh &fresh) {
	if (!Fresh(fresh)) { return {}; }
	auto transport = Transport();
	if (!transport || !Readers(*transport, fresh)) { return {}; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(slot).toObject();
	if (!DesktopSyncCarryTransportEntry(entry) || entry.value(u"state"_q).toString() != u"prepared"_q) { return {}; }
	if (!ReceiptCapacity(_store._envelope,*transport)) { return {}; }
	auto active = std::make_shared<std::atomic<bool>>(true);
	const auto permit = Permit(slot, entry, *transport, fresh, active);
	if (!permit) { return {}; }
	entry.insert(u"state"_q, u"issued"_q); entries.insert(slot, entry); transport->insert(u"entries"_q, entries);
	if (!SaveGeneration(slot,entry,*transport)) { active->store(false); return {}; }
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
	if (permit) { permit->_uploadClaim = opening._uploadClaim; }
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
	if (!DesktopSyncCarryTransportEntry(entry) || entry.value(u"state"_q).toString() != u"issued"_q
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
	if (!DesktopSyncCarryTransportEntry(entry) || entry.value(u"state"_q).toString() != u"issued"_q) { return false; }
	entry.insert(u"state"_q, u"unknown"_q); entries.insert(permit._identity.slot, entry); transport->insert(u"entries"_q, entries);
	permit._active->store(false);
	return SaveGeneration(permit._identity.slot,entry,*transport);
}

DesktopSyncCarryReconcile DesktopSyncChatCarryIo::Reconcile(const QString &slot, const DesktopSyncChatInventory &input, const DesktopSyncCarryFresh &fresh) {
	if (!Context(fresh) || input.route.peer.origin != fresh.target.peer.origin
		|| input.route.peer.chat != _store._journal->target) { return DesktopSyncCarryReconcile::Held; }
	const auto inventory = DesktopSyncChatCarryInventory(input);
	auto transport = Transport();
	if (!inventory || !transport || !ReadFresh(slot,input)) { return DesktopSyncCarryReconcile::Held; }
	auto entries = transport->value(u"entries"_q).toObject();
	auto entry = entries.value(slot).toObject();
	if (!DesktopSyncCarryTransportEntry(entry) || entry.value(u"kind"_q).toString() == u"mark"_q) { return DesktopSyncCarryReconcile::Held; }
	if (const auto active = _store._ioActive[slot].second.lock(); active && active->load()
		&& _store._ioActive[slot].first == _store._ioEpoch->load()) { return DesktopSyncCarryReconcile::Held; }
	const auto found = std::any_of(inventory->messages.begin(), inventory->messages.end(), [&](const auto &message) { return message.reference.recordHash == entry.value(u"hash"_q).toString(); });
	entry.insert(u"state"_q, found ? u"verified"_q : u"prepared"_q); entries.insert(slot, entry); transport->insert(u"entries"_q, entries);
	return SaveGeneration(slot,entry,*transport) ? (found ? DesktopSyncCarryReconcile::Present : DesktopSyncCarryReconcile::Absent) : DesktopSyncCarryReconcile::Held;
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
	if (!DesktopSyncCarryTransportEntry(entry) || entry.value(u"kind"_q).toString() != u"mark"_q
		|| entry.value(u"ack_id"_q).toInt() != id) { return DesktopSyncCarryReconcile::Held; }
	if (const auto active = _store._ioActive[slot].second.lock(); active && active->load()
		&& _store._ioActive[slot].first == _store._ioEpoch->load()) { return DesktopSyncCarryReconcile::Held; }
	entry.insert(u"state"_q, u"verified"_q); entries.insert(slot, entry); transport->insert(u"entries"_q, entries);
	return Save(*transport) ? DesktopSyncCarryReconcile::Present : DesktopSyncCarryReconcile::Held;
}

bool DesktopSyncChatCarryIo::ReconcileAll(const DesktopSyncChatInventory &inventory, const DesktopSyncCarryFresh &fresh) {
	const auto transport = Transport();
	if (!transport) { return false; }
	const auto entries = transport->value(u"entries"_q).toObject();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		if (i.value().toObject().value(u"kind"_q).toString() == u"mark"_q
			|| i.value().toObject().value(u"kind"_q).toString() == u"pending"_q) { continue; }
		if (Reconcile(i.key(), inventory, fresh) == DesktopSyncCarryReconcile::Held) { return false; }
	}
	return true;
}

int32_t DesktopSyncChatCarryIo::MarkReceiptId() const {
	const auto transport = Transport();
	if (!transport) { return 0; }
	const auto entries = transport->value(u"entries"_q).toObject();
	for (const auto &entry : entries) {
		const auto value = entry.toObject();
		if (value.value(u"kind"_q).toString() == u"mark"_q) { return value.value(u"ack_id"_q).toInt(); }
	}
	return 0;
}

bool DesktopSyncChatCarryIo::RetainedMarkReceipt(const DesktopSyncCarryFresh &fresh, const MTPUpdates &updates) {
	if (!Context(fresh) || !_store._journal->markAttempted) { return false; }
	const auto random = DesktopSyncCarryTransportId(_store._journal->markRequestId);
	const auto id = random ? DesktopSyncChatUploadReceipt(fresh.target, updates, *random) : std::nullopt;
	auto transport = Transport();
	if (!id || !transport) { return false; }
	auto entries = transport->value(u"entries"_q).toObject();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		auto entry = i.value().toObject();
		if (entry.value(u"kind"_q).toString() != u"mark"_q) { continue; }
		if (const auto active = _store._ioActive[i.key()].second.lock(); active && active->load()
			&& _store._ioActive[i.key()].first == _store._ioEpoch->load()) { return false; }
		if (entry.contains(u"ack_id"_q) && entry.value(u"ack_id"_q).toInt() != *id) { return false; }
		entry.insert(u"ack_id"_q, *id); i.value() = entry; transport->insert(u"entries"_q, entries);
		return Save(*transport);
	}
	return false;
}

}
