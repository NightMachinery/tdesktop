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

#include <QtCore/QByteArray>

#include <memory>
#include <optional>

namespace Main {
class Session;
}

namespace Purple {

class SyncConfigUpload final
	: public std::enable_shared_from_this<SyncConfigUpload> {
public:
	SyncConfigUpload(
		Main::Session &session,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished);
	~SyncConfigUpload();

	SyncConfigUpload(const SyncConfigUpload &) = delete;
	SyncConfigUpload &operator=(const SyncConfigUpload &) = delete;

	void Start();

private:
	void SendNextPart();
	void SendMedia();
	void OnMedia(const MTPUpdates &result);
	void Finish(std::optional<MsgId> messageId);

	base::weak_ptr<Main::Session> _session;
	QByteArray _content;
	Fn<void(std::optional<MsgId>)> _finished;
	uint64 _fileId = 0;
	uint64 _randomId = 0;
	qsizetype _offset = 0;
	int _part = 0;
	int _parts = 0;
	bool _started = false;
	bool _done = false;
	rpl::lifetime _lifetime;

};

}
