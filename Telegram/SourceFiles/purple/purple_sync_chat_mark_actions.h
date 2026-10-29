#pragma once

#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_chat_carry_gate.h"
#include "purple/purple_sync_chat_carry_store.h"
#include "purple/purple_sync_chat_write.h"

namespace Purple {

struct DesktopSyncChatStaleMark {
	DesktopSyncChatOrigin origin;
	QString login;
	SyncChatCache location;
	QString text;
	QByteArray author;
	[[nodiscard]] bool operator==(const DesktopSyncChatStaleMark &) const = default;
};

struct DesktopSyncChatMarkExact {
	DesktopSyncChatIo route;
	MTPmessages_Messages response;
	bool readable = false;
};

[[nodiscard]] bool DesktopSyncChatMarkRemovalActivated();
[[nodiscard]] std::optional<DesktopSyncChatStaleMark> CaptureDesktopSyncChatStaleMark(
	const DesktopSyncChatPeer &peer, const QString &login, const MTPMessage &message);
[[nodiscard]] bool DesktopSyncChatSelectedMarkAbsent(
	const DesktopSyncChatStaleMark &row, const DesktopSyncChatMarkExact &exact);
[[nodiscard]] bool AuthorizeDesktopSyncChatStaleMark(
	const DesktopSyncChatStaleMark &row, const DesktopSyncChatSetupSnapshot &fresh,
	const DesktopSyncChatMarkExact &exact);
[[nodiscard]] bool DesktopSyncChatMarkReferencesClear(
	SyncChatCarryStore &store, const DesktopSyncChatStaleMark &row);

class DesktopSyncChatMarkPermit final {
public:
	[[nodiscard]] bool current() const;
	[[nodiscard]] const DesktopSyncChatStaleMark &row() const;
	[[nodiscard]] const DesktopSyncChatIo &route() const;
	[[nodiscard]] std::optional<DesktopSyncChatDelete> request() const;

private:
	friend class DesktopSyncChatMarkActions;
	DesktopSyncChatStaleMark _row;
	DesktopSyncChatIo _route;
	Fn<bool()> _current;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::shared_ptr<std::atomic<bool>> _active;
	std::shared_ptr<std::atomic<bool>> _claimed;
};

using DesktopSyncChatMarkRenewal = Fn<void(Fn<void(std::optional<DesktopSyncChatMarkPermit>)>)>;

struct DesktopSyncChatMarkProgress {
	QString reason;
	bool terminal = false;
	bool absent = false;
};

struct DesktopSyncChatMarkPort {
	Fn<DesktopSyncChatOrigin()> origin;
	Fn<QString()> login;
	Fn<QString()> window;
	Fn<bool()> current;
	Fn<bool()> foreground;
	Fn<void(Fn<void()>)> worker;
	Fn<void(Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)>)> resolve;
	Fn<void(const DesktopSyncChatStaleMark &, Fn<void(std::optional<DesktopSyncChatMarkExact>)>)> exact;
	Fn<void(DesktopSyncChatMarkPermit, DesktopSyncChatMarkRenewal,
		Fn<void(std::optional<MTPmessages_AffectedMessages>)>)> send;
	Fn<void(const DesktopSyncChatIo &, const MTPmessages_AffectedMessages &, Fn<void(bool)>)> apply;
	Fn<void()> cancel;
	Fn<void(DesktopSyncChatMarkProgress)> status;
};

class DesktopSyncChatMarkActions final {
public:
	DesktopSyncChatMarkActions(DesktopSyncChatStaleMark row, QString window,
		QString directory, SyncChatCarryStoreScope scope, DesktopSyncChatMarkPort port);
	~DesktopSyncChatMarkActions();
	void start();
	void close();

private:
	class State;
	std::shared_ptr<State> _state;
};

}
