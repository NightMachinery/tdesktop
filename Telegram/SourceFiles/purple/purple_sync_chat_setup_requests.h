#pragma once

#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_chat_io.h"
#include "purple/purple_sync_retry.h"
#include "purple/purple_sync_chat_cleanup.h"

namespace Purple {

using DesktopSyncChatDecoration = std::variant<
	MTPmessages_UpdatePinnedMessage,
	MTPaccount_UpdateNotifySettings,
	MTPfolders_EditPeerFolders>;

[[nodiscard]] std::optional<MTPmessages_SendMessage> DesktopSyncChatMarkRequest(
	const DesktopSyncChatIo &route, uint64_t randomId);
[[nodiscard]] std::optional<DesktopSyncChatDecoration> DesktopSyncChatDecorationRequest(
	const DesktopSyncChatCreateCommand &command, const DesktopSyncChatIo &route);
[[nodiscard]] auto DesktopSyncChatMoveDecorationRequest(
	const DesktopSyncChatSetupSnapshot &opening,
	const DesktopSyncChatCreateJournal &provision,
	const SyncCarryJournal &carry, const DesktopSyncChatIo &route,
	DesktopSyncChatCreateStep step)
-> std::optional<DesktopSyncChatDecoration>;
enum class DesktopSyncChatMoveDecorationPhase {
	NotStarted, Ready, Waiting, Held, Complete, Cancelled
};

struct DesktopSyncChatMoveDecorationCommand {
	QString run;
	QString operation;
	uint64_t ticket = 0;
	DesktopSyncChatCreateStep step = DesktopSyncChatCreateStep::Pin;
	DesktopSyncChatDecoration request;
};

class DesktopSyncChatMoveDecorationAction final {
public:
	DesktopSyncChatMoveDecorationAction(DesktopSyncChatSetupSnapshot opening,
		DesktopSyncChatCreateJournal provision, Fn<bool()> current);
	[[nodiscard]] auto observe(
		const SyncCarryJournal &carry, const DesktopSyncChatIo &route)
	-> std::optional<DesktopSyncChatMoveDecorationCommand>;
	[[nodiscard]] auto retry(
		const SyncCarryJournal &carry, const DesktopSyncChatIo &route)
	-> std::optional<DesktopSyncChatMoveDecorationCommand>;
	void complete(const DesktopSyncChatMoveDecorationCommand &command, bool okay);
	void cancel();
	[[nodiscard]] DesktopSyncChatMoveDecorationPhase phase() const;
	[[nodiscard]] QString reason() const;
	[[nodiscard]] DesktopSyncCleanupProgress cleanupProgress(const SyncCarryJournal &journal, bool pendingRequest) const;

private:
	bool accepts(const DesktopSyncChatMoveDecorationCommand &command) const;
	DesktopSyncChatSetupSnapshot _opening;
	DesktopSyncChatCreateJournal _provision;
	Fn<bool()> _current;
	QString _run;
	QString _receipt;
	QString _reason;
	uint64_t _ticket = 0;
	DesktopSyncChatCreateStep _next = DesktopSyncChatCreateStep::Pin;
	DesktopSyncChatMoveDecorationPhase _phase = DesktopSyncChatMoveDecorationPhase::NotStarted;
	std::optional<DesktopSyncChatMoveDecorationCommand> _active;

};

[[nodiscard]] QByteArray EncodeDesktopSyncChatSetupUpdates(const MTPUpdates &updates);
[[nodiscard]] std::optional<MTPUpdates> DecodeDesktopSyncChatSetupUpdates(const QByteArray &bytes);
[[nodiscard]] DesktopSyncChatCreateOutcome DesktopSyncChatSetupError(const QString &type);

}
