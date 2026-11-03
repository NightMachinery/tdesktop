/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_store.h"

#include "purple/hooks/media_recording_capture.h"
#include "purple/purple_archive_crypto.h"
#include "purple/purple_config.h"
#include "purple/purple_recording.h"
#include "purple/purple_recording_mirror.h"
#include "purple/purple_recording_recovery.h"
#include "purple/purple_recording_round.h"
#include "purple/purple_recording_round_media.h"

#include "apiwrap.h"
#include "api/api_common.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture.h"
#include "media/audio/media_audio_edit.h"
#include "storage/storage_account.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>

#include <cstring>
#include <mutex>
#include <set>

namespace Purple {
namespace {

using namespace RecordingRegistry;

constexpr auto kManifestName = "recordings.json";
constexpr auto kLongestVoice = crl::time(24 * 3600 * 1000);

struct Store {
	std::mutex mutex;
	std::shared_ptr<const RecordingCrypt::Key> key;
	Manifest manifest;
	bool loaded = false;
	std::vector<std::string> deleteAfterClose;
	rpl::event_stream<> changes;
	rpl::lifetime captureLifetime;
	rpl::lifetime lifetime;
	std::set<not_null<Main::Session*>> watched;
	std::unique_ptr<base::Timer> sweep;
	bool captureWatched = false;
};

[[nodiscard]] Store &TheStore() {
	static auto result = Store();
	return result;
}

[[nodiscard]] QString Directory() {
	const auto name = cDataFile();
	return cWorkingDir() + u"tdata/purple/recordings"_q
		+ ((name == u"data"_q) ? QString() : (u"-"_q + name))
		+ '/';
}

[[nodiscard]] std::filesystem::path Native(const QString &path) {
	return std::filesystem::path(path.toStdU16String());
}

[[nodiscard]] std::string NewId() {
	auto random = DesktopArchiveCrypto::Random(8);
	if (!random) {
		return {};
	}
	const auto result = random->toHex().toStdString();
	DesktopArchiveCrypto::Clear(*random);
	return result;
}

void Fire() {
	crl::on_main([] {
		TheStore().changes.fire({});
	});
}

void SaveLocked(Store &store) {
	if (!store.key) {
		return;
	}
	const auto text = Encode(store.manifest);
	std::error_code error;
	std::filesystem::create_directories(Native(Directory()), error);
	const auto saved = RecordingCrypt::WriteWhole(
		RecordingPath(kManifestName),
		store.key,
		RecordingRandom(),
		reinterpret_cast<const std::uint8_t*>(text.data()),
		text.size());
	if (!saved) {
		LOG(("Purple Recording: could not write the manifest."));
	}
}

void RemoveFilesOf(const Entry &entry) {
	auto error = std::error_code();
	std::filesystem::remove(RecordingPath(entry.media), error);
	if (!entry.journal.empty()) {
		std::filesystem::remove(RecordingPath(entry.journal), error);
	}
}

void RemoveLocked(Store &store, const std::string &id) {
	if (const auto entry = store.manifest.find(id)) {
		RemoveFilesOf(*entry);
		store.manifest.remove(id);
	}
}

[[nodiscard]] std::optional<RecordingCrypt::Key> DeriveKey(
		not_null<Main::Account*> account) {
	const auto local = account->local().peekLegacyLocalKey();
	if (!local) {
		return std::nullopt;
	}
	const auto bytes = local->data();
	auto input = QByteArray(
		reinterpret_cast<const char*>(bytes.data()),
		bytes.size());
	auto derived = DesktopArchiveCrypto::Derive(input, {
		.salt = QByteArray("purple-recording-v1"),
		.info = QByteArray("recording-files"),
		.outputBytes = int(RecordingCrypt::kKeyBytes),
	});
	DesktopArchiveCrypto::Clear(input);
	if (!derived || derived->size() != int(RecordingCrypt::kKeyBytes)) {
		return std::nullopt;
	}
	auto check = DesktopArchiveCrypto::Hmac(*derived, QByteArray("check"));
	auto result = std::optional<RecordingCrypt::Key>();
	if (check && check->size() >= int(RecordingCrypt::kCheckBytes)) {
		result.emplace();
		std::memcpy(result->bytes.data(), derived->constData(), result->bytes.size());
		std::memcpy(result->check.data(), check->constData(), result->check.size());
	}
	DesktopArchiveCrypto::Clear(*derived);
	if (check) {
		DesktopArchiveCrypto::Clear(*check);
	}
	return result;
}

void RemoveStrays(const Store &store) {
	auto referenced = std::set<std::string>{ kManifestName };
	for (const auto &entry : store.manifest.entries) {
		referenced.insert(entry.media);
		if (!entry.journal.empty()) {
			referenced.insert(entry.journal);
		}
	}
	auto removed = 0;
	const auto names = QDir(Directory()).entryList(QDir::Files);
	for (const auto &name : names) {
		const auto utf = name.toStdString();
		if (!referenced.contains(utf)
			&& (name.endsWith(u".prc"_q) || name.endsWith(u".new"_q))) {
			auto error = std::error_code();
			if (std::filesystem::remove(RecordingPath(utf), error)) {
				++removed;
			}
		}
	}
	if (removed) {
		LOG(("Purple Recording: removed %1 stray files.").arg(removed));
	}
}

[[nodiscard]] RecordingRecovery::Result RecoverFiles(
		Kind kind,
		const std::filesystem::path &media,
		const std::filesystem::path &journal,
		std::shared_ptr<const RecordingCrypt::Key> key,
		bool wasSending) {
	return (kind == Kind::Round)
		? RoundMedia::Recover(media, journal, key, RecordingRandom(), wasSending)
		: RecordingRecovery::RecoverVoice(media, key, RecordingRandom(), wasSending);
}

[[nodiscard]] std::filesystem::path JournalPath(const Entry &entry) {
	return entry.journal.empty()
		? std::filesystem::path()
		: RecordingPath(entry.journal);
}

void ApplyRecovery(
		const std::string &id,
		RecordingRecovery::Result result,
		TimeId now) {
	auto &store = TheStore();
	{
		const auto lock = std::lock_guard(store.mutex);
		const auto entry = store.manifest.find(id);
		if (!entry) {
			return;
		}
		entry->state = result.state;
		entry->durationMs = result.durationMs;
		entry->partial = entry->partial || result.partial;
		entry->binned = now;
		SaveLocked(store);
	}
	Fire();
}

void Recover(std::vector<Entry> entries) {
	if (entries.empty()) {
		return;
	}
	const auto key = RecordingKey();
	crl::async([=] {
		auto recovered = 0;
		auto unrecoverable = 0;
		for (const auto &entry : entries) {
			const auto now = base::unixtime::now();
			const auto result = RecoverFiles(
				entry.kind,
				RecordingPath(entry.media),
				JournalPath(entry),
				key,
				entry.state == State::Sending);
			if (result.state == State::Unrecoverable) {
				++unrecoverable;
			} else {
				++recovered;
			}
			crl::on_main([=] {
				ApplyRecovery(entry.id, result, now);
			});
		}
		LOG(("Purple Recording: recovered %1, unrecoverable %2."
			).arg(recovered
			).arg(unrecoverable));
	});
}

void Sweep() {
	auto &store = TheStore();
	auto removed = 0;
	{
		const auto lock = std::lock_guard(store.mutex);
		if (!store.loaded) {
			return;
		}
		const auto now = base::unixtime::now();
		const auto &settings = ActiveSettings().recording;
		auto ids = Expired(store.manifest, [&](std::int64_t binned) {
			return BinItemExpired(settings, binned, now);
		});
		if (!settings.keepDiscarded) {
			for (auto &id : UnclaimedListen(store.manifest, now)) {
				ids.push_back(std::move(id));
			}
		}
		for (const auto &id : ids) {
			RemoveLocked(store, id);
			++removed;
		}
		if (removed) {
			SaveLocked(store);
		}
	}
	if (removed) {
		LOG(("Purple Recording: the bin cleared %1 items.").arg(removed));
		Fire();
	}
}

[[nodiscard]] std::int64_t MediaSizeOf(const Entry &entry) {
	auto error = std::error_code();
	const auto size = std::filesystem::file_size(RecordingPath(entry.media), error);
	return (error || size < RecordingCrypt::kHeaderBytes)
		? -1
		: std::int64_t(size - RecordingCrypt::kHeaderBytes);
}

void SettleLocked(Store &store, const std::string &id) {
	for (const auto &part : GroupOf(store.manifest, id)) {
		const auto pending = std::find(
			store.deleteAfterClose.begin(),
			store.deleteAfterClose.end(),
			part);
		const auto entry = store.manifest.find(part);
		if (pending != store.deleteAfterClose.end() || !entry) {
			continue;
		} else if (entry->state == State::Recording) {
			entry->state = State::Sent;
		} else {
			RemoveLocked(store, part);
		}
	}
}

void HandleSent(
		not_null<Main::Session*> session,
		const Data::Session::IdChange &change) {
	const auto item = session->data().message(change.newId);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!item || !item->out() || !document
		|| item->Get<HistoryMessageForwarded>()
		|| !(document->isVoiceMessage() || document->isVideoMessage())) {
		return;
	}
	auto &store = TheStore();
	{
		const auto lock = std::lock_guard(store.mutex);
		const auto index = MatchSent(store.manifest, {
			.account = RecordingAccountIndex(session),
			.peer = change.newId.peer.value,
			.kind = document->isVideoMessage() ? Kind::Round : Kind::Voice,
			.now = base::unixtime::now(),
			.size = document->size,
		}, MediaSizeOf);
		if (!index) {
			return;
		}
		SettleLocked(store, store.manifest.entries[*index].id);
		SaveLocked(store);
	}
	Fire();
}

void Watch(not_null<Main::Session*> session) {
	auto &store = TheStore();
	if (!store.watched.emplace(session).second) {
		return;
	}
	session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		HandleSent(session, change);
	}, session->lifetime());
	session->lifetime().add([=] {
		TheStore().watched.erase(session);
	});
}

void FinishVoice();
void FinishClosed(std::vector<RecordingMirror::Closed> closed);

void WatchCapture() {
	auto &store = TheStore();
	const auto capture = ::Media::Capture::instance();
	if (store.captureWatched || !capture) {
		return;
	}
	store.captureWatched = true;
	capture->startedChanges(
	) | rpl::filter([](bool started) {
		return !started;
	}) | rpl::on_next([] {
		FinishVoice();
	}, store.captureLifetime);
}

void Load(not_null<Main::Account*> account) {
	auto &store = TheStore();
	auto pending = std::vector<Entry>();
	{
		const auto lock = std::lock_guard(store.mutex);
		if (store.loaded) {
			return;
		}
		const auto key = DeriveKey(account);
		if (!key) {
			return;
		}
		store.key = std::make_shared<const RecordingCrypt::Key>(*key);
		auto status = RecordingCrypt::OpenStatus::Missing;
		const auto bytes = RecordingCrypt::ReadWhole(
			RecordingPath(kManifestName),
			store.key,
			&status);
		auto read = ReadStatus::Empty;
		if (bytes) {
			store.manifest = Decode(
				std::string(bytes->begin(), bytes->end()),
				&read);
		}
		auto error = std::error_code();
		const auto path = RecordingPath(kManifestName);
		const auto exists = std::filesystem::exists(path, error);
		const auto clean = !exists
			|| (status == RecordingCrypt::OpenStatus::Ok
				&& (read == ReadStatus::Ok || read == ReadStatus::Empty));
		if (!clean) {
			LOG(("Purple Recording: the manifest did not read cleanly"
				" (%1, %2), %3 entries kept, moved aside."
				).arg(int(status)
				).arg(int(read)
				).arg(int(store.manifest.entries.size())));
			auto aside = path;
			aside += ".bad";
			std::filesystem::rename(path, aside, error);
			if (error) {
				store.manifest = Manifest();
				store.key = nullptr;
				return;
			}
			auto names = std::vector<std::string>();
			for (const auto &name : QDir(Directory()).entryList(QDir::Files)) {
				names.push_back(name.toStdString());
			}
			const auto adopted = AdoptOrphans(
				store.manifest,
				names,
				base::unixtime::now());
			LOG(("Purple Recording: adopted %1 files without an entry."
				).arg(int(adopted.size())));
		}
		store.loaded = true;
		for (const auto &entry : store.manifest.entries) {
			if (NeedsRecovery(entry.state)) {
				pending.push_back(entry);
			}
		}
		if (clean) {
			RemoveStrays(store);
		} else {
			SaveLocked(store);
		}
	}
	Recover(std::move(pending));
	Sweep();
	store.sweep = std::make_unique<base::Timer>(Sweep);
	store.sweep->callEach(crl::time(kBinSweepIntervalSeconds) * 1000);
	Fire();
}

void Refresh() {
	auto &store = TheStore();
	store.lifetime.destroy();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto account = entry.account.get();
		const auto ready = [=](Main::Session *session) {
			if (session) {
				Load(account);
				Watch(session);
				WatchCapture();
			}
		};
		account->sessionChanges(
		) | rpl::on_next(ready, store.lifetime);
		ready(account->maybeSession());
	}
}

[[nodiscard]] RecordingMirror &VoiceMirror() {
	static auto result = RecordingMirror([]() -> std::optional<RecordingMirror::Opened> {
		const auto id = BeginRecordingEntry(Kind::Voice, ".ogg.prc", {});
		if (!id) {
			return std::nullopt;
		}
		auto file = RecordingCrypt::File::Create(
			RecordingPath(*id + ".ogg.prc"),
			RecordingKey(),
			RecordingRandom());
		if (!file) {
			ForgetRecordingEntry(*id);
			return std::nullopt;
		}
		crl::on_main(WatchCapture);
		return RecordingMirror::Opened{ *id, std::move(file) };
	}, [](const std::string &) {
		LOG(("Purple Recording: a voice mirror write failed, mirroring stopped."));
	});
	return result;
}

void FinishClosed(std::vector<RecordingMirror::Closed> closed) {
	if (closed.empty()) {
		return;
	}
	auto &store = TheStore();
	auto finalise = std::vector<std::string>();
	{
		const auto lock = std::lock_guard(store.mutex);
		auto facts = std::vector<ClosedFacts>();
		for (const auto &one : closed) {
			facts.push_back({ .id = one.id, .failed = one.failed });
		}
		const auto copies = store.manifest;
		auto result = NoteClosed(
			store.manifest,
			facts,
			store.deleteAfterClose,
			ActiveSettings().recording.keepDiscarded,
			base::unixtime::now());
		for (const auto &id : result.removed) {
			if (const auto entry = copies.find(id)) {
				RemoveFilesOf(*entry);
			}
		}
		finalise = std::move(result.finalise);
		SaveLocked(store);
	}
	const auto key = RecordingKey();
	crl::async([=] {
		for (const auto &id : finalise) {
			auto copy = std::optional<Entry>();
			{
				const auto lock = std::lock_guard(TheStore().mutex);
				if (const auto entry = TheStore().manifest.find(id)) {
					copy = *entry;
				}
			}
			if (!copy) {
				continue;
			}
			const auto result = RecoverFiles(
				copy->kind,
				RecordingPath(copy->media),
				JournalPath(*copy),
				key,
				false);
			crl::on_main([=] {
				auto &store = TheStore();
				{
					const auto lock = std::lock_guard(store.mutex);
					const auto entry = store.manifest.find(id);
					if (!entry) {
						return;
					}
					const auto recovered = (result.state != State::Unrecoverable);
					if (DropAfterFinalise(*entry, recovered, result.durationMs)) {
						RemoveLocked(store, id);
					} else {
						entry->durationMs = result.durationMs;
						entry->partial = entry->partial || result.partial;
						if (!recovered && InBin(entry->state)) {
							entry->state = State::Unrecoverable;
						}
					}
					SaveLocked(store);
				}
				Fire();
			});
		}
	});
	Fire();
}

void FinishVoice() {
	FinishClosed(VoiceMirror().closeAll());
}

} // namespace

void ForgetRecordingsAtLogout(Main::Account &account) {
	auto index = -1;
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account.get() == &account) {
			index = entry.index;
		}
	}
	auto &store = TheStore();
	auto removed = 0;
	{
		const auto lock = std::lock_guard(store.mutex);
		for (const auto &id : OfAccount(store.manifest, index)) {
			SettleLocked(store, id);
			++removed;
		}
		if (removed) {
			SaveLocked(store);
		}
	}
	if (removed) {
		LOG(("Purple Recording: logout removed %1 recordings.").arg(removed));
		Fire();
	}
}

void StartRecordingStore() {
	static auto started = false;
	if (std::exchange(started, true)) {
		return;
	}
	static auto lifetime = rpl::lifetime();
	StartRoundCopies();
	Core::App().domain().accountsChanges(
	) | rpl::on_next(Refresh, lifetime);
	Refresh();
}

std::shared_ptr<const RecordingCrypt::Key> RecordingKey() {
	const auto lock = std::lock_guard(TheStore().mutex);
	return TheStore().key;
}

RecordingCrypt::RandomFill RecordingRandom() {
	return [](std::uint8_t *data, std::size_t size) {
		auto random = DesktopArchiveCrypto::Random(int(size));
		if (!random || random->size() != int(size)) {
			return false;
		}
		std::memcpy(data, random->constData(), size);
		DesktopArchiveCrypto::Clear(*random);
		return true;
	};
}

std::filesystem::path RecordingPath(const std::string &name) {
	return Native(Directory() + QString::fromStdString(name));
}

std::optional<std::string> BeginRecordingEntry(
		Kind kind,
		const std::string &mediaSuffix,
		const std::string &journalSuffix,
		const std::string &group) {
	auto &store = TheStore();
	auto id = NewId();
	if (id.empty()) {
		return std::nullopt;
	}
	{
		const auto lock = std::lock_guard(store.mutex);
		if (!store.loaded || !store.key) {
			return std::nullopt;
		}
		auto error = std::error_code();
		std::filesystem::create_directories(Native(Directory()), error);
		store.manifest.entries.push_back({
			.id = id,
			.kind = kind,
			.started = base::unixtime::now(),
			.media = id + mediaSuffix,
			.group = (group.empty() && kind == Kind::Round) ? id : group,
			.journal = journalSuffix.empty() ? std::string() : (id + journalSuffix),
			.state = State::Recording,
		});
		SaveLocked(store);
	}
	crl::on_main([=] {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return;
		}
		const auto chat = controller->activeChatCurrent();
		const auto peer = chat.peer();
		const auto topic = chat.topic();
		const auto account = RecordingAccountIndex(&controller->session());
		const auto user = controller->session().userId().bare;
		UpdateRecordingEntry(id, [&](Entry &entry) {
			entry.account = account;
			entry.user = user;
			entry.peer = peer ? peer->id.value : 0;
			entry.topic = topic ? topic->rootId().bare : 0;
		});
	});
	return id;
}

void UpdateRecordingEntry(
		const std::string &id,
		Fn<void(Entry&)> update) {
	auto &store = TheStore();
	{
		const auto lock = std::lock_guard(store.mutex);
		const auto entry = store.manifest.find(id);
		if (!entry) {
			return;
		}
		update(*entry);
		SaveLocked(store);
	}
	Fire();
}

void ForgetRecordingEntry(const std::string &id) {
	auto &store = TheStore();
	{
		const auto lock = std::lock_guard(store.mutex);
		RemoveLocked(store, id);
		SaveLocked(store);
	}
	Fire();
}

void MirrorRecordingWrite(
		const void *owner,
		int64 offset,
		const uint8_t *data,
		int size) {
	if (size > 0) {
		VoiceMirror().write(owner, offset, data, std::size_t(size));
	}
}

void NoteRecordingStop(
		not_null<Main::Session*> session,
		int type,
		bool viewOnce) {
	const auto kind = StopKindFromOrdinal(type);
	if (!kind) {
		return;
	}
	const auto listen = (*kind == StopKind::Listen);
	auto ids = listen
		? VoiceMirror().activeIds()
		: VoiceMirror().requestCloseAll();
	for (auto &id : (listen ? RoundCopyIds() : RequestRoundCopiesClose())) {
		ids.push_back(std::move(id));
	}
	if (ids.empty()) {
		return;
	}
	const auto account = RecordingAccountIndex(session);
	auto &store = TheStore();
	{
		const auto lock = std::lock_guard(store.mutex);
		for (auto &id : NoteStop(
				store.manifest,
				ids,
				*kind,
				ActiveSettings().recording.keepDiscarded,
				viewOnce,
				account,
				base::unixtime::now())) {
			store.deleteAfterClose.push_back(std::move(id));
		}
		SaveLocked(store);
	}
	if (!listen) {
		FinishRoundCopiesLater();
	}
	Fire();
}

void FinishRecordingFiles(std::vector<RecordingMirror::Closed> closed) {
	FinishClosed(std::move(closed));
}

std::vector<RecordingBinItem> RecordingBinItems() {
	auto &store = TheStore();
	const auto lock = std::lock_guard(store.mutex);
	auto result = std::vector<RecordingBinItem>();
	for (const auto entry : BinNewestFirst(store.manifest)) {
		auto duration = std::int64_t();
		auto partial = false;
		for (const auto &part : GroupOf(store.manifest, entry->id)) {
			const auto one = store.manifest.find(part);
			duration += one->durationMs;
			partial = partial
				|| one->partial
				|| (one->state == State::Unrecoverable);
		}
		const auto mark = MarkOf(*entry);
		const auto session = RecordingAccountSession(entry->account);
		const auto own = session
			&& (!entry->user || session->userId().bare == entry->user);
		result.push_back({
			.id = QString::fromStdString(entry->id),
			.account = own ? entry->account : -1,
			.peer = PeerId(PeerIdHelper(entry->peer)),
			.topic = MsgId(entry->topic),
			.reply = MsgId(entry->reply),
			.kind = entry->kind,
			.mark = (partial && mark == Mark::Recovered) ? Mark::Partial : mark,
			.started = TimeId(entry->started),
			.duration = crl::time(duration),
			.viewOnce = entry->viewOnce,
		});
	}
	return result;
}

int RecordingBinCount() {
	auto &store = TheStore();
	const auto lock = std::lock_guard(store.mutex);
	return int(BinNewestFirst(store.manifest).size());
}

rpl::producer<> RecordingBinChanges() {
	return TheStore().changes.events();
}

std::optional<QByteArray> RecordingBinMedia(const QString &id) {
	auto paths = std::vector<std::filesystem::path>();
	{
		auto &store = TheStore();
		const auto lock = std::lock_guard(store.mutex);
		const auto entry = store.manifest.find(id.toStdString());
		if (!entry || !InBin(entry->state)) {
			return std::nullopt;
		}
		for (const auto &part : GroupOf(store.manifest, entry->id)) {
			const auto one = store.manifest.find(part);
			if (one->state != State::Unrecoverable || part == entry->id) {
				paths.push_back(RecordingPath(one->media));
			}
		}
	}
	auto parts = std::vector<RoundMedia::Bytes>();
	for (const auto &path : paths) {
		auto bytes = RecordingCrypt::ReadWhole(path, RecordingKey());
		if (!bytes) {
			return std::nullopt;
		}
		parts.push_back(std::move(*bytes));
	}
	if (parts.size() > 1) {
		auto joined = RoundMedia::Join(parts);
		if (!joined) {
			return std::nullopt;
		}
		parts = { std::move(joined->bytes) };
	}
	return parts.empty()
		? std::nullopt
		: std::optional(QByteArray(
			reinterpret_cast<const char*>(parts.front().data()),
			qsizetype(parts.front().size())));
}

void DeleteRecordingBinItem(const QString &id) {
	auto parts = std::vector<std::string>();
	{
		const auto lock = std::lock_guard(TheStore().mutex);
		parts = GroupOf(TheStore().manifest, id.toStdString());
	}
	for (const auto &part : parts) {
		ForgetRecordingEntry(part);
	}
}

void ClearRecordingBin() {
	auto &store = TheStore();
	{
		const auto lock = std::lock_guard(store.mutex);
		auto ids = std::vector<std::string>();
		for (const auto &entry : store.manifest.entries) {
			if (InBin(entry.state)) {
				ids.push_back(entry.id);
			}
		}
		for (const auto &id : ids) {
			RemoveLocked(store, id);
		}
		SaveLocked(store);
	}
	Fire();
}

void SendRecordingBinItem(
		const QString &id,
		not_null<Data::Thread*> thread,
		Fn<void(bool)> done) {
	const auto bytes = RecordingBinMedia(id);
	auto kind = Kind::Voice;
	auto duration = crl::time();
	auto viewOnce = false;
	{
		auto &store = TheStore();
		const auto lock = std::lock_guard(store.mutex);
		if (const auto entry = store.manifest.find(id.toStdString())) {
			kind = entry->kind;
			duration = entry->durationMs;
			viewOnce = entry->viewOnce;
		}
	}
	if (!bytes || bytes->isEmpty()) {
		done(false);
		return;
	}
	const auto weak = base::make_weak(thread);
	crl::async([=, content = *bytes]() mutable {
		auto waveform = VoiceWaveform();
		auto length = duration;
		if (kind == Kind::Round) {
			const auto data = reinterpret_cast<const std::uint8_t*>(content.constData());
			const auto standard = RoundMedia::Remux(
				RoundMedia::Bytes(data, data + content.size()));
			if (!standard || standard->videoPackets <= 0) {
				crl::on_main([=] { done(false); });
				return;
			}
			content = QByteArray(
				reinterpret_cast<const char*>(standard->bytes.data()),
				qsizetype(standard->bytes.size()));
			length = standard->durationMs ? standard->durationMs : duration;
		} else {
			auto edit = ::Media::TrimAudioToRange(
				content,
				0,
				duration ? duration : kLongestVoice);
			waveform = std::move(edit.waveform);
			length = edit.duration ? edit.duration : duration;
		}
		crl::on_main([=] {
			const auto strong = weak.get();
			if (!strong) {
				done(false);
				return;
			}
			auto options = Api::SendOptions();
			if (viewOnce && strong->peer()->isUser()) {
				options.ttlSeconds = std::numeric_limits<int>::max();
			}
			auto action = Api::SendAction(strong, options);
			const auto session = &strong->session();
			session->api().sendVoiceMessage(
				content,
				waveform,
				length,
				(kind == Kind::Round),
				action);
			auto parts = std::vector<std::string>();
			{
				const auto lock = std::lock_guard(TheStore().mutex);
				parts = GroupOf(TheStore().manifest, id.toStdString());
			}
			for (const auto &part : parts) {
				UpdateRecordingEntry(part, [&](Entry &entry) {
					entry.state = State::Sending;
					entry.fromListen = false;
					entry.account = RecordingAccountIndex(session);
					entry.user = session->userId().bare;
					entry.peer = strong->peer()->id.value;
					entry.topic = strong->topicRootId().bare;
				});
			}
			done(true);
		});
	});
}

Main::Session *RecordingAccountSession(int account) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.index == account) {
			return entry.account->maybeSession();
		}
	}
	return nullptr;
}

int RecordingAccountIndex(not_null<Main::Session*> session) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account.get() == &session->account()) {
			return entry.index;
		}
	}
	return -1;
}

} // namespace Purple
