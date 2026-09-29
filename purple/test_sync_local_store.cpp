#include "purple/purple_sync_local_store.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>

#include <cstdio>

namespace {

auto checks = 0;
auto failures = 0;

void Check(bool ok, int line) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("FAIL line %d\n", line);
	}
}

#define CHECK(value) Check(bool(value), __LINE__)

[[nodiscard]] Purple::SyncLocalState InitialState() {
	auto state = Purple::SyncLocalState();
	state.install = u"in-"_q + QString(26, u'a');
	state.space = u"sp-"_q + QString(26, u'a');
	state.createdDevice = u"device-a"_q;
	state.preserved.insert(u"future"_q, true);
	return state;
}

[[nodiscard]] Purple::ConfigRecordBuildResult Record(
		const Purple::SyncLocalState &state,
		uint64_t seq,
		const QByteArray &text) {
	auto input = Purple::ConfigRecordBuildInput();
	input.text = text;
	input.space = state.space;
	input.install = state.install;
	input.device = state.createdDevice;
	input.platform = u"macos"_q;
	input.app = u"Purple"_q;
	input.seq = seq;
	return Purple::BuildConfigRecord(input);
}

[[nodiscard]] Purple::ConfigRecordBuildResult RecordWithVersion(
		const Purple::SyncLocalState &state,
		uint64_t seq,
		const QByteArray &text,
		const Purple::ConfigVersion &version) {
	auto input = Purple::ConfigRecordBuildInput();
	input.text = text;
	input.version = version;
	input.space = state.space;
	input.install = state.install;
	input.device = state.createdDevice;
	input.platform = u"macos"_q;
	input.app = u"Purple"_q;
	input.seq = seq;
	return Purple::BuildConfigRecord(input);
}

[[nodiscard]] Purple::ConfigRecordBuildResult RecordAfter(
		const Purple::SyncLocalState &state,
		uint64_t seq,
		const QByteArray &text,
		const Purple::ConfigVersion &parent) {
	auto input = Purple::ConfigRecordBuildInput();
	input.text = text;
	input.parents = { parent };
	input.space = state.space;
	input.install = state.install;
	input.device = state.createdDevice;
	input.platform = u"macos"_q;
	input.app = u"Purple"_q;
	input.seq = seq;
	return Purple::BuildConfigRecord(input);
}

[[nodiscard]] bool Private(const QString &path) {
#ifdef Q_OS_UNIX
	const auto permissions = QFileInfo(path).permissions();
	constexpr auto broader = QFileDevice::ReadGroup
		| QFileDevice::WriteGroup | QFileDevice::ExeGroup
		| QFileDevice::ReadOther | QFileDevice::WriteOther
		| QFileDevice::ExeOther;
	return !(permissions & broader);
#else
	return true;
#endif
}

[[nodiscard]] QString Root(const QTemporaryDir &dir) {
	return dir.path() + u"/sync"_q;
}

void TestLifecycle() {
	auto temp = QTemporaryDir();
	CHECK(temp.isValid());
	const auto root = Root(temp);
	const auto initial = InitialState();
	const auto record = Record(initial, 1,
		"version = 1\nname = 'staged'\n");
	CHECK(bool(record));
	{
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(false).status == Purple::SyncStoreStatus::Disabled);
		CHECK(!QFileInfo::exists(root));
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Open(false).status
			== Purple::SyncStoreStatus::InvalidTransition);
		CHECK(Private(root));
		CHECK(Private(root + u"/pending"_q));
		auto other = Purple::SyncLocalStore(root);
		CHECK(other.Open(true).status == Purple::SyncStoreStatus::LockBusy);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		CHECK(Private(root + u"/state.json"_q));
		auto nextConfig = initial.configData;
		nextConfig.pending = record.version.key;
		const auto staged = store.StageConfig(record.canonical, nextConfig);
		CHECK(bool(staged));
		CHECK(staged.staged == record.canonical);
		CHECK(staged.seq == 1);
		CHECK(store.state()->config.seq == 1);
		CHECK(store.state()->config.pendingSeq == 1);
		CHECK(store.state()->config.ownHash == record.payloadHash);
		CHECK(Private(root + u"/pending/config-1.json"_q));
		CHECK(Private(root + u"/state.json"_q));
		CHECK(QFileInfo(root + u"/pending/config-1.json"_q).size()
			== record.canonical.size());
		const auto saved = Purple::ParseSyncLocalState([&] {
			auto file = QFile(root + u"/state.json"_q);
			CHECK(file.open(QIODevice::ReadOnly));
			return file.readAll();
		}());
		CHECK(bool(saved));
		CHECK(saved.state.preserved.value(u"future"_q).toBool());
	}
	{
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == Purple::SyncStoreStatus::Ready);
		const auto pending = restarted.ReadPendingConfig();
		CHECK(bool(pending));
		CHECK(pending.staged == record.canonical);
		CHECK(pending.seq == 1);
		CHECK(restarted.state()->configData.pending == record.version.key);
	}
}

void TestConfirmation() {
	auto temp = QTemporaryDir();
	const auto root = Root(temp);
	const auto initial = InitialState();
	const auto first = Record(initial, 1, "version = 1\nname = 'first'\n");
	const auto changed = RecordAfter(initial, 2,
		"version = 1\nname = 'changed'\n", first.version);
	CHECK(bool(first));
	CHECK(bool(changed));
	{
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		auto metadata = initial.configData;
		metadata.pending = first.version.key;
		metadata.seenSeq[u"in-"_q + QString(25, u'b') + u'a'] = 7;
		CHECK(bool(store.StageConfig(first.canonical, metadata)));
		CHECK(store.state()->config.pendingSeq == 1);
		CHECK(store.ConfirmConfigReadBack(first.canonical, u"device-a"_q).status
			== Purple::SyncStoreStatus::Ready);
		CHECK(store.state()->config.pendingSeq == 0);
		CHECK(store.state()->config.confirmedSeq == 1);
		CHECK(store.state()->configData.base == first.version.key);
		CHECK(store.state()->configData.baseLineage == first.version.lineage);
		CHECK(store.state()->configData.pending.isEmpty());
		CHECK(store.state()->configData.seenSeq == metadata.seenSeq);
		CHECK(!QFileInfo::exists(root + u"/pending/config-1.json"_q));
		CHECK(store.ConfirmConfigReadBack(first.canonical, u"device-a"_q).status
			== Purple::SyncStoreStatus::NoPending);
		auto next = store.state()->configData;
		next.pending = changed.version.key;
		CHECK(bool(store.StageConfig(changed.canonical, next)));
		CHECK(store.ConfirmConfigReadBack(first.canonical, u"device-a"_q).status
			== Purple::SyncStoreStatus::Unconfirmed);
		CHECK(store.state()->config.pendingSeq == 2);
		CHECK(store.ConfirmConfigReadBack(changed.canonical, u"device-a"_q)
			.status == Purple::SyncStoreStatus::Ready);
		CHECK(store.state()->configData.base == changed.version.key);
		CHECK(store.state()->configData.baseLineage == changed.version.lineage);
		CHECK(store.state()->configData.seenSeq == metadata.seenSeq);
		const auto acknowledgement = RecordWithVersion(initial, 3,
			"version = 1\nname = 'changed'\n", changed.version);
		CHECK(bool(acknowledgement));
		auto sameMetadata = store.state()->configData;
		sameMetadata.pending = acknowledgement.version.key;
		CHECK(bool(store.StageConfig(acknowledgement.canonical, sameMetadata)));
		CHECK(store.ConfirmConfigReadBack(
			acknowledgement.canonical, u"device-a"_q).status
			== Purple::SyncStoreStatus::Ready);
		CHECK(store.state()->configData.base == changed.version.key);
		CHECK(store.state()->configData.seenSeq == metadata.seenSeq);
	}
	{
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == Purple::SyncStoreStatus::Ready);
		CHECK(restarted.state()->configData.base == changed.version.key);
	}
}

void TestCloneRejection() {
	for (auto mode = 0; mode != 4; ++mode) {
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		const auto initial = InitialState();
		const auto first = Record(initial, 1, "version = 1\nname = 'first'\n");
		const auto wrong = Record(initial, 1, "version = 1\nname = 'wrong'\n");
		const auto higher = Record(initial, 2, "version = 1\nname = 'higher'\n");
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		auto metadata = initial.configData;
		metadata.pending = first.version.key;
		CHECK(bool(store.StageConfig(first.canonical, metadata)));
		const auto record = mode == 2 ? wrong.canonical
			: mode == 3 ? higher.canonical : first.canonical;
		const auto device = mode == 0 ? u"device-b"_q
			: mode == 1 ? QString() : u"device-a"_q;
		const auto result = store.ConfirmConfigReadBack(record, device);
		CHECK(result.status == Purple::SyncStoreStatus::CloneDetected);
		CHECK(result.cloneVerdict == (mode < 2
			? Purple::SyncCloneVerdict::DeviceMismatch
			: mode == 2 ? Purple::SyncCloneVerdict::HashMismatch
			: Purple::SyncCloneVerdict::RemoteAhead));
		CHECK(store.state() == nullptr);
		CHECK(store.StageConfig(first.canonical, metadata).status
			== Purple::SyncStoreStatus::InvalidTransition);
		CHECK(QFileInfo::exists(root + u"/pending/config-1.json"_q));
	}
}

void TestSupersededStages() {
	for (auto mode = 0; mode != 2; ++mode) {
		const auto restart = (mode != 0);
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		const auto initial = InitialState();
		const auto first = Record(initial, 1,
			"version = 1\nname = 'abandoned'\n");
		const auto second = Record(initial, 2,
			"version = 1\nname = 'chosen remote'\n");
		CHECK(bool(first));
		CHECK(bool(second));
		{
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
			CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
			auto metadata = initial.configData;
			metadata.pending = first.version.key;
			CHECK(bool(store.StageConfig(first.canonical, metadata)));
			metadata.pending = second.version.key;
			CHECK(bool(store.StageConfig(second.canonical, metadata)));
			CHECK(QFileInfo::exists(root + u"/pending/config-1.json"_q));
			if (!restart) {
				CHECK(store.ConfirmConfigReadBack(
					second.canonical, u"device-a"_q).status
					== Purple::SyncStoreStatus::Ready);
				CHECK(!QFileInfo::exists(root + u"/pending/config-1.json"_q));
			}
		}
		if (restart) {
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status == Purple::SyncStoreStatus::Ready);
			CHECK(!QFileInfo::exists(root + u"/pending/config-1.json"_q));
			CHECK(bool(store.ReadPendingConfig()));
			CHECK(store.ConfirmConfigReadBack(
				second.canonical, u"device-a"_q).status
				== Purple::SyncStoreStatus::Ready);
		}
	}
}

void TestNoPartialCleanup() {
	auto temp = QTemporaryDir();
	const auto root = Root(temp);
	const auto initial = InitialState();
	const auto first = Record(initial, 1, "version = 1\nname = 'first'\n");
	const auto second = Record(initial, 2, "version = 1\nname = 'second'\n");
	{
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		auto metadata = initial.configData;
		metadata.pending = first.version.key;
		CHECK(bool(store.StageConfig(first.canonical, metadata)));
		metadata.pending = second.version.key;
		CHECK(bool(store.StageConfig(second.canonical, metadata)));
	}
	const auto oldPath = root + u"/pending/config-1.json"_q;
	const auto futurePath = root + u"/pending/config-3.json"_q;
	{
		auto file = QFile(futurePath);
		CHECK(file.open(QIODevice::WriteOnly));
		CHECK(file.write("future") == 6);
	}
	{
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == Purple::SyncStoreStatus::OrphanStage);
		CHECK(QFileInfo::exists(oldPath));
		CHECK(QFileInfo::exists(futurePath));
	}
}

void TestPostCommitCleanup() {
	auto temp = QTemporaryDir();
	const auto root = Root(temp);
	const auto initial = InitialState();
	const auto first = Record(initial, 1, "version = 1\nname = 'first'\n");
	{
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		auto metadata = initial.configData;
		metadata.pending = first.version.key;
		CHECK(bool(store.StageConfig(first.canonical, metadata)));
		CHECK(store.ConfirmConfigReadBack(first.canonical, u"device-a"_q).status
			== Purple::SyncStoreStatus::Ready);
	}
	const auto stagePath = root + u"/pending/config-1.json"_q;
	{
		auto file = QFile(stagePath);
		CHECK(file.open(QIODevice::WriteOnly));
		CHECK(file.write(first.canonical) == first.canonical.size());
		CHECK(file.setPermissions(QFileDevice::ReadOwner
			| QFileDevice::WriteOwner));
	}
	{
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == Purple::SyncStoreStatus::Ready);
		CHECK(!QFileInfo::exists(stagePath));
	}
	{
		auto file = QFile(stagePath);
		CHECK(file.open(QIODevice::WriteOnly));
		CHECK(file.write("corrupt") == 7);
		CHECK(file.setPermissions(QFileDevice::ReadOwner
			| QFileDevice::WriteOwner));
	}
	{
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == Purple::SyncStoreStatus::OrphanStage);
		CHECK(QFileInfo::exists(stagePath));
	}
}

void TestCleanupFailure() {
#ifdef Q_OS_UNIX
	auto temp = QTemporaryDir();
	const auto root = Root(temp);
	const auto initial = InitialState();
	const auto first = Record(initial, 1, "version = 1\nname = 'first'\n");
	const auto pendingDirectory = root + u"/pending"_q;
	const auto stagePath = pendingDirectory + u"/config-1.json"_q;
	{
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		auto metadata = initial.configData;
		metadata.pending = first.version.key;
		CHECK(bool(store.StageConfig(first.canonical, metadata)));
		CHECK(QFile(pendingDirectory).setPermissions(
			QFileDevice::ReadOwner | QFileDevice::ExeOwner));
		CHECK(store.ConfirmConfigReadBack(first.canonical, u"device-a"_q)
			.status == Purple::SyncStoreStatus::CleanupFailed);
		CHECK(QFileInfo::exists(stagePath));
		const auto file = QFile(root + u"/state.json"_q);
		auto saved = QFile(file.fileName());
		CHECK(saved.open(QIODevice::ReadOnly));
		const auto parsed = Purple::ParseSyncLocalState(saved.readAll());
		CHECK(bool(parsed));
		CHECK(parsed.state.config.pendingSeq == 0);
		CHECK(parsed.state.configData.base == first.version.key);
	}
	CHECK(QFile(pendingDirectory).setPermissions(
		QFileDevice::ReadOwner | QFileDevice::WriteOwner
			| QFileDevice::ExeOwner));
	{
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == Purple::SyncStoreStatus::Ready);
		CHECK(!QFileInfo::exists(stagePath));
	}
#endif
}

void TestOrphanScan() {
	for (auto mode = 0; mode != 3; ++mode) {
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		{
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status
				== Purple::SyncStoreStatus::Uninitialized);
			if (mode != 0) {
				CHECK(store.Initialize(InitialState()).status
					== Purple::SyncStoreStatus::Ready);
			}
		}
		if (mode != 0) {
			auto state = InitialState();
			state.config.seq = 1;
			state.config.confirmedSeq = 1;
			state.config.ownHash = QString(64, u'0');
			const auto serialized = Purple::SerializeSyncLocalState(state);
			CHECK(bool(serialized));
			auto file = QFile(root + u"/state.json"_q);
			CHECK(file.open(QIODevice::WriteOnly));
			CHECK(file.write(serialized.canonical)
				== serialized.canonical.size());
		}
		const auto path = root + ((mode == 0)
			? u"/pending/config-2.json"_q
			: (mode == 1)
			? u"/pending/config-3.json"_q
			: u"/pending/config-01.json"_q);
		auto file = QFile(path);
		CHECK(file.open(QIODevice::WriteOnly));
		CHECK(file.write("orphan") == 6);
		file.close();
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status
			== Purple::SyncStoreStatus::OrphanStage);
	}
}

void TestRejectedStage() {
	auto temp = QTemporaryDir();
	const auto initial = InitialState();
	auto store = Purple::SyncLocalStore(Root(temp));
	CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
	CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
	const auto one = Record(initial, 1, "version = 1\nname = 'one'\n");
	const auto two = Record(initial, 2, "version = 1\nname = 'two'\n");
	auto metadata = initial.configData;
	metadata.pending = one.version.key;
	CHECK(store.StageConfig(two.canonical, metadata).status
		== Purple::SyncStoreStatus::InvalidRecord);
	metadata.pending.clear();
	CHECK(store.StageConfig(one.canonical, metadata).status
		== Purple::SyncStoreStatus::InvalidRecord);
	metadata.pending = one.version.key;
	auto changed = one.canonical;
	changed.append(' ');
	CHECK(store.StageConfig(changed, metadata).status
		== Purple::SyncStoreStatus::InvalidRecord);
	CHECK(store.state()->config.seq == 0);
	CHECK(store.ReadPendingConfig().status == Purple::SyncStoreStatus::NoPending);
}

void TestCorruptPending() {
	for (auto mode = 0; mode != 2; ++mode) {
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		const auto initial = InitialState();
		const auto one = Record(initial, 1, "version = 1\nname = 'one'\n");
		const auto other = Record(initial, 1, "version = 1\nname = 'other'\n");
		{
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
			CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
			auto metadata = initial.configData;
			metadata.pending = one.version.key;
			CHECK(bool(store.StageConfig(one.canonical, metadata)));
		}
		const auto path = root + u"/pending/config-1.json"_q;
		if (mode == 0) {
			CHECK(QFile::remove(path));
		} else {
			auto file = QFile(path);
			CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			CHECK(file.write(other.canonical) == other.canonical.size());
		}
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status == (mode == 0
			? Purple::SyncStoreStatus::PendingMissing
			: Purple::SyncStoreStatus::PendingMismatch));
		CHECK(restarted.state() == nullptr);
	}
}

void TestInvalidStateAndOrphan() {
	for (auto mode = 0; mode != 3; ++mode) {
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		{
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
			if (mode != 2) {
				CHECK(store.Initialize(InitialState()).status
					== Purple::SyncStoreStatus::Ready);
			}
		}
		if (mode == 2) {
			auto file = QFile(root + u"/pending/config-1.json"_q);
			CHECK(file.open(QIODevice::WriteOnly));
			CHECK(file.write("orphan") == 6);
		} else {
			auto file = QFile(root + u"/state.json"_q);
			CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			if (mode == 0) {
				CHECK(file.write("{") == 1);
			} else {
				auto state = Purple::SerializeSyncLocalState(InitialState()).canonical;
				state.replace("\"version\":1", "\"version\":2");
				CHECK(file.write(state) == state.size());
			}
		}
		auto restarted = Purple::SyncLocalStore(root);
		const auto expected = (mode == 0)
			? Purple::SyncStoreStatus::InvalidState
			: (mode == 1)
			? Purple::SyncStoreStatus::NewerState
			: Purple::SyncStoreStatus::OrphanStage;
		CHECK(restarted.Open(true).status == expected);
		CHECK(restarted.state() == nullptr);
	}
}

void TestPermissionsAndCollision() {
	{
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		CHECK(QDir().mkdir(root));
#ifdef Q_OS_UNIX
		CHECK(QFile(root).setPermissions(QFileDevice::ReadOwner
			| QFileDevice::WriteOwner | QFileDevice::ExeOwner
			| QFileDevice::ReadGroup));
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status
			== Purple::SyncStoreStatus::InsecurePermissions);
#endif
	}
	{
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		const auto initial = InitialState();
		auto store = Purple::SyncLocalStore(root);
		CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial).status == Purple::SyncStoreStatus::Ready);
		CHECK(QDir().mkdir(root + u"/pending/config-1.json"_q));
		const auto record = Record(initial, 1, "version = 1\n");
		auto metadata = initial.configData;
		metadata.pending = record.version.key;
		CHECK(store.StageConfig(record.canonical, metadata).status
			== Purple::SyncStoreStatus::OrphanStage);
		CHECK(store.state() == nullptr);
	}
#ifdef Q_OS_UNIX
	for (auto pathKind = 0; pathKind != 2; ++pathKind) {
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		{
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
			CHECK(store.Initialize(InitialState()).status
				== Purple::SyncStoreStatus::Ready);
		}
		const auto path = pathKind == 0
			? root + u"/state.json"_q
			: root + u"/pending"_q;
		const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
		CHECK(QFile(path).setPermissions(owner | QFileDevice::ReadGroup));
		auto restarted = Purple::SyncLocalStore(root);
		CHECK(restarted.Open(true).status
			== Purple::SyncStoreStatus::InsecurePermissions);
	}
	for (auto pathKind = 0; pathKind != 2; ++pathKind) {
		auto temp = QTemporaryDir();
		const auto root = Root(temp);
		{
			auto store = Purple::SyncLocalStore(root);
			CHECK(store.Open(true).status == Purple::SyncStoreStatus::Uninitialized);
			CHECK(store.Initialize(InitialState()).status
				== Purple::SyncStoreStatus::Ready);
		}
		const auto path = pathKind == 0 ? root : root + u"/pending"_q;
		CHECK(QFile(path).setPermissions(
			QFileDevice::WriteOwner | QFileDevice::ExeOwner));
		{
			auto restarted = Purple::SyncLocalStore(root);
			CHECK(restarted.Open(true).status
				== Purple::SyncStoreStatus::InsecurePermissions);
		}
		CHECK(QFile(path).setPermissions(QFileDevice::ReadOwner
			| QFileDevice::WriteOwner | QFileDevice::ExeOwner));
	}
#endif
}

}

int main() {
	TestLifecycle();
	TestConfirmation();
	TestCloneRejection();
	TestSupersededStages();
	TestNoPartialCleanup();
	TestPostCommitCleanup();
	TestCleanupFailure();
	TestRejectedStage();
	TestCorruptPending();
	TestInvalidStateAndOrphan();
	TestOrphanScan();
	TestPermissionsAndCollision();
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
