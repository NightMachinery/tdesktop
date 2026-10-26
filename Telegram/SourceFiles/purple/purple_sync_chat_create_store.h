#pragma once

#include "purple/purple_sync_chat_create.h"

#include <QtCore/QLockFile>

namespace Purple {

class DesktopSyncChatCreateStore final {
public:
	DesktopSyncChatCreateStore(QString directory, QString authority,
		AccountKey account, Fn<bool()> current);
	[[nodiscard]] bool Open();
	[[nodiscard]] bool Save(const DesktopSyncChatCreateJournal &journal);
	[[nodiscard]] const std::optional<DesktopSyncChatCreateJournal> &journal() const;
	[[nodiscard]] QString error() const;

private:
	QString _directory;
	QString _path;
	QString _authority;
	AccountKey _account;
	Fn<bool()> _current;
	std::unique_ptr<QLockFile> _lock;
	QByteArray _identity;
	QByteArray _bytes;
	std::optional<DesktopSyncChatCreateJournal> _journal;
	QString _error;

};

}
