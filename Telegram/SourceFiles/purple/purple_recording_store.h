/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_recording_crypt.h"
#include "purple/purple_recording_registry.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Purple {

struct RecordingBinItem {
	QString id;
	int account = -1;
	PeerId peer = 0;
	MsgId topic = 0;
	MsgId reply = 0;
	RecordingRegistry::Kind kind = RecordingRegistry::Kind::Voice;
	RecordingRegistry::Mark mark = RecordingRegistry::Mark::None;
	TimeId started = 0;
	crl::time duration = 0;
	bool viewOnce = false;
};

void StartRecordingStore();

[[nodiscard]] std::vector<RecordingBinItem> RecordingBinItems();
[[nodiscard]] int RecordingBinCount();
[[nodiscard]] rpl::producer<> RecordingBinChanges();
[[nodiscard]] std::optional<QByteArray> RecordingBinMedia(const QString &id);
void DeleteRecordingBinItem(const QString &id);
void ClearRecordingBin();
void SendRecordingBinItem(
	const QString &id,
	not_null<Data::Thread*> thread,
	Fn<void(bool)> done);
[[nodiscard]] Main::Session *RecordingAccountSession(int account);
[[nodiscard]] int RecordingAccountIndex(not_null<Main::Session*> session);

[[nodiscard]] std::shared_ptr<const RecordingCrypt::Key> RecordingKey();
[[nodiscard]] RecordingCrypt::RandomFill RecordingRandom();
[[nodiscard]] std::filesystem::path RecordingPath(const std::string &name);
[[nodiscard]] std::optional<std::string> BeginRecordingEntry(
	RecordingRegistry::Kind kind,
	const std::string &mediaSuffix,
	const std::string &journalSuffix);
void UpdateRecordingEntry(
	const std::string &id,
	Fn<void(RecordingRegistry::Entry&)> update);
void ForgetRecordingEntry(const std::string &id);

} // namespace Purple
