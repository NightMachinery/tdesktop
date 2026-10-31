#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"
#include "purple/purple_sync_chat_setup.h"

#include <QtCore/QJsonArray>
#include <random>
#include <set>

#undef CHECK
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::fprintf(stderr, "MOVE FAIL seed=%u case=%s line=%d: %s\n", MoveSeed, qPrintable(MoveCase), __LINE__, #value); } } while (false)

namespace {

uint32_t MoveSeed = 0;
QString MoveCase;
QString MoveDirectory;
std::mt19937 MoveRandom;
int MoveSerial = 0;
int MoveWindows = 0;
int MoveRaces = 0;
const auto MoveOld = PeerToken{PeerTokenKind::Channel, 100};
const auto MoveFirst = PeerToken{PeerTokenKind::Channel, 200};
const auto MoveSecond = PeerToken{PeerTokenKind::Channel, 300};

void MoveRequire(bool value, const char *description, int line = __builtin_LINE()) {
	CHECK(value);
	if (!value) {
		std::fprintf(stderr, "MOVE REQUIRED seed=%u case=%s line=%d: %s\n",
			MoveSeed, qPrintable(MoveCase), line, description);
		throw std::runtime_error(description);
	}
}

QString MoveToken(PeerToken peer) {
	return PeerTokenText(peer).value_or(QString());
}

struct MoveMessage {
	int id = 0;
	int date = 0;
	QByteArray bytes;
	bool mark = false;
	QString sentinel;
};

struct MoveCloud {
	std::map<QString, std::vector<MoveMessage>> chats;
	std::set<QString> requests;
	std::vector<std::pair<PeerToken, QString>> origins;
	int sends = 0;
	int forwards = 0;
	int unavailableSourceReads = 0;
	std::map<uint64_t, std::pair<uint64_t, QByteArray>> uploads;
	int marks = 0;
	int deletes = 0;
	int markDate = 500;
	bool gone = false;

	MoveCloud();
	std::vector<MoveMessage> &rows(PeerToken peer);
	std::optional<MoveMessage> find(PeerToken peer, int id);
	int next(PeerToken peer);
	MTPMessage message(const DesktopSyncChatIo &route, const MoveMessage &value);
	DesktopSyncChatInventory inventory(const DesktopSyncChatIo &route);
	SyncChatResolveInput resolution(const SyncLocalState &state, const DesktopSyncChatCacheRecord &cache);
	MTPUpdates effect(const DesktopSyncCarryCommand &command, const DesktopSyncCarryPermit &permit);
	void erase(const DesktopSyncCleanupPermit &permit);
	void sentinels();
	void add(PeerToken peer, MoveMessage value);
	bool contains(PeerToken peer, const QString &hash);
	void conserved(PeerToken winner);
};

MoveCloud::MoveCloud() {
	for (const auto peer : {MoveOld, MoveFirst, MoveSecond}) {
		rows(peer).push_back({10, 50, {}, false, u"ordinary"_q});
		rows(peer).push_back({11, 50, {}, false, u"settings.toml"_q});
	}
	rows(MoveOld).push_back({90, 100, {}, true, {}});
}

std::vector<MoveMessage> &MoveCloud::rows(PeerToken peer) {
	return chats[MoveToken(peer)];
}

std::optional<MoveMessage> MoveCloud::find(PeerToken peer, int id) {
	for (const auto &value : rows(peer)) {
		if (value.id == id) { return value; }
	}
	return {};
}

int MoveCloud::next(PeerToken peer) {
	auto id = 100;
	for (const auto &value : rows(peer)) { id = std::max(id, value.id + 1); }
	return id;
}

MTPMessage MoveCloud::message(const DesktopSyncChatIo &route, const MoveMessage &value) {
	if (!value.bytes.isEmpty()) { return CarryDocument(route, value.bytes, value.id, value.date); }
	auto native = NativeMessage();
	native.flags = MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_out);
	native.id = MTP_int(value.id);
	native.date = MTP_int(value.date);
	native.peer_id = Target(route.peer.chat);
	native.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
	native.message = MTP_string(value.mark ? DesktopSyncChatMarkText() : value.sentinel);
	if (value.sentinel == u"settings.toml"_q) {
		auto document = NativeDocument();
		document.id = MTP_long(98); document.access_hash = MTP_long(789);
		document.dc_id = MTP_int(2); document.size = MTP_long(16);
		document.attributes = MTP_vector<MTPDocumentAttribute>({MTP_documentAttributeFilename(MTP_string("settings.toml"))});
		auto media = NativeMessageMediaDocument();
		media.flags = MTP_flags(MTPDmessageMediaDocument::Flag::f_document); media.document = document.Build();
		native.flags = MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_media);
		native.media = media.Build();
	}
	return native.Build();
}

void MoveCloud::add(PeerToken peer, MoveMessage value) {
	if (!value.bytes.isEmpty()) { origins.emplace_back(peer, SyncCarryRecordHash(value.bytes)); }
	rows(peer).push_back(std::move(value));
}

bool MoveCloud::contains(PeerToken peer, const QString &hash) {
	return std::any_of(rows(peer).begin(), rows(peer).end(), [&](const auto &row) {
		return !row.bytes.isEmpty() && SyncCarryRecordHash(row.bytes) == hash;
	});
}

void MoveCloud::conserved(PeerToken winner) {
	CHECK(!origins.empty());
	for (const auto &[origin, hash] : origins) {
		CHECK(contains(origin, hash) || contains(winner, hash));
	}
}

DesktopSyncChatInventory MoveCloud::inventory(const DesktopSyncChatIo &route) {
	if (gone && route.peer.chat == MoveOld) {
		++unavailableSourceReads;
		auto result = InventoryRows(route, {});
		result.inventory.scan.status = SyncHistoryScanStatus::InvalidResponse;
		return result;
	}
	auto receipts = std::vector<DesktopSyncChatRecordReceipt>();
	for (const auto &value : rows(route.peer.chat)) {
		const auto candidate = IsSyncHistoryCandidate(message(route, value));
		CHECK(candidate == !value.bytes.isEmpty());
		if (candidate) { receipts.push_back(Receipt(route, value.bytes, value.id, value.date)); }
	}
	std::shuffle(receipts.begin(), receipts.end(), MoveRandom);
	return InventoryRows(route, receipts);
}

SyncChatResolveInput MoveCloud::resolution(const SyncLocalState &state,
		const DesktopSyncChatCacheRecord &cache) {
	auto input = SyncChatResolveInput();
	input.selfUserId = Origin.account.userId;
	input.settingsBound = true;
	input.settingsChat = state.chat;
	input.cache = cache.mark;
	input.cacheRead = SyncChatDirectRead::Valid;
	input.readerWarningAccepted = true;
	input.cachedEligibility = *DesktopSyncChatEligibility(Route(cache.mark.chat).peer);
	for (const auto &[token, messages] : chats) {
		for (const auto &value : messages) {
			if (!value.mark) { continue; }
			const auto peer = *ParsePeerToken(token);
			const auto present = std::any_of(messages.begin(), messages.end(), [](const auto &row) {
				return !row.bytes.isEmpty();
			});
			input.marks.push_back({{peer, value.id, value.date}, SyncChatMarkStatus::Supported,
				*DesktopSyncChatEligibility(Route(peer).peer),
				present ? SyncChatRecordPresence::Present : SyncChatRecordPresence::Empty, true, true});
		}
	}
	std::shuffle(input.marks.begin(), input.marks.end(), MoveRandom);
	return input;
}

MTPUpdates MoveCloud::effect(const DesktopSyncCarryCommand &command,
		const DesktopSyncCarryPermit &permit) {
	MoveRequire(permit.current(), "live sealed effect");
	const auto peer = permit.target().peer.chat;
	const auto key = MoveToken(peer) + u':' + QString::number(permit.randomId());
	MoveRequire(requests.emplace(key).second, "no repeated send, including server-deduplicated sends");
	const auto id = next(peer);
	if (command.action == SyncCarryJournalAction::PostMark) {
		MoveRequire(DesktopSyncCarryMarkRequest(permit).has_value(), "real mark request");
		rows(peer).push_back({id, markDate, {}, true, {}});
		++marks;
	} else {
		auto bytes = permit.uploadContent();
		if (bytes.isEmpty()) {
			const auto request = DesktopSyncCarryForwardRequest(permit);
			MoveRequire(request.has_value(), "real forward request");
			auto wire = mtpBuffer(); request->write(wire);
			auto cursor = wire.constData(); const auto end = cursor + wire.size();
			Read<MTPint>(cursor, end); Read<MTPint>(cursor, end); Read<MTPInputPeer>(cursor, end);
			const auto ids = Read<MTPVector<MTPint>>(cursor, end);
			MoveRequire(ids.v.size() == 1, "one exact forward identity");
			const auto source = find(command.source, ids.v.front().v);
			MoveRequire(source.has_value(), "forward references actual cloud bytes");
			bytes = source->bytes;
			++forwards;
		} else {
			MoveRequire(permit.claimUpload() && permit.uploadMatches(bytes), "single exact upload");
			MoveRequire(permit.fileId() && uploads.emplace(permit.randomId(), std::make_pair(permit.fileId(), bytes)).second, "one upload per durable identity");
		}
		MoveRequire(!bytes.isEmpty(), "nonempty delivered record");
		add(peer, {id, 400, bytes, false, {}});
		++sends;
	}
	return Receipt(permit.target(), permit.randomId(), id);
}

void MoveCloud::erase(const DesktopSyncCleanupPermit &permit) {
	MoveRequire(permit.current() && permit.request().has_value(), "real single-use cleanup request");
	CHECK(!permit.request());
	const auto &reference = permit.item().reference;
	const auto before = find(reference.chat, reference.messageId);
	MoveRequire(before && (!before->bytes.isEmpty() || before->mark), "cleanup never targets a sentinel");
	auto &values = rows(reference.chat);
	values.erase(std::remove_if(values.begin(), values.end(), [&](const auto &row) {
		return row.id == reference.messageId;
	}), values.end());
	++deletes;
}

void MoveCloud::sentinels() {
	for (const auto peer : {MoveOld, MoveFirst, MoveSecond}) {
		CHECK(find(peer, 10) && find(peer, 10)->sentinel == u"ordinary"_q);
		CHECK(find(peer, 11) && find(peer, 11)->sentinel == u"settings.toml"_q);
	}
}

struct MoveInstall {
	MoveCloud &cloud;
	QTemporaryDir temporary;
	QString root;
	SyncChatCarryStoreScope scope;
	PeerToken source;
	PeerToken target;
	SyncCarryMode mode;
	QByteArray own;
	QByteArray desired = "version = 1\n# desired local edit retained\n";
	std::shared_ptr<DesktopSyncCarryGate> gate = std::make_shared<DesktopSyncCarryGate>();
	std::shared_ptr<std::atomic<bool>> admitted;
	std::unique_ptr<DesktopSyncChatCarry> engine;
	DesktopSyncCarryProgress progress;
	std::vector<DesktopSyncCarryReply> late;
	bool duplicates = false;
	bool cleanup = true;
	QString access = u"Readable"_q;
	QString commitWindow;
	bool faulted = false;
	int commands = 0;
	int deleteEffects = 0;

	MoveInstall(MoveCloud &cloud, int identity, PeerToken source = MoveOld,
		PeerToken target = MoveFirst, SyncCarryMode mode = SyncCarryMode::Switch,
		bool sourceCopy = true);
	~MoveInstall();
	SyncLocalState state();
	DesktopSyncChatCacheRecord cache();
	QByteArray active();
	DesktopSyncCarryFresh fresh();
	void owner();
	void start(std::optional<DesktopSyncChatMoveDraft> draft = {});
	void restart();
	bool fault(SyncDurablePoint point);
	DesktopSyncCarryReply reply();
	void step();
	void run();
	void stale();
	void ledger();
	void done();
};

MoveInstall::MoveInstall(MoveCloud &cloud, int identity, PeerToken source,
		PeerToken target, SyncCarryMode mode, bool sourceCopy)
: cloud(cloud)
, temporary(MoveDirectory + u"/install-XXXXXX"_q)
, source(source)
, target(target)
, mode(mode) {
	MoveRequire(temporary.isValid(), "private install directory");
	temporary.setAutoRemove(false);
	root = temporary.path() + u"/sync"_q;
	MoveRequire(EnsureSyncPrivateDirectory(root) == SyncStoreStatus::Ready, "private sync root");
	auto initial = SyncLocalState();
	initial.version = 2;
	initial.chat = source;
	initial.install = u"in-"_q + QString(QChar('a' + identity)) + QString(25, u'a');
	initial.space = u"sp-"_q + QString(26, u'a');
	initial.createdDevice = u"Synthetic-"_q + QString::number(identity);
	initial.bindingToken = QString(32, QChar('a' + identity));
	scope = {Origin.account, initial.bindingToken.toLatin1(), QString(64, u'a'),
		temporary.path() + u'/' + DesktopSyncChatCacheFileName(Origin.account)};
	auto build = ConfigRecordBuildInput();
	build.text = "version = 1\n";
	build.space = initial.space; build.install = initial.install;
	build.device = initial.createdDevice; build.platform = u"unit"_q;
	build.app = u"Purple unit"_q; build.seq = 1; build.at = 1;
	const auto record = BuildConfigRecord(build);
	MoveRequire(bool(record), "distinct install record"); own = record.canonical;
	{
		auto local = SyncLocalStore(root);
		CHECK(local.Open(true).status == SyncStoreStatus::Uninitialized);
		MoveRequire(bool(local.Initialize(initial)), "initialize state");
		auto data = initial.configData;
		data.pending = InspectConfigPayload(ParseSyncEnvelope(own)).version.key;
		MoveRequire(bool(local.StageConfig(own, data, scope.binding)), "stage own record");
		MoveRequire(bool(local.ConfirmConfigReadBack(own, initial.createdDevice, 20 + identity, source)), "confirm own record");
	}
	if (sourceCopy) { cloud.add(source, {20 + identity, 150, own, false, {}}); }
	if (!cloud.find(source, 90)) { cloud.rows(source).push_back({90, 100, {}, true, {}}); }
	auto cached = DesktopSyncChatCacheRecord();
	cached.account = Origin.account; cached.mark = {source, 90, 100};
	cached.loginBinding = scope.loginAuthority; cached.readerConsent = true;
	cached.readers = DesktopSyncChatEligibility(Route(source).peer)->readers;
	const auto bytes = SerializeDesktopSyncChatCache(cached);
	MoveRequire(bytes && WriteSyncPrivateFile(scope.cacheFile, *bytes) == SyncStoreStatus::Ready, "initial cache file");
	MoveRequire(WriteSyncPrivateFile(temporary.path() + u"/settings.toml"_q, desired) == SyncStoreStatus::Ready, "desired text file");
}

MoveInstall::~MoveInstall() { engine.reset(); }

SyncLocalState MoveInstall::state() {
	const auto parsed = ParseSyncLocalState(CaptureSyncPrivateFile(root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes);
	MoveRequire(bool(parsed), "reopen durable state");
	return parsed.state;
}

DesktopSyncChatCacheRecord MoveInstall::cache() {
	const auto parsed = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(scope.cacheFile, 65536).bytes, scope.account);
	MoveRequire(parsed.record.has_value(), "reopen durable cache");
	return *parsed.record;
}

QByteArray MoveInstall::active() {
	return CaptureSyncPrivateFile(root + u"/carry.json"_q, kSyncCarryJournalStorageLimit).bytes;
}

DesktopSyncCarryFresh MoveInstall::fresh() {
	auto value = DesktopSyncCarryFresh();
	value.source = Route(source); value.target = Route(target);
	value.loginAuthority = scope.loginAuthority;
	value.current = [alive = admitted] { return alive && alive->load(); };
	value.readerConsentAccepted = true; value.consentPeer = value.target.peer;
	value.resolutionInput = cloud.resolution(state(), cache());
	if (access == u"Protected"_q) { value.source.peer.facts->protectedContent = true; }
	if (access == u"Unknown"_q || (access == u"ChatGone"_q && !cloud.gone)) { value.source.peer.facts.reset(); }
	return value;
}

void MoveInstall::owner() {
	engine.reset();
	CHECK(!gate->postingHeld());
	admitted = std::make_shared<std::atomic<bool>>(true);
	progress = {};
	const auto lease = gate->carry();
	MoveRequire(bool(lease), "process-local carry lease");
	engine = std::make_unique<DesktopSyncChatCarry>(root, scope, Origin, admitted, lease,
		[this](DesktopSyncCarryProgress value) { progress = std::move(value); },
		[this](SyncDurablePoint point) { return fault(point); });
}

void MoveInstall::start(std::optional<DesktopSyncChatMoveDraft> draft) {
	owner();
	const auto facts = fresh();
	auto begin = SyncCarryJournalStart();
	begin.operationId = QString::number(++MoveSerial, 16).rightJustified(32, u'0');
	begin.account = scope.account; begin.source = source; begin.target = target; begin.mode = mode;
	begin.state = state(); begin.preferenceToken = scope.binding;
	begin.targetEligibility = *DesktopSyncChatEligibility(facts.target.peer);
	begin.readerConsentAccepted = true; begin.operationConfirmed = true;
	begin.resolution = ResolveSyncChat(facts.resolutionInput);
	begin.markText = DesktopSyncChatMarkText();
	begin.markRequestId = mode == SyncCarryMode::Switch ? QString::number(10000 + MoveSerial * 2) : QString();
	begin.ownRequestId = QString::number(10001 + MoveSerial * 2);
	begin.loginAuthority = DesktopSyncCarryLogin(scope.loginAuthority);
	begin.oldRead = SyncChatDirectRead::Valid; begin.sourceAccess = SyncCarrySourceAccess::Readable;
	auto opening = DesktopSyncCarryOpening{SerializeSyncLocalState(*begin.state).canonical,
		CaptureSyncPrivateFile(scope.cacheFile, 65536).bytes, cleanup && mode == SyncCarryMode::Switch};
	if (draft) {
		CHECK(draft->opening.stateImage == opening.state && draft->opening.cacheImage == opening.cache);
		CHECK(draft->opening.scope.settingsChat == source && draft->selection.peer->chat == target);
		CHECK(draft->selection.readerConsent);
		opening.cleanupSettings = draft->cleanup;
	}
	engine->open(begin, facts.target.peer, u"config"_q, opening);
}

void MoveInstall::restart() {
	const auto oldRoot = root;
	engine.reset();
	faulted = false; commitWindow.clear();
	owner(); engine->open();
	CHECK(root == oldRoot);
}

bool MoveInstall::fault(SyncDurablePoint point) {
	if (commitWindow.isEmpty() || faulted) { return !faulted; }
	const auto journal = InspectSyncCarryJournal(active(), scope.binding);
	if (!journal) { return true; }
	const auto committing = NextSyncCarryJournalAction(*journal) == SyncCarryJournalAction::CommitState;
	const auto before = commitWindow == u"before"_q && committing && point == SyncDurablePoint::BeforeWrite;
	const auto effect = commitWindow == u"effect"_q && committing
		&& point == SyncDurablePoint::AfterRename && state().chat == target;
	const auto durable = commitWindow == u"durable"_q && journal->stateCommitted
		&& point == SyncDurablePoint::BeforeWrite;
	if (before || effect || durable) { faulted = true; return false; }
	return true;
}

DesktopSyncCarryReply MoveInstall::reply() {
	MoveRequire(progress.command.has_value(), "executor command available");
	const auto command = *progress.command;
	CHECK(command.source == source && command.target == target && command.origin == Origin);
	++commands;
	auto value = DesktopSyncCarryReply();
	value.command = command; value.outcome = SyncCarryOutcome::Success;
	value.fresh = fresh();
	if (value.fresh->source.peer.facts && !cloud.gone) { value.source = cloud.inventory(value.fresh->source); }
	value.destination = cloud.inventory(value.fresh->target);
	value.access = access == u"Readable"_q ? SyncCarrySourceAccess::Readable
		: access == u"Protected"_q ? SyncCarrySourceAccess::Protected
		: access == u"ChatGone"_q ? SyncCarrySourceAccess::ChatGone : SyncCarrySourceAccess::Unknown;
	value.oldRead = cloud.gone ? SyncChatDirectRead::ChatGone : SyncChatDirectRead::Unknown;
	if (command.dispatch && command.cleanupPermit) {
		const auto found = cloud.find(source, command.cleanupPermit->item().reference.messageId);
		if (found && !found->bytes.isEmpty()) { CHECK(cloud.contains(target, SyncCarryRecordHash(found->bytes))); }
		cloud.erase(*command.cleanupPermit); ++deleteEffects;
		value.cleanupResult = MTP_messages_affectedMessages(MTP_int(7), MTP_int(1));
	} else if (command.dispatch) {
		for (const auto &permit : command.effects) { value.updates.push_back(cloud.effect(command, permit)); }
	} else if (command.action == SyncCarryJournalAction::ReadMark) {
		value.markId = command.markId;
		const auto mark = cloud.find(target, command.markId);
		value.markResponse = mark ? Response(value.fresh->target, {cloud.message(value.fresh->target, *mark)})
			: Response(value.fresh->target, {});
	} else if (command.cleanupRead) {
		if (!value.fresh->source.peer.facts || cloud.gone) { return value; }
		const auto &item = command.cleanupRead->item;
		const auto found = cloud.find(source, item.reference.messageId);
		if (found) {
			value.cleanupResponse = Response(value.fresh->source, {cloud.message(value.fresh->source, *found)});
			if (!found->bytes.isEmpty()) {
				value.fresh->sourceRecord = Receipt(value.fresh->source, found->bytes, found->id, found->date);
			}
		} else {
			const auto empty = MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),
				MTP_int(item.reference.messageId), Target(source));
			value.cleanupResponse = Response(value.fresh->source, {empty});
		}
	}
	return value;
}

void MoveInstall::step() {
	auto value = reply();
	if (duplicates) { late.push_back(value); }
	engine->complete(value);
	if (duplicates) { engine->complete(value); }
}

void MoveInstall::run() {
	for (auto n = 0; n != 200; ++n) {
		if (progress.command) { step(); }
		else if (!faulted && progress.reason == u"carry-retry-required"_q) { engine->resume(); }
		else { break; }
	}
	CHECK(!progress.command);
}

void MoveInstall::stale() {
	std::shuffle(late.begin(), late.end(), MoveRandom);
	const auto bytes = active();
	const auto before = cloud.sends + cloud.marks + cloud.deletes;
	const auto command = progress.command;
	for (const auto &value : late) { engine->complete(value); }
	CHECK(active() == bytes && cloud.sends + cloud.marks + cloud.deletes == before);
	CHECK(command.has_value() == progress.command.has_value());
	if (command && progress.command) {
		CHECK(command->ticket == progress.command->ticket && command->run == progress.command->run
			&& command->operation == progress.command->operation && command->revision == progress.command->revision);
	}
}

void MoveInstall::ledger() {
	const auto local = state();
	CHECK(!local.ownMessages.empty());
	for (const auto &row : local.ownMessages) {
		const auto found = cloud.find(local.chat, row.messageId);
		CHECK(found && SyncCarryRecordHash(found->bytes) == row.recordHash);
		if (!found) { continue; }
		const auto record = ParseSyncEnvelope(found->bytes);
		MoveRequire(record.header.has_value(), "bound ledger record parses");
		CHECK(row.install == local.install && row.device == local.createdDevice);
		CHECK(record.header->writerInstall == local.install && record.header->writerDevice == local.createdDevice);
		CHECK(row.seq == record.header->seq && row.payloadHash == record.envelope.document.value(u"payload_sha256"_q).toString());
	}
	CHECK(CaptureSyncPrivateFile(temporary.path() + u"/settings.toml"_q, 262144).bytes == desired);
	cloud.sentinels();
}

void MoveInstall::done() {
	if (!progress.terminal) {
		std::fprintf(stderr, "MOVE HOLD seed=%u case=%s reason=%s phase=%d store=%d journal=%d target=%s\n", MoveSeed,
			qPrintable(MoveCase), qPrintable(progress.reason), progress.journal ? int(progress.journal->phase) : -1, int(progress.storeStatus), int(progress.journalStatus), qPrintable(MoveToken(target)));
	}
	MoveRequire(progress.terminal && progress.journal && progress.journal->phase == SyncCarryPhase::Done, "completed durable operation");
	CHECK(state().chat == target && cache().mark.chat == target && !cache().carry);
	CHECK(!gate->postingHeld() && SyncCarryStartupAdmission(root, scope.binding) == SyncStoreStatus::Ready);
	ledger();
}

bool MoveBoundary(const DesktopSyncCarryCommand &command, SyncCarryJournalAction action) {
	if (command.action != action) { return false; }
	if (action == SyncCarryJournalAction::Deliver || action == SyncCarryJournalAction::PostMark) { return command.dispatch; }
	if (action == SyncCarryJournalAction::Cleanup) { return command.dispatch && command.cleanupPermit.has_value(); }
	return true;
}

void MoveTo(MoveInstall &install, SyncCarryJournalAction action) {
	for (auto n = 0; install.progress.command && n != 100; ++n) {
		if (MoveBoundary(*install.progress.command, action)) { return; }
		install.step();
	}
	MoveRequire(false, "requested command boundary reached");
}

void MovePreserved(const QByteArray &before, const QByteArray &after) {
	const auto oldEntries = QJsonDocument::fromJson(before).object().value(u"transport"_q).toObject().value(u"entries"_q).toObject();
	const auto newEntries = QJsonDocument::fromJson(after).object().value(u"transport"_q).toObject().value(u"entries"_q).toObject();
	for (auto i = oldEntries.begin(); i != oldEntries.end(); ++i) {
		const auto old = i.value().toObject(); const auto now = newEntries.value(i.key()).toObject();
		for (const auto &field : {u"kind"_q, u"random_id"_q, u"file_id"_q, u"hash"_q, u"source_id"_q}) {
			CHECK(old.contains(field) && now.contains(field) && old.value(field) == now.value(field));
		}
	}
}

QByteArray MoveImage(MoveInstall &install, const QString &operation) {
	if (!install.active().isEmpty()) { return install.active(); }
	auto store = SyncChatCarryStore(install.root, install.scope);
	MoveRequire(bool(store.Open()), "reopen archived operation");
	const auto images = store.RetainedImages();
	MoveRequire(images.has_value(), "archived wrappers readable");
	for (const auto &image : *images) {
		const auto journal = InspectSyncCarryJournal(image, install.scope.binding);
		if (journal && journal->operationId == operation) { return image; }
	}
	MoveRequire(false, "completed operation archived");
	return {};
}

QJsonObject MoveOwnEntry(const QByteArray &image) {
	const auto entries = QJsonDocument::fromJson(image).object().value(u"transport"_q).toObject()
		.value(u"entries"_q).toObject();
	auto own = QJsonObject();
	for (const auto &entry : entries) {
		if (entry.toObject().value(u"kind"_q).toString() == u"own"_q) {
			MoveRequire(own.isEmpty(), "one own transport slot");
			own = entry.toObject();
		}
	}
	MoveRequire(!own.isEmpty(), "real own transport slot exists");
	CHECK(own.value(u"file_id"_q).toString().toULongLong() != 0);
	CHECK(own.value(u"source_id"_q).toInt() == 0);
	return own;
}

void MoveMatrix(bool uploadOwn) {
	const auto phases = std::vector<std::pair<QString, SyncCarryJournalAction>>{
		{u"Deliver"_q, SyncCarryJournalAction::Deliver},
		{u"VerifyDestination"_q, SyncCarryJournalAction::VerifyDestination},
		{u"PostMark"_q, SyncCarryJournalAction::PostMark},
		{u"ReadMark"_q, SyncCarryJournalAction::ReadMark},
		{u"CommitState"_q, SyncCarryJournalAction::CommitState},
		{u"Cleanup"_q, SyncCarryJournalAction::Cleanup}};
	for (const auto &[name, action] : phases) {
		for (const auto &window : {u"before"_q, u"effect"_q, u"durable"_q}) {
			MoveCase = (uploadOwn ? u"own-upload/"_q : u"forward/"_q) + name + u'/' + window;
			std::printf("MOVE_WINDOW seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
			auto cloud = MoveCloud();
			auto first = MoveInstall(cloud, 0, MoveOld, MoveFirst, SyncCarryMode::Switch, !uploadOwn);
			auto second = MoveInstall(cloud, 1);
			auto third = MoveInstall(cloud, 2);
			first.duplicates = true;
			CHECK(cloud.contains(MoveOld, SyncCarryRecordHash(first.own)) == !uploadOwn);
			CHECK(!cloud.contains(MoveFirst, SyncCarryRecordHash(first.own)));
			if (action == SyncCarryJournalAction::CommitState) { first.commitWindow = window; }
			first.start();
			if (action == SyncCarryJournalAction::CommitState) {
				first.run();
				MoveRequire(first.faulted, "internal commit durability boundary reached");
				CHECK(first.progress.storeStatus == SyncStoreStatus::DurabilityFailed);
				CHECK(first.state().chat == (window == u"before"_q ? MoveOld : MoveFirst));
			} else {
				MoveTo(first, action);
				if (window != u"before"_q) {
					auto value = first.reply(); first.late.push_back(value);
					if (window == u"durable"_q) {
						first.engine->complete(value);
						MoveRequire(first.progress.journal.has_value(), "durable transition publishes journal");
						if (action == SyncCarryJournalAction::Deliver) { CHECK(first.progress.journal->phase == SyncCarryPhase::VerifyDestination); }
						if (action == SyncCarryJournalAction::VerifyDestination) { CHECK(first.progress.journal->destinationVerified); }
						if (action == SyncCarryJournalAction::PostMark) { CHECK(NextSyncCarryJournalAction(*first.progress.journal) == SyncCarryJournalAction::ReadMark); }
						if (action == SyncCarryJournalAction::ReadMark) { CHECK(first.progress.journal->markVerified); }
						if (action == SyncCarryJournalAction::Cleanup) {
							for (auto n = 0; first.progress.command && first.progress.cleanup.confirmed == 0 && n != 10; ++n) { first.step(); }
							CHECK(first.progress.cleanup.confirmed == 1);
							const auto entries = QJsonDocument::fromJson(first.active()).object().value(u"transport"_q).toObject()
								.value(u"cleanup"_q).toObject().value(u"entries"_q).toObject();
							CHECK(std::any_of(entries.begin(), entries.end(), [](const auto &entry) { return entry.toObject().value(u"state"_q).toString() == u"absent"_q; }));
						}
					}
				}
			}
			const auto before = first.active();
			const auto previous = first.progress.journal;
			const auto alreadyDone = first.progress.terminal;
			MoveRequire(previous && previous->plan, "crash boundary has durable plan");
			const auto image = MoveImage(first, previous->operationId);
			const auto ownEntry = uploadOwn ? MoveOwnEntry(image) : QJsonObject();
			if (uploadOwn) {
				MoveRequire(!previous->plan->ownRecord.isEmpty(), "retained-only install requires own upload");
				CHECK(ParseSyncEnvelope(previous->plan->ownRecord).header->seq == 2);
				CHECK(InspectConfigPayload(ParseSyncEnvelope(previous->plan->ownRecord)).text
					== InspectConfigPayload(ParseSyncEnvelope(first.own)).text);
				CHECK(ownEntry.value(u"random_id"_q).toString() == previous->ownRequestId);
				CHECK(ownEntry.value(u"hash"_q).toString() == previous->plan->ownRecordHash);
				if (action == SyncCarryJournalAction::Deliver) {
					CHECK(cloud.uploads.size() == (window == u"before"_q ? 0 : 1));
				}
			}
			first.restart(); first.stale(); first.run(); first.stale();
			if (action == SyncCarryJournalAction::PostMark && window != u"durable"_q) {
				CHECK(!first.progress.terminal && !first.progress.command);
				CHECK(first.progress.journal && first.progress.journal->markAttempted && !first.progress.journal->stateCommitted);
				CHECK(cloud.deletes == 0 && first.state().chat == MoveOld);
			} else if (alreadyDone) {
				CHECK(first.progress.absent && !first.gate->postingHeld()); first.ledger();
			} else { first.done(); }
			if (!before.isEmpty() && !first.active().isEmpty()) { MovePreserved(before, first.active()); }
			if (previous && previous->plan && first.progress.journal && first.progress.journal->plan) {
				CHECK(previous->plan->ownRecord == first.progress.journal->plan->ownRecord);
				CHECK(previous->ownRequestId == first.progress.journal->ownRequestId);
			}
			CHECK(cloud.marks == (action == SyncCarryJournalAction::PostMark && window == u"before"_q ? 0 : 1));
			CHECK(cloud.sends == 3);
			CHECK(cloud.forwards == (uploadOwn ? 2 : 3) && cloud.uploads.size() == (uploadOwn ? 1 : 0));
			if (uploadOwn) {
				const auto reopened = MoveImage(first, previous->operationId);
				const auto journal = InspectSyncCarryJournal(reopened, first.scope.binding);
				MoveRequire(journal && journal->plan, "own plan survives reopen or terminal archival");
				MovePreserved(image, reopened);
				CHECK(journal->plan->ownRecord == previous->plan->ownRecord);
				CHECK(journal->ownRequestId == previous->ownRequestId && first.state().config.seq == 2);
				CHECK(MoveOwnEntry(reopened).value(u"file_id"_q) == ownEntry.value(u"file_id"_q));
				const auto sent = cloud.uploads.find(previous->ownRequestId.toULongLong());
				MoveRequire(sent != cloud.uploads.end(), "expected own request actually uploaded");
				CHECK(sent->second.first == ownEntry.value(u"file_id"_q).toString().toULongLong());
				CHECK(sent->second.second == previous->plan->ownRecord);
				CHECK(cloud.contains(MoveFirst, previous->plan->ownRecordHash));
			}
			if (!uploadOwn || first.state().chat == MoveFirst) { first.ledger(); }
			else {
				const auto held = first.state();
				CHECK(held.chat == MoveOld && held.config.confirmedSeq == 1);
				CHECK(held.ownMessages.size() == 1 && held.ownMessages.front().messageId == 20);
			}
			cloud.conserved(MoveFirst); ++MoveWindows;
		}
	}
}

void MovePump(const std::vector<MoveInstall*> &installs,
		std::optional<SyncCarryJournalAction> barrier = {}) {
	for (auto budget = 0; budget != 2000; ++budget) {
		auto ready = std::vector<MoveInstall*>();
		for (const auto install : installs) {
			if (install->progress.command && (!barrier || !MoveBoundary(*install->progress.command, *barrier))) { ready.push_back(install); }
		}
		if (ready.empty()) { return; }
		ready[MoveRandom() % ready.size()]->step();
	}
	MoveRequire(false, "bounded seeded scheduling");
}

void MoveSimultaneous(bool sameChat) {
	MoveCase = sameChat ? u"simultaneous-same-chat"_q : u"simultaneous-mixed-chats"_q;
	std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
	auto cloud = MoveCloud();
	auto first = MoveInstall(cloud, 0);
	auto second = MoveInstall(cloud, 1);
	auto third = MoveInstall(cloud, 2, MoveOld, sameChat ? MoveFirst : MoveSecond);
	const auto installs = std::vector{&first, &second, &third};
	for (const auto install : installs) { install->duplicates = true; install->start(); }
	MovePump(installs, SyncCarryJournalAction::PostMark);
	auto markIds = std::set<QString>(); auto ownIds = std::set<QString>();
	for (auto index = size_t(0); index != installs.size(); ++index) {
		const auto install = installs[index];
		MoveRequire(install->progress.command && install->progress.command->dispatch, "all installs at mark intent");
		markIds.emplace(install->progress.journal->markRequestId);
		ownIds.emplace(install->progress.journal->ownRequestId);
		cloud.markDate = index == MoveSeed % installs.size() ? 900 : 500 + int(index);
		install->step();
		MoveRequire(install->progress.command && install->progress.command->action == SyncCarryJournalAction::ReadMark, "all marks visible before any commit");
	}
	CHECK(markIds.size() == 3 && ownIds.size() == 3);
	CHECK(cloud.requests.size() == size_t(cloud.sends + cloud.marks));
	const auto winner = ResolveSyncChat(cloud.resolution(first.state(), first.cache())).winner;
	MoveRequire(winner.has_value(), "shared cloud winner");
	for (const auto install : installs) { install->restart(); install->stale(); }
	MovePump(installs);
	auto finished = 0; auto sameChatLosers = 0;
	for (const auto install : installs) {
		install->stale();
		if (install->progress.terminal) {
			++finished; install->done();
			CHECK(install->progress.journal->targetMark == winner);
		} else {
			CHECK(install->progress.journal && !install->progress.journal->stateCommitted);
			CHECK(install->state().chat == MoveOld && install->progress.journal->markAttempted);
			CHECK(install->progress.journal->source == MoveOld && install->progress.journal->target == install->target);
			CHECK(install->deleteEffects == 0);
			sameChatLosers += install->target == winner->chat;
		}
	}
	CHECK(finished == 1 && cloud.marks == 3 && cloud.deletes == 3);
	CHECK(!sameChat || sameChatLosers == 2);
	CHECK(winner->chat == installs[MoveSeed % installs.size()]->target);
	std::printf("MOVE_WINNER seed=%u case=%s install=%u chat=%s mark=%d same-chat-losers=%d\n",
		MoveSeed, qPrintable(MoveCase), unsigned(MoveSeed % installs.size()), qPrintable(MoveToken(winner->chat)), winner->markId, sameChatLosers);
	cloud.conserved(winner->chat);
	++MoveRaces;
}

void MoveChain() {
	MoveCase = u"O-N1-N2-offline-follow"_q;
	std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
	auto cloud = MoveCloud();
	auto first = MoveInstall(cloud, 0);
	auto offline = MoveInstall(cloud, 2, MoveOld, MoveSecond, SyncCarryMode::Follow);
	first.cleanup = false;
	first.start(); first.run(); first.done();
	const auto firstMark = *first.progress.journal->targetMark;
	first.engine.reset();
	auto second = MoveInstall(cloud, 1, MoveFirst, MoveSecond);
	second.cleanup = false;
	auto cached = second.cache(); cached.mark = firstMark;
	const auto cacheBytes = SerializeDesktopSyncChatCache(cached);
	MoveRequire(cacheBytes && WriteSyncPrivateFile(second.scope.cacheFile, *cacheBytes) == SyncStoreStatus::Ready, "second install starts bound to the actual N1 mark");
	cloud.markDate = 1000;
	second.start(); second.run(); second.done(); second.engine.reset();
	auto local = offline.state();
	auto input = ConfigRecordBuildInput();
	input.text = "version = 1\n# offline new settings version\n";
	input.parents = {InspectConfigPayload(ParseSyncEnvelope(offline.own)).version};
	input.space = local.space; input.install = local.install; input.device = local.createdDevice;
	input.platform = u"unit"_q; input.app = u"Purple unit"_q; input.seq = 2; input.at = 1500;
	const auto late = BuildConfigRecord(input); MoveRequire(bool(late), "new late settings version");
	const auto lateHash = SyncCarryRecordHash(late.canonical);
	CHECK(late.canonical != offline.own && !cloud.contains(MoveSecond, lateHash));
	{
		auto store = SyncLocalStore(offline.root); MoveRequire(bool(store.Open(true, offline.scope.binding)), "offline ordinary store");
		auto data = store.state()->configData; data.pending = late.version.key;
		MoveRequire(bool(store.StageConfig(late.canonical, data, offline.scope.binding)), "stage new offline version");
		MoveRequire(bool(store.ConfirmConfigReadBack(late.canonical, local.createdDevice, 999, MoveOld)), "confirm new offline version in O");
	}
	offline.own = late.canonical; cloud.add(MoveOld, {999, 1500, late.canonical, false, {}});
	const auto sends = cloud.sends;
	offline.duplicates = true; offline.start(); MoveTo(offline, SyncCarryJournalAction::VerifyDestination);
	CHECK(cloud.sends == sends + 1 && cloud.contains(MoveSecond, lateHash) && cloud.find(MoveOld, 999));
	MoveRequire(offline.progress.journal && offline.progress.journal->plan, "Follow plan persisted");
	CHECK(std::any_of(offline.progress.journal->plan->forwards.begin(), offline.progress.journal->plan->forwards.end(), [&](const auto &reference) {
		return reference.messageId == 999 && reference.recordHash == lateHash;
	}));
	auto incomplete = offline.reply(); incomplete.destination->inventory.scan.status = SyncHistoryScanStatus::InvalidResponse;
	offline.engine->complete(incomplete);
	CHECK(!offline.progress.command && !offline.progress.terminal && cloud.find(MoveOld, 999) && cloud.deletes == 0);
	offline.restart(); offline.stale(); offline.run(); offline.stale(); offline.done();
	CHECK(cloud.contains(MoveSecond, lateHash));
	CHECK(offline.state().config.seq == 2 && offline.state().config.confirmedSeq == 2);
	cloud.conserved(MoveSecond);
	CHECK(!cloud.find(MoveOld, 999));
	CHECK(cloud.find(MoveOld, 20).has_value());
	CHECK(cloud.deletes == 1 && cloud.marks == 2);
	++MoveRaces;
}

void MovePlanGone() {
	MoveCase = u"Plan-ChatGone-with-readable-O"_q;
	std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
	auto cloud = MoveCloud();
	auto first = MoveInstall(cloud, 0); auto second = MoveInstall(cloud, 1); auto third = MoveInstall(cloud, 2);
	first.start(); MoveTo(first, SyncCarryJournalAction::Plan);
	const auto state = CaptureSyncPrivateFile(first.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes;
	auto reply = first.reply(); reply.access = SyncCarrySourceAccess::ChatGone; reply.oldRead = SyncChatDirectRead::Valid;
	CHECK(reply.fresh->resolutionInput.cacheRead == SyncChatDirectRead::Valid && reply.source->inventory.scan.status == SyncHistoryScanStatus::Complete);
	first.engine->complete(reply);
	std::printf("PLAN_GONE_RESULT seed=%u command=%d terminal=%d reason=%s\n", MoveSeed, bool(first.progress.command), first.progress.terminal, qPrintable(first.progress.reason));
	CHECK(!first.progress.command && !first.progress.terminal && first.progress.journal && !first.progress.journal->plan);
	CHECK(cloud.sends == 0 && cloud.marks == 0 && cloud.deletes == 0);
	CHECK(CaptureSyncPrivateFile(first.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes == state);
	cloud.conserved(MoveFirst); ++MoveRaces;
}

void MovePlanAffirmativeGone() {
	MoveCase = u"Plan-affirmative-ChatGone"_q;
	std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
	auto cloud = MoveCloud();
	auto first = MoveInstall(cloud, 0, MoveOld, MoveFirst, SyncCarryMode::Switch, false);
	cloud.gone = true; first.access = u"ChatGone"_q;
	CHECK(!cloud.contains(MoveOld, SyncCarryRecordHash(first.own)));
	first.start(); MoveTo(first, SyncCarryJournalAction::Plan);
	auto reply = first.reply();
	CHECK(reply.access == SyncCarrySourceAccess::ChatGone && reply.oldRead == SyncChatDirectRead::ChatGone);
	CHECK(!reply.source && reply.destination->inventory.scan.status == SyncHistoryScanStatus::Complete);
	first.engine->complete(reply);
	MoveRequire(first.progress.journal && first.progress.journal->plan, "affirmative loss passes Plan guard");
	const auto plan = *first.progress.journal->plan;
	CHECK(plan.access == SyncCarrySourceAccess::ChatGone && plan.forwards.empty());
	MoveRequire(!plan.ownRecord.isEmpty(), "lost chat requires retained own upload");
	CHECK(InspectConfigPayload(ParseSyncEnvelope(plan.ownRecord)).text
		== InspectConfigPayload(ParseSyncEnvelope(first.own)).text);
	CHECK(ParseSyncEnvelope(plan.ownRecord).header->seq == 2);
	first.restart(); first.run(); first.done();
	MoveRequire(cloud.uploads.size() == 1, "affirmative loss uploads exactly once");
	CHECK(cloud.forwards == 0 && cloud.sends == 1);
	CHECK(cloud.marks == 1 && cloud.deletes == 0 && cloud.unavailableSourceReads == 0);
	CHECK(first.progress.journal->cleanupChatGone && first.state().config.confirmedSeq == 2);
	CHECK(cloud.contains(MoveFirst, plan.ownRecordHash));
	CHECK(cloud.uploads.begin()->second.second == plan.ownRecord);
	cloud.conserved(MoveFirst); ++MoveRaces;
}

void MoveProtectedPlan() {
	for (auto control = 0; control != 3; ++control) {
		MoveCase = control == 0 ? u"Plan-readable-baseline"_q
			: control == 1 ? u"Plan-Protected-agrees"_q : u"Plan-Protected-contradicts"_q;
		std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
		auto cloud = MoveCloud();
		auto first = MoveInstall(cloud, 0); auto second = MoveInstall(cloud, 1); auto third = MoveInstall(cloud, 2);
		const auto old = cloud.rows(MoveOld);
		first.duplicates = true;
		if (control == 1) { first.access = u"Protected"_q; }
		first.start(); MoveTo(first, SyncCarryJournalAction::Plan);
		auto reply = first.reply();
		reply.access = control == 0 ? SyncCarrySourceAccess::Readable : SyncCarrySourceAccess::Protected;
		reply.oldRead = SyncChatDirectRead::Valid;
		MoveRequire(reply.source && reply.fresh && reply.destination, "scoped direct Plan inventories");
		CHECK(reply.source->route.peer.origin == Origin && reply.source->route.peer.chat == MoveOld);
		CHECK(reply.source->inventory.scan.status == SyncHistoryScanStatus::Complete);
		CHECK(reply.source->route.peer.facts->protectedContent == (control == 1));
		CHECK(reply.fresh->source.peer.facts->protectedContent == (control == 1));
		CHECK(ValidDesktopSyncChatReadIo(reply.source->route));
		CHECK(ValidDesktopSyncChatIo(reply.source->route) == (control != 1));
		CHECK(reply.destination->inventory.scan.status == SyncHistoryScanStatus::Complete);
		first.engine->complete(reply);
		MoveRequire(first.progress.journal && first.progress.journal->plan, "Plan accepts scoped source classification");
		const auto plan = *first.progress.journal->plan;
		CHECK(plan.access == (control == 0 ? SyncCarrySourceAccess::Readable : SyncCarrySourceAccess::Protected));
		if (control == 0) {
			CHECK(plan.ownRecord.isEmpty() && plan.forwards.size() == 3 && plan.cleanupCandidates.size() == 3);
		} else {
			MoveRequire(!plan.ownRecord.isEmpty(), "Protected plan retains this install's own base");
			CHECK(plan.forwards.empty() && plan.cleanupCandidates.empty());
			const auto initial = InspectConfigPayload(ParseSyncEnvelope(first.own));
			const auto retained = InspectConfigPayload(ParseSyncEnvelope(plan.ownRecord));
			CHECK(retained.text == initial.text && retained.version.key == initial.version.key);
			CHECK(plan.ownRecord != first.own && ParseSyncEnvelope(plan.ownRecord).header->seq == 2);
		}
		MoveTo(first, SyncCarryJournalAction::Deliver);
		const auto before = first.active();
		const auto ownRequest = first.progress.journal->ownRequestId;
		auto delivered = first.reply(); first.late.push_back(delivered);
		first.restart(); first.stale(); first.run(); first.done(); first.stale();
		const auto after = MoveImage(first, first.progress.journal->operationId);
		MovePreserved(before, after);
		CHECK(cloud.marks == 1);
		if (control == 0) {
			CHECK(cloud.sends == 3 && cloud.forwards == 3 && cloud.uploads.empty() && cloud.deletes == 3);
			CHECK(first.state().config.confirmedSeq == 1);
		} else {
			MoveRequire(cloud.uploads.size() == 1, "one own upload across unacknowledged delivery and restart");
			const auto upload = cloud.uploads.find(ownRequest.toULongLong());
			MoveRequire(upload != cloud.uploads.end(), "own upload uses its durable request identity");
			CHECK(upload->second.second == plan.ownRecord);
			CHECK(upload->second.first == MoveOwnEntry(after).value(u"file_id"_q).toString().toULongLong());
			CHECK(cloud.sends == 1 && cloud.forwards == 0 && cloud.deletes == 0);
			CHECK(first.state().config.confirmedSeq == 2);
			CHECK(cloud.rows(MoveOld).size() == old.size());
			for (const auto &row : old) {
				const auto retained = cloud.find(MoveOld, row.id);
				CHECK(retained && retained->bytes == row.bytes && retained->mark == row.mark && retained->sentinel == row.sentinel);
			}
			CHECK(cloud.contains(MoveFirst, plan.ownRecordHash));
		}
		std::printf("PLAN_PROTECTED_RESULT seed=%u case=%s sends=%d uploads=%zu forwards=%d deletes=%d confirmed=%llu\n",
			MoveSeed, qPrintable(MoveCase), cloud.sends, cloud.uploads.size(), cloud.forwards, cloud.deletes,
			static_cast<unsigned long long>(first.state().config.confirmedSeq));
		cloud.conserved(MoveFirst); ++MoveRaces;
	}
}

void MoveGone() {
	for (const auto &evidence : {u"Unknown"_q, u"Protected"_q, u"ChatGone"_q}) {
		MoveCase = u"nonaffirmative-"_q + evidence;
		std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
		auto cloud = MoveCloud();
		auto first = MoveInstall(cloud, 0);
		auto second = MoveInstall(cloud, 1);
		auto third = MoveInstall(cloud, 2);
		first.start(); MoveTo(first, SyncCarryJournalAction::Cleanup);
		first.restart(); first.access = evidence; first.run();
		CHECK(!first.progress.terminal && cloud.deletes == 0);
		CHECK(cloud.find(MoveOld, 20) && cloud.find(MoveOld, 21) && cloud.find(MoveOld, 22));
		first.engine.reset(); cloud.gone = true; first.access = u"ChatGone"_q;
		first.restart(); first.run(); first.done();
		CHECK(first.progress.journal->cleanupChatGone && cloud.deletes == 0);
		cloud.conserved(MoveFirst);
		++MoveRaces;
	}
}

}

#include "purple/tests/sync_chat_move_file_cases.cpp"

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	std::setbuf(stdout, nullptr);
	if (argc != 4) { std::fprintf(stderr, "usage: move seed fixture-directory matrix|races|all\n"); return 2; }
	MoveSeed = QString::fromLatin1(argv[1]).toUInt(); MoveRandom.seed(MoveSeed);
	MoveDirectory = QString::fromLocal8Bit(argv[2]);
	MoveRequire(QDir().mkpath(MoveDirectory), "fixture parent directory");
	const auto scenario = QString::fromLatin1(argv[3]);
	auto worker = std::thread([&] {
		try {
			if (scenario != u"races"_q) { MoveMatrix(false); MoveMatrix(true); }
			if (scenario != u"matrix"_q) { MoveSimultaneous(false); MoveSimultaneous(true); MoveChain(); MoveGone(); MoveConsecutive(); MovePlanGone(); MovePlanAffirmativeGone(); MoveProtectedPlan(); }
		} catch (const std::exception &error) {
			std::fprintf(stderr, "MOVE EXCEPTION seed=%u case=%s: %s\n", MoveSeed, qPrintable(MoveCase), error.what());
			++Failures;
		}
		QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
	});
	app.exec();
	worker.join();
	std::printf("MOVE seed=%u %d checks, %d failures; %d crash windows, %d race cases\n",
		MoveSeed, Checks, Failures, MoveWindows, MoveRaces);
	return Failures ? 1 : 0;
}
