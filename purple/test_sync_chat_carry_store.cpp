#include "purple/purple_sync_chat_carry_store.h"
#include "purple/purple_sync_chat_cache.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Purple;

namespace {

auto Checks = 0;
auto Failures = 0;

bool Check(bool good, const char *text, int line) {
	++Checks;
	if (!good) { ++Failures; std::printf("FAIL %d %s\n", line, text); }
	return good;
}

#define CHECK(value) Check(bool(value), #value, __LINE__)
#define REQUIRE(value) do { if (!CHECK(value)) { std::exit(1); } } while (false)

const auto Account = AccountKey{ 0, 777 };

PeerToken Target() { return *ParsePeerToken(u"chat:20"_q); }

SyncLocalState Initial(int version = 1) {
	auto state = SyncLocalState();
	state.version = version;
	state.install = *FormatSyncInstallId(QByteArray(16, 'a'));
	state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
	state.createdDevice = u"synthetic"_q;
	state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 'b'));
	state.chat = version == 1 ? PeerToken{} : *ParsePeerToken(u"chat:10"_q);
	state.preserved.insert(u"future"_q, QJsonObject{ { u"kept"_q, true } });
	REQUIRE(SerializeSyncLocalState(state));
	return state;
}

ConfigRecordBuildResult Record(const SyncLocalState &state, int revision = 1) {
	auto input = ConfigRecordBuildInput();
	input.text = "version = 2\n# revision " + QByteArray::number(revision) + '\n';
	input.space = state.space; input.install = state.install;
	input.device = state.createdDevice; input.platform = u"test"_q;
	input.app = u"T5a"_q; input.seq = state.config.seq + 1; input.at = 1000;
	const auto built = BuildConfigRecord(input); REQUIRE(built); return built;
}

struct Files {
	QTemporaryDir temporary;
	QString root;
	SyncLocalState initial;
	SyncChatCarryStoreScope scope;

	explicit Files(bool absent = false) : root(temporary.path() + u"/sync"_q), initial(Initial()) {
		REQUIRE(temporary.isValid());
		REQUIRE(EnsureSyncPrivateDirectory(root) == SyncStoreStatus::Ready);
		scope = { Account, initial.bindingToken.toLatin1(), QString(64, u'a'),
			temporary.path() + u'/' + DesktopSyncChatCacheFileName(Account) };
		if (!absent) {
			auto store = SyncLocalStore(root);
			REQUIRE(store.Open(true).status == SyncStoreStatus::Uninitialized);
			REQUIRE(store.Initialize(initial));
		}
	}

	QByteArray bytes(const QString &name) const {
		auto result = QByteArray();
		REQUIRE(ReadSyncPrivateFile(root + u'/' + name, 4 * 1024 * 1024, result) == SyncStoreStatus::Ready);
		return result;
	}

	SyncCarryJournalStart start(const std::optional<SyncLocalState> &state) const {
		auto result = SyncCarryJournalStart();
		result.operationId = *FormatSyncBindingToken(QByteArray(16, 'o'));
		result.account = Account; result.state = state; result.stateAbsentConfirmed = !state;
		result.preferenceToken = scope.binding;
		result.source = state ? state->chat : PeerToken{}; result.target = Target();
		result.resolution.action = SyncChatAction::Use;
		result.resolution.cache = SyncChatCache{ result.source, 0, 0 };
		result.resolution.winner = result.resolution.cache;
		auto facts = SyncChatFacts(); facts.kind = SyncChatKind::BasicGroup;
		facts.canSendFiles = true; facts.otherMembers = 0; facts.otherAdmins = 0;
		result.targetEligibility = SyncChatEligibility(facts, SyncChatBroadcastOwnership::Unselected);
		result.readerConsentAccepted = true; result.operationConfirmed = true;
		result.markText = u"Synthetic\n#purplechatsync v1"_q;
		result.markRequestId = u"41"_q; result.ownRequestId = u"42"_q;
		return result;
	}
};

void Retention() {
	for (const auto version : { 1, 2 }) {
		auto state = Initial(version);
		const auto built = Record(state);
		REQUIRE(RetainSyncConfigRecord(state, built.canonical) == SyncStoreStatus::Ready);
		state.configData.base = built.version.key;
		state.configData.baseLineage = built.version.lineage;
		REQUIRE(ReadSyncRetainedConfigBase(state).staged == built.canonical);
		const auto encoded = SerializeSyncLocalState(state); REQUIRE(encoded);
		const auto parsed = ParseSyncLocalState(encoded.canonical); REQUIRE(parsed);
		CHECK(parsed.state.version == version);
		CHECK(parsed.state.preserved.value(u"future"_q).toObject().value(u"kept"_q).toBool());
		CHECK(ReadSyncRetainedConfigBase(parsed.state).staged == built.canonical);
		auto retained = state.preserved.value(u"retained_config_bases"_q).toObject();
		retained.insert(u"container-future"_q, 7);
		auto rows = retained.value(u"records"_q).toArray();
		auto row = rows[0].toObject(); row.insert(u"entry-future"_q, true); rows[0] = row;
		retained.insert(u"records"_q, rows); state.preserved.insert(u"retained_config_bases"_q, retained);
		CHECK(RetainSyncConfigRecord(state, built.canonical) == SyncStoreStatus::Ready);
		CHECK(state.preserved.value(u"retained_config_bases"_q).toObject() == retained);
		for (const auto future : { 2., 2147483648. }) {
			auto changed = retained; changed.insert(u"version"_q, future);
			state.preserved.insert(u"retained_config_bases"_q, changed);
			const auto before = SerializeSyncLocalState(state).canonical;
			CHECK(ValidateSyncRetainedConfig(state) == SyncStoreStatus::NewerRetention);
			CHECK(RetainSyncConfigRecord(state, built.canonical) == SyncStoreStatus::NewerRetention);
			CHECK(SerializeSyncLocalState(state).canonical == before);
		}
	}
	auto state = Initial();
	const auto built = Record(state);
	auto document = ParseSyncEnvelope(built.canonical).envelope;
	document.document.insert(u"padding"_q, QString());
	const auto empty = SerializeSyncEnvelope(document); REQUIRE(empty);
	document.document.insert(u"padding"_q, QString(kSyncSettingsRecordStorageLimit - empty.canonical.size(), u'x'));
	const auto exact = SerializeSyncEnvelope(document); REQUIRE(exact);
	CHECK(exact.canonical.size() == 256 * 1024);
	CHECK(RetainSyncConfigRecord(state, exact.canonical) == SyncStoreStatus::Ready);
	const auto before = SerializeSyncLocalState(state).canonical;
	document.document.insert(u"padding"_q, document.document.value(u"padding"_q).toString() + u'x');
	const auto over = exact.canonical + ' ';
	CHECK(RetainSyncConfigRecord(state, over) == SyncStoreStatus::BaseRecordTooLarge);
	CHECK(SerializeSyncLocalState(state).canonical == before);
	CHECK(RetainSyncConfigRecord(state, QByteArray(262183, ' ')) == SyncStoreStatus::BaseRecordTooLarge);
	for (auto i = 2; i <= kSyncRetainedBaseCountLimit; ++i) {
		CHECK(RetainSyncConfigRecord(state, Record(state, i).canonical) == SyncStoreStatus::Ready);
	}
	CHECK(RetainSyncConfigRecord(state, Record(state, 99).canonical) == SyncStoreStatus::RetentionFull);
	auto total = Initial();
	for (auto i = 0; i < 9; ++i) {
		auto envelope = ParseSyncEnvelope(Record(total, 100 + i).canonical).envelope;
		envelope.document.insert(u"padding"_q, QString());
		const auto size = SerializeSyncEnvelope(envelope).canonical.size();
		envelope.document.insert(u"padding"_q, QString(kSyncSettingsRecordStorageLimit - size, u'x'));
		const auto bytes = SerializeSyncEnvelope(envelope); REQUIRE(bytes);
		CHECK(bytes.canonical.size() == kSyncSettingsRecordStorageLimit);
		const auto expected = i < 8 ? SyncStoreStatus::Ready : SyncStoreStatus::RetentionFull;
		CHECK(RetainSyncConfigRecord(total, bytes.canonical) == expected);
	}
	for (const auto invalid : { QJsonValue(true), QJsonValue(QJsonObject()),
		QJsonValue(QJsonObject{ { u"version"_q, 1 }, { u"records"_q, false } }) }) {
		auto changed = Initial(); changed.preserved.insert(u"retained_config_bases"_q, invalid);
		CHECK(ValidateSyncRetainedConfig(changed) == SyncStoreStatus::InvalidRetention);
	}
}

void Ordinary() {
	auto files = Files();
	const auto built = Record(files.initial);
	{
		auto store = SyncLocalStore(files.root); REQUIRE(store.Open(true, files.scope.binding));
		auto next = store.state()->configData; next.pending = built.version.key;
		REQUIRE(store.StageConfig(built.canonical, next, files.scope.binding));
		REQUIRE(store.ConfirmConfigReadBack(built.canonical, files.initial.createdDevice, 1));
		CHECK(!QFileInfo::exists(files.root + u"/pending/config-1.json"_q));
		CHECK(ReadSyncRetainedConfigBase(*store.state()).staged == built.canonical);
		CHECK(store.state()->version == 1);
	}
	const auto before = files.bytes(u"state.json"_q);
	{
		auto store = SyncLocalStore(files.root); REQUIRE(store.Open(true, files.scope.binding));
		REQUIRE(store.CheckpointRetainedConfigBases() == SyncStoreStatus::Ready);
		CHECK(ReadSyncRetainedConfigBase(*store.state()).staged == built.canonical);
	}
	CHECK(files.bytes(u"state.json"_q) == before);
	const auto hash = SyncCarryRecordHash(built.canonical);
	CHECK(files.bytes(u"retained-bases/"_q + hash + u".json"_q) == built.canonical);
	auto legacy = ParseSyncLocalState(before).state;
	legacy.preserved.remove(u"retained_config_bases"_q);
	REQUIRE(WriteSyncPrivateFile(files.root + u"/state.json"_q, SerializeSyncLocalState(legacy).canonical) == SyncStoreStatus::Ready);
	for (const auto seq : { uint64_t(1), uint64_t(2) }) {
		auto input = ConfigRecordBuildInput();
		input.text = InspectConfigPayload(ParseSyncEnvelope(built.canonical)).text;
		input.version = built.version; input.space = legacy.space; input.install = legacy.install;
		input.device = legacy.createdDevice; input.platform = u"test"_q; input.app = u"T5a"_q; input.seq = seq;
		const auto record = BuildConfigRecord(input); REQUIRE(record);
		const auto path = files.root + u"/pending/config-"_q + QString::number(seq) + u".json"_q;
		REQUIRE(WriteSyncPrivateFile(path, seq == 1 ? built.canonical : record.canonical) == SyncStoreStatus::Ready);
		{
			auto store = SyncLocalStore(files.root);
			CHECK(store.Open(true, files.scope.binding).status == SyncStoreStatus::MissingLocalVersion);
			CHECK(QFileInfo::exists(path));
		}
		REQUIRE(QFile::remove(path));
	}
	{
		auto store = SyncLocalStore(files.root); REQUIRE(store.Open(true, files.scope.binding));
		CHECK(store.CheckpointRetainedConfigBases() == SyncStoreStatus::MissingLocalVersion);
		CHECK(store.SupplyRetainedConfigBase(Record(files.initial, 2).canonical) == SyncStoreStatus::MissingLocalVersion);
		CHECK(store.SupplyRetainedConfigBase(built.canonical) == SyncStoreStatus::Ready);
		CHECK(!ReadSyncRetainedConfigBase(*store.state()));
		REQUIRE(store.CheckpointRetainedConfigBases() == SyncStoreStatus::Ready);
		CHECK(ReadSyncRetainedConfigBase(*store.state()).staged == built.canonical);
	}
	REQUIRE(WriteSyncPrivateFile(files.root + u"/settings.toml"_q, "version = 2\n# unsaved dirty primary\n") == SyncStoreStatus::Ready);
	{
		auto carry = SyncChatCarryStore(files.root, files.scope); REQUIRE(carry.Open());
		CHECK(carry.OwnBase().staged == built.canonical);
		CHECK(InspectConfigPayload(ParseSyncEnvelope(carry.OwnBase().staged)).text != files.bytes(u"settings.toml"_q));
	}
}

void Barriers() {
	for (const auto point : { SyncDurablePoint::BeforeWrite, SyncDurablePoint::PartialWrite,
		SyncDurablePoint::BeforeRename, SyncDurablePoint::AfterRename,
		SyncDurablePoint::FileBarrier, SyncDurablePoint::DirectoryBarrier }) {
		auto files = Files();
		const auto path = files.root + u"/probe"_q;
		REQUIRE(WriteSyncPrivateFile(path, "old") == SyncStoreStatus::Ready);
		const auto old = CaptureSyncPrivateFile(path, 16);
		CHECK(WriteSyncDurableFile(path, "new", old, [=](auto current) { return current != point; }) != SyncStoreStatus::Ready);
		const auto actual = CaptureSyncPrivateFile(path, 16);
		CHECK(actual.status == SyncStoreStatus::Ready);
		CHECK(actual.bytes == (point <= SyncDurablePoint::BeforeRename ? "old" : "new"));
		CHECK(ConfirmSyncDurableFile(path, actual) == SyncStoreStatus::Ready);
	}
	auto files = Files();
	const auto path = files.root + u"/probe"_q;
	const auto missing = CaptureSyncPrivateFile(path, 16); REQUIRE(missing.absent());
	REQUIRE(WriteSyncPrivateFile(path, "substitute") == SyncStoreStatus::Ready);
	CHECK(WriteSyncDurableFile(path, "wrong", missing) == SyncStoreStatus::Changed);
	CHECK(files.bytes(u"probe"_q) == "substitute");
	REQUIRE(QFile::link(path, files.root + u"/link"_q));
	CHECK(CaptureSyncPrivateFile(files.root + u"/link"_q, 16).status != SyncStoreStatus::Ready);
}

void CarryCommit(bool ownStage, int failRename = 0) {
	auto files = Files();
	const auto built = Record(files.initial);
	{
		auto store = SyncLocalStore(files.root); REQUIRE(store.Open(true, files.scope.binding));
		auto next = store.state()->configData; next.pending = built.version.key;
		REQUIRE(store.StageConfig(built.canonical, next, files.scope.binding));
		REQUIRE(store.ConfirmConfigReadBack(built.canonical, files.initial.createdDevice, 1));
	}
	auto armed = false;
	auto renames = 0;
	auto fault = [&](SyncDurablePoint point) {
		return !armed || point != SyncDurablePoint::AfterRename || ++renames != failRename;
	};
	auto carry = SyncChatCarryStore(files.root, files.scope, fault); REQUIRE(carry.Open());
	const auto initial = *carry.state();
	const auto start = files.start(initial); REQUIRE(carry.Begin(start));
	auto event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::Resolution);
	event.resolution = start.resolution; event.outcome = SyncCarryOutcome::Success;
	REQUIRE(carry.Advance(event));
	auto input = SyncCarryInput();
	input.account = Account; input.source = initial.chat; input.target = Target();
	input.access = ownStage ? SyncCarrySourceAccess::ChatGone : SyncCarrySourceAccess::Readable;
	input.state = initial; input.preferenceToken = files.scope.binding;
	input.targetEligibility = start.targetEligibility; input.readerConsentAccepted = true;
	input.oldInventory = { Account, initial.chat, true, {} };
	input.newInventory = { Account, Target(), true, {} };
	const auto old = MakeSyncCarryMessage(Account, initial.chat, 1, 1000,
		built.canonical, SyncChatMessageOwnership::Own); REQUIRE(old);
	if (!ownStage) { input.oldInventory.messages.push_back(*old); }
	input.ownVersion = SyncCarryOwnVersion{ InspectConfigPayload(ParseSyncEnvelope(built.canonical)).text, built.version, u"test"_q, u"T5a"_q, 5000 };
	const auto planned = PlanCarry(input); REQUIRE(planned);
	event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::PlanPrepared); event.plan = planned.plan; event.outcome = SyncCarryOutcome::Success;
	REQUIRE(carry.Advance(event));
	if (ownStage) {
		CHECK(carry.journal()->phase == SyncCarryPhase::StageOwn);
		armed = failRename > 0;
		const auto staged = carry.StageOwn();
		armed = false;
		if (failRename) {
			CHECK(!staged); carry.Close(); REQUIRE(carry.Open()); REQUIRE(carry.Resume());
			if (carry.journal()->phase == SyncCarryPhase::StageOwn) { REQUIRE(carry.StageOwn()); }
			CHECK(carry.state()->config.seq == initial.config.seq + 1);
		} else { REQUIRE(staged); }
		CHECK(carry.journal()->phase == SyncCarryPhase::Deliver
			|| carry.journal()->phase == SyncCarryPhase::VerifyDestination);
	}
	if (carry.journal()->phase == SyncCarryPhase::Deliver) {
		event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::DeliveryPrepared);
		event.outcome = SyncCarryOutcome::Unknown; REQUIRE(carry.Advance(event));
	} else { CHECK(carry.journal()->phase == SyncCarryPhase::VerifyDestination); }
	auto destination = SyncCarryInventory{ Account, Target(), true, {} };
	const auto bytes = ownStage ? carry.journal()->plan->ownRecord : built.canonical;
	const auto posted = MakeSyncCarryMessage(Account, Target(), 1, 6000, bytes, SyncChatMessageOwnership::Own);
	REQUIRE(posted); destination.messages.push_back(*posted);
	event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::DestinationRead);
	event.destination = destination; event.outcome = SyncCarryOutcome::Success;
	REQUIRE(carry.Advance(event)); CHECK(carry.journal()->phase == SyncCarryPhase::Mark);
	event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::MarkPostPrepared);
	event.outcome = SyncCarryOutcome::Unknown; REQUIRE(carry.Advance(event));
	event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::MarkRead);
	event.outcome = SyncCarryOutcome::Success;
	event.mark = { SyncCarryMarkOutcome::Valid, Account, Target(), 2, 7000,
		carry.journal()->markText, false, SyncChatMessageOwnership::Own };
	REQUIRE(carry.Advance(event)); CHECK(carry.journal()->phase == SyncCarryPhase::PrepareCache);
	auto cache = DesktopSyncChatCacheRecord(); cache.account = Account;
	cache.loginBinding = files.scope.loginAuthority; cache.mark = *carry.journal()->targetMark;
	cache.carry = QJsonObject{ { u"operation"_q, carry.journal()->operationId } };
	cache.readerConsent = true;
	const auto preparedCache = SerializeDesktopSyncChatCache(cache); REQUIRE(preparedCache);
	REQUIRE(WriteSyncPrivateFile(files.root + u"/carry-cache.json"_q, *preparedCache) == SyncStoreStatus::Ready);
	event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::CachePrepared);
	event.outcome = SyncCarryOutcome::Success;
	event.cache = { carry.journal()->operationId, Account, cache.mark, false };
	REQUIRE(carry.Advance(event));
	const auto nextHash = carry.journal()->nextStateHash;
	REQUIRE(carry.CommitPreparedState());
	CHECK(SyncCarryRecordHash(files.bytes(u"state.json"_q)) == nextHash);
	CHECK(carry.state()->version == 2 && carry.state()->chat == Target());
	CHECK(InspectConfigPayload(ParseSyncEnvelope(carry.OwnBase().staged)).version.key == built.version.key);
	CHECK(InspectConfigPayload(ParseSyncEnvelope(carry.OwnBase().staged)).version.lineage == built.version.lineage);
	if (ownStage) {
		CHECK(carry.journal()->phase == SyncCarryPhase::RetireOwnStage);
		REQUIRE(carry.RetireStage());
	}
	cache.carry.reset(); const auto completed = SerializeDesktopSyncChatCache(cache); REQUIRE(completed);
	REQUIRE(WriteSyncPrivateFile(files.scope.cacheFile, *completed) == SyncStoreStatus::Ready);
	event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::CacheCompleted);
	event.outcome = SyncCarryOutcome::Success;
	event.cache = { carry.journal()->operationId, Account, cache.mark, true };
	REQUIRE(carry.Advance(event));
	if (carry.journal()->phase == SyncCarryPhase::Cleanup) {
		event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::CleanupCompleted);
		event.destination = destination; event.noRequestInFlight = true; event.outcome = SyncCarryOutcome::Success;
		REQUIRE(carry.Advance(event));
	}
	CHECK(carry.journal()->phase == SyncCarryPhase::Done);
	carry.Close(); REQUIRE(carry.Open()); REQUIRE(carry.Resume());
	CHECK(carry.journal()->phase == SyncCarryPhase::Done);
	CHECK(carry.OwnBase());
}

void PendingRetirement() {
	auto files = Files();
	const auto built = Record(files.initial);
	{
		auto local = SyncLocalStore(files.root); REQUIRE(local.Open(true, files.scope.binding));
		auto next = local.state()->configData; next.pending = built.version.key;
		REQUIRE(local.StageConfig(built.canonical, next, files.scope.binding));
	}
	auto state = ParseSyncLocalState(files.bytes(u"state.json"_q)).state;
	state.preserved.remove(u"retained_config_bases"_q);
	REQUIRE(WriteSyncPrivateFile(files.root + u"/state.json"_q, SerializeSyncLocalState(state).canonical) == SyncStoreStatus::Ready);
	auto armed = false;
	auto directories = 0;
	auto fault = [&](SyncDurablePoint point) {
		return !armed || point != SyncDurablePoint::DirectoryBarrier || ++directories != 3;
	};
	auto carry = SyncChatCarryStore(files.root, files.scope, fault); REQUIRE(carry.Open());
	REQUIRE(carry.Begin(files.start(*carry.state())));
	CHECK(carry.journal()->phase == SyncCarryPhase::ResolvePending);
	auto event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::PendingPrepared);
	event.outcome = SyncCarryOutcome::Success; event.stagedRecord = built.canonical;
	event.pendingReadback = MakeSyncCarryMessage(Account, state.chat, 1, 1000, built.canonical,
		SyncChatMessageOwnership::Own); REQUIRE(event.pendingReadback);
	REQUIRE(carry.Advance(event)); REQUIRE(carry.CommitPreparedState());
	CHECK(carry.journal()->phase == SyncCarryPhase::RetirePendingStage);
	CHECK(carry.state()->config.pendingSeq == 0 && carry.state()->config.confirmedSeq == 1);
	const auto object = files.root + u"/carry-objects/record-"_q + SyncCarryRecordHash(built.canonical) + u".json"_q;
	REQUIRE(QFile::remove(object));
	CHECK(!carry.RetireStage());
	CHECK(QFileInfo::exists(files.root + u"/pending/config-1.json"_q));
	REQUIRE(WriteSyncPrivateFile(object, built.canonical) == SyncStoreStatus::Ready);
	armed = true; CHECK(!carry.RetireStage()); armed = false;
	CHECK(directories == 3);
	CHECK(!QFileInfo::exists(files.root + u"/pending/config-1.json"_q));
	REQUIRE(carry.Open()); REQUIRE(carry.Resume()); REQUIRE(carry.RetireStage());
	CHECK(carry.journal()->phase == SyncCarryPhase::ResolveAgain);
	CHECK(carry.OwnBase().staged == built.canonical);
	CHECK(carry.state()->config.seq == 1);
}

void RefusalMatrix() {
	for (auto mode = 0; mode < 7; ++mode) {
		auto files = Files();
		auto carry = SyncChatCarryStore(files.root, files.scope); REQUIRE(carry.Open());
		REQUIRE(carry.Begin(files.start(*carry.state()))); carry.Close();
		const auto journal = files.bytes(u"carry.json"_q);
		const auto state = files.bytes(u"state.json"_q);
		auto scope = files.scope;
		if (mode == 0) { ++scope.account.userId; }
		if (mode == 1) { scope.binding = QByteArray(32, 'c'); }
		if (mode == 2) { scope.loginAuthority = QString(64, u'c'); }
		if (mode == 3) {
			auto value = QJsonDocument::fromJson(journal).object();
			value.insert(u"journal_sha256"_q, QString(64, u'0'));
			REQUIRE(WriteSyncPrivateFile(files.root + u"/carry.json"_q, QJsonDocument(value).toJson()) == SyncStoreStatus::Ready);
		}
		if (mode == 4) {
			auto value = QJsonDocument::fromJson(journal).object(); value.remove(u"operation"_q);
			REQUIRE(WriteSyncPrivateFile(files.root + u"/carry.json"_q, QJsonDocument(value).toJson()) == SyncStoreStatus::Ready);
		}
		if (mode == 5) {
			const auto object = QJsonDocument::fromJson(journal).object().value(u"objects"_q).toObject();
			REQUIRE(!object.isEmpty());
			const auto hash = object.begin().key();
			REQUIRE(WriteSyncPrivateFile(files.root + u"/carry-objects/state-"_q + hash + u".json"_q,
				"corrupt") == SyncStoreStatus::Ready);
		}
		if (mode == 6) {
			REQUIRE(WriteSyncPrivateFile(files.root + u"/carry.json"_q,
				QByteArray(kSyncCarryJournalStorageLimit + 1, ' ')) == SyncStoreStatus::Ready);
		}
		auto refused = SyncChatCarryStore(files.root, scope);
		const auto result = refused.Open(); CHECK(!result);
		CHECK(files.bytes(u"state.json"_q) == state);
		if (mode < 3 || mode == 5) { CHECK(files.bytes(u"carry.json"_q) == journal); }
		if (mode < 3) { auto valid = SyncChatCarryStore(files.root, files.scope); CHECK(valid.Open()); }
		else { auto local = SyncLocalStore(files.root); CHECK(local.Open(true).status == SyncStoreStatus::CarryPending); }
	}
	{
		auto files = Files();
		REQUIRE(WriteSyncPrivateFile(files.root + u"/pending/orphan"_q, "keep") == SyncStoreStatus::Ready);
		auto carry = SyncChatCarryStore(files.root, files.scope); REQUIRE(carry.Open());
		CHECK(!carry.Begin(files.start(*carry.state())));
		CHECK(files.bytes(u"pending/orphan"_q) == "keep");
		CHECK(!QFileInfo::exists(files.root + u"/carry.json"_q));
	}
	{
		auto files = Files();
		auto carry = SyncChatCarryStore(files.root, files.scope); REQUIRE(carry.Open());
		REQUIRE(carry.Begin(files.start(*carry.state())));
		const auto journal = files.bytes(u"carry.json"_q);
		const auto event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::Resolution);
		const auto moved = files.root + u"-moved"_q;
		REQUIRE(QDir().rename(files.root, moved));
		REQUIRE(EnsureSyncPrivateDirectory(files.root) == SyncStoreStatus::Ready);
		REQUIRE(WriteSyncPrivateFile(files.root + u"/state.json"_q, "replacement") == SyncStoreStatus::Ready);
		CHECK(!carry.Advance(event)); CHECK(files.bytes(u"state.json"_q) == "replacement");
		auto original = QByteArray();
		REQUIRE(ReadSyncPrivateFile(moved + u"/carry.json"_q, kSyncCarryJournalStorageLimit, original) == SyncStoreStatus::Ready);
		CHECK(original == journal); carry.Close();
	}
	{
		auto files = Files(true);
		auto state = files.initial;
		state.preserved.insert(u"retained_config_bases"_q, QJsonObject{ { u"version"_q, 2147483648. } });
		auto local = SyncLocalStore(files.root); REQUIRE(local.Open(true).status == SyncStoreStatus::Uninitialized);
		CHECK(local.Initialize(state).status == SyncStoreStatus::NewerRetention);
		CHECK(!QFileInfo::exists(files.root + u"/state.json"_q));
	}
}

void Journals() {
	for (const auto absent : { false, true }) {
		auto files = Files(absent);
		auto carry = SyncChatCarryStore(files.root, files.scope); REQUIRE(carry.Open());
		const auto state = carry.state() ? std::optional(*carry.state()) : std::nullopt;
		const auto start = files.start(state);
		REQUIRE(carry.Begin(start));
		const auto original = files.bytes(u"carry.json"_q);
		CHECK(carry.journal()->revision == 0);
		{
			auto other = SyncLocalStore(files.root);
			CHECK(other.Open(true).status == SyncStoreStatus::LockBusy);
		}
		auto stale = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::Resolution);
		stale.operationId = *FormatSyncBindingToken(QByteArray(16, 'x'));
		CHECK(!carry.Advance(stale)); CHECK(files.bytes(u"carry.json"_q) == original);
		carry.Close();
		{
			auto other = SyncLocalStore(files.root);
			CHECK(other.Open(true).status == SyncStoreStatus::CarryPending);
		}
		{
			auto wrong = files.scope; wrong.loginAuthority = QString(64, u'b');
			auto foreign = SyncChatCarryStore(files.root, wrong);
			CHECK(foreign.Open().status == SyncStoreStatus::AccountUnbound);
			CHECK(files.bytes(u"carry.json"_q) == original);
		}
		REQUIRE(carry.Open()); REQUIRE(carry.Resume());
		CHECK(carry.journal()->phase == SyncCarryPhase::ResolveAgain);
		auto event = SyncCarryEvent(*carry.journal(), SyncCarryEventKind::Resolution);
		event.resolution = start.resolution; event.outcome = SyncCarryOutcome::Success;
		REQUIRE(carry.Advance(event)); CHECK(carry.journal()->phase == SyncCarryPhase::Plan);
		const auto unchanged = files.bytes(u"carry.json"_q); carry.Close();
		auto object = QJsonDocument::fromJson(unchanged).object(); object.insert(u"future"_q, 7);
		REQUIRE(WriteSyncPrivateFile(files.root + u"/carry.json"_q, QJsonDocument(object).toJson()) == SyncStoreStatus::Ready);
		REQUIRE(carry.Open()); REQUIRE(carry.Resume());
		CHECK(QJsonDocument::fromJson(files.bytes(u"carry.json"_q)).object().value(u"future"_q).toInt() == 7);
		carry.Close();
		for (const auto version : { 2., 2147483648. }) {
			auto future = object; future.insert(u"version"_q, version);
			REQUIRE(WriteSyncPrivateFile(files.root + u"/carry.json"_q, QJsonDocument(future).toJson()) == SyncStoreStatus::Ready);
			const auto exact = files.bytes(u"carry.json"_q);
			CHECK(carry.Open().status == SyncStoreStatus::NewerState);
			CHECK(files.bytes(u"carry.json"_q) == exact);
		}
	}
}

}

int main(int argc, char **argv) {
	Retention(); Ordinary(); Barriers(); Journals(); RefusalMatrix(); PendingRetirement();
	CarryCommit(false); CarryCommit(true);
	for (auto i = 1; i <= 3; ++i) { CarryCommit(true, i); }
	{
		auto app = QCoreApplication(argc, argv);
		CHECK(!SyncStoreWorkerThread());
		auto files = Files(true);
		auto carry = SyncChatCarryStore(files.root, files.scope);
		CHECK(carry.Open().status == SyncStoreStatus::WrongThread);
		auto worker = std::thread([&] { CHECK(SyncStoreWorkerThread()); CHECK(carry.Open()); carry.Close(); });
		worker.join();
	}
	std::printf("T5a %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
