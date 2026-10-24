#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_sync_auto.h"
#include "purple/purple_sync_local_store.h"

namespace Purple {

struct DesktopSyncAutoRead {
	SyncStoreStatus status = SyncStoreStatus::IoError;
	std::optional<SyncAutoRecord> record;
	QByteArray bytes;
};

[[nodiscard]] DesktopSyncAutoRead ReadDesktopSyncAuto(
	const AccountFiles &files, const AccountKey &account);
[[nodiscard]] bool DesktopSyncAutoEditAuthorized(const SyncLocalState &state, const SyncAutoServerRead &read);
[[nodiscard]] bool SameDesktopSyncAutoContext(
	const AccountSettingsOpenTarget &opening,
	const AccountSettingsOpenTarget &current);

enum class DesktopSyncAutoCompletion { Ready, Obsolete, ContextChanged, LocalFailure };

class DesktopSyncAutoStore final {
public:
	DesktopSyncAutoStore(AccountFiles files, AccountKey account,
		QByteArray token, std::function<bool()> current);
	[[nodiscard]] bool Open();
	[[nodiscard]] SyncAutoPlan Transition(SyncAutoFacts facts,
		SyncAutoEvent event, int64_t now,
		uint64_t completedTicket = 0,
		SyncAutoOutcome outcome = SyncAutoOutcome::Inventory,
		std::optional<SyncAutoInventory> inventory = std::nullopt);
	[[nodiscard]] bool ExecuteLocal(const SyncAutoPlan &plan,
		const std::function<bool(SyncLocalStore &)> &apply = {});
	[[nodiscard]] bool AuthorizeEffect(const SyncAutoPlan &plan, const SyncAutoServerRead *read = nullptr);
	[[nodiscard]] bool CommitRetirement(const SyncAutoPlan &plan,
		const SyncAutoServerRead &absence);
	[[nodiscard]] bool Authorized(const SyncAutoPlan &plan) const;
	[[nodiscard]] bool Current() const;
	[[nodiscard]] DesktopSyncAutoCompletion CheckCompletion(
		const SyncAutoPlan &plan, const QString &owner) const;
	[[nodiscard]] bool HoldCheckpoint(
		const SyncAutoPlan &plan, SyncAutoReason reason);
	[[nodiscard]] const SyncAutoRecord &record() const;
	[[nodiscard]] SyncLocalStore &local();
	[[nodiscard]] const AccountFiles &files() const;

private:
	[[nodiscard]] bool RecordCurrent() const;
	[[nodiscard]] bool WriteRecord(const SyncAutoRecord &record, bool requireCurrent);
	AccountFiles _files;
	AccountKey _account;
	QByteArray _token;
	std::function<bool()> _current;
	SyncLocalStore _local;
	SyncAutoRecord _record;
	QByteArray _recordBytes;
	QByteArray _directoryIdentity;
	bool _opened = false;
	bool _saveSettled = false;

};

}
