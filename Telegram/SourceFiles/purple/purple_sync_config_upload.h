/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "rpl/lifetime.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_chat_carry_io.h"

#include <QtCore/QByteArray>

#include <memory>
#include <optional>

namespace Main {
class Session;
}

namespace Purple {

class SyncRequestSender;

class SyncConfigUpload final
	: public std::enable_shared_from_this<SyncConfigUpload> {
public:
	SyncConfigUpload(
		Main::Session &session,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent,
		MsgId editId = 0, Fn<void()> editRefused = nullptr,
		DesktopSyncChatOperation operation = {},
		Fn<void(Fn<void(bool)>)> authorizeEdit = {});
	SyncConfigUpload(
		Main::Session &session,
		DesktopSyncChatIo route,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent,
		DesktopSyncChatOperation operation = {},
		MsgId editId = 0, Fn<void()> editRefused = nullptr,
		Fn<void(Fn<void(bool)>)> authorizeEdit = {});
	SyncConfigUpload(
		Main::Session &session,
		DesktopSyncCarryPermit permit,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent,
		Fn<void(Fn<void(std::optional<DesktopSyncCarryPermit>)>)> renewCarry);
	~SyncConfigUpload();

	SyncConfigUpload(const SyncConfigUpload &) = delete;
	SyncConfigUpload &operator=(const SyncConfigUpload &) = delete;

	void Start();
	void Cancel();
	[[nodiscard]] std::optional<DesktopSyncChatIo> route() const;
	[[nodiscard]] std::optional<MTPUpdates> carryResult() const;
	[[nodiscard]] Fn<bool()> readbackCurrent() const;

private:
	[[nodiscard]] bool RequestCurrent() const;
	[[nodiscard]] bool Current() const;
	void SendNextPart();
	void SendMedia();
	void RenewBeforePost(Fn<void(bool)> ready);
	void ResumeEffect(Fn<void(bool)> ready);
	void DispatchMedia();
	void OnMedia(const MTPUpdates &result);
	void Finish(std::optional<MsgId> messageId);

	std::unique_ptr<SyncRequestSender> _api;
	base::weak_ptr<Main::Session> _session;
	AccountKey _account;
	std::optional<DesktopSyncChatIo> _route;
	std::optional<DesktopSyncChatLease> _lease;
	DesktopSyncChatOperation _operation;
	std::optional<DesktopSyncCarryPermit> _carry;
	std::optional<MTPUpdates> _carryResult;
	bool _carryContentValid = false;
	uint64_t _carryGateTicket = 0;
	Fn<void(Fn<void(std::optional<DesktopSyncCarryPermit>)>)> _renewCarry;
	QByteArray _content;
	Fn<void(std::optional<MsgId>)> _finished;
	Fn<bool()> _stillCurrent;
	MsgId _editId;
	Fn<void()> _editRefused;
	Fn<void(Fn<void(bool)>)> _authorizeEdit;
	uint64 _fileId = 0;
	uint64 _randomId = 0;
	qsizetype _offset = 0;
	int _part = 0;
	int _parts = 0;
	bool _checking = false;
	bool _started = false;
	bool _done = false;
	rpl::lifetime _lifetime;

};

}
