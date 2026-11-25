#include "purple/purple_archive_service.h"

#include "ui/style/style_core_direction.h"
#include "purple/purple_recording_store.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "config.h"
#include "core/application.h"
#include "core/version.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_config.h"
#include "purple/purple_notices.h"
#include "purple/purple_markdown_source.h"
#include "purple/purple_copy.h"
#include "purple/purple_markdown_delivery.h"
#include "ui/text/text_utilities.h"
#include "settings.h"
#include "storage/storage_account.h"
#include "storage/details/storage_file_utilities.h"
#include <QtCore/QPointer>
#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Purple {
namespace {

rpl::event_stream<> Changes;

QString Slot(Main::Account &account) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account.get() == &account) {
			return QString::number(entry.index);
		}
	}
	return {};
}

DesktopArchiveOptions Options(Main::Account &account) {
	auto result = DesktopArchiveOptions();
	result.archiveRoot = ConfigDirectory() + u"/content-archive"_q;
	result.profileRoot = cWorkingDir() + u"tdata"_q;
	result.profileNamespace = cDataFile();
	result.slot = Slot(account);
	result.account = account.maybeSession() ? SessionAccountKey(account.maybeSession())
											: AccountKey{0, 1};
	if (account.maybeSession()) {
		const auto &mtp = account.mtp();
		auto keys = std::set<QByteArray>();
		for (const auto &key : mtp.getKeysForWrite()) {
			if (!key || key->type() == MTP::AuthKey::Type::Temporary
				|| key->type() == MTP::AuthKey::Type::Local) {
				continue;
			}
			const auto id = QByteArray::number(key->keyId(), 16).rightJustified(16, '0');
			keys.insert(id);
			if (key->dcId() == mtp.mainDcId()) {
				result.loginAuthKeyId = id;
			}
		}
		for (const auto &id : keys) {
			if (!result.loginAuthKeyIds.isEmpty()) {
				result.loginAuthKeyIds += ',';
			}
			result.loginAuthKeyIds += id;
		}
	}
	return result;
}

bool ArchiveFailureNeedsToast(DesktopArchiveStatus status,
		const QString &failure, bool alreadyWarned) {
	return !alreadyWarned && status != DesktopArchiveStatus::Ready
		&& status != DesktopArchiveStatus::Disabled
		&& status != DesktopArchiveStatus::Maintenance
		&& status != DesktopArchiveStatus::RemovalPending
		&& status != DesktopArchiveStatus::RetentionGap
		&& !failure.isEmpty();
}

void ShowArchiveRemovalPending() {
	ShowPurpleNotice({
		.appScoped = true,
		.scope = u"Local archive"_q,
		.summary = tr::lng_purple_archive_removal_pending(tr::now),
		.coalesceKey = QString::fromLatin1(ModalNotices::kArchiveRemovalNoticeKey),
		.severity = ModalNotices::NoticeSeverity::Warning,
		.background = true,
	});
}

void ShowArchiveDiscardFailure(Main::Session *session, const QString &reason) {
	LOG(("Purple Archive: event=discard_failed reason=%1 count=1.").arg(reason));
	ShowPurpleNotice({
		.session = session,
		.scope = u"Local archive"_q,
		.summary = tr::lng_purple_archive_discard_failed(tr::now) + u" ["_q + reason + ']',
		.coalesceKey = u"archive:discard_failed:"_q + PurpleNoticeAccountKey(session),
		.severity = ModalNotices::NoticeSeverity::Error,
		.background = true,
	});
}

constexpr auto kLoginAuthorityPref = "purple.archive.login_authority.v1";
constexpr auto kLoginStorePref = "purple.archive.login_store.v1";
constexpr auto kLoginAuthKeyPref = "purple.archive.login_auth_key.v1";

std::optional<DesktopArchiveLoginAuthority> ArchiveLoginAuthority(const QByteArray &nonce,
	const QByteArray &storedKeys, const QByteArray &liveKeys) {
	return DesktopArchiveSelectLoginAuthority(nonce, storedKeys, liveKeys);
}

std::optional<QByteArray> ReadAuthorityFile(const QString &base) {
	auto paths = QStringList();
	if (QFileInfo::exists(base + 's')) {
		paths.push_back(base + 's');
	} else {
		paths = {base + '0', base + '1'};
		if (QFileInfo(paths[0]).lastModified() < QFileInfo(paths[1]).lastModified()) {
			std::swap(paths[0], paths[1]);
		}
	}
	for (const auto &path : paths) {
		const auto identity = DesktopArchiveIdentity(path);
		auto file = QFile(path);
		if (identity.isEmpty() || !file.open(QIODevice::ReadOnly)
			|| file.size() < 24 || file.size() > 32 * 1024 * 1024) {
			continue;
		}
		const auto frame = file.read(32 * 1024 * 1024 + 1);
		if (file.error() != QFileDevice::NoError || !file.atEnd()
			|| frame.size() > 32 * 1024 * 1024 || frame.size() != file.size()
			|| DesktopArchiveIdentity(path) != identity
			|| frame.first(4) != QByteArray("TDF$")) {
			continue;
		}
		auto version = qint32(0);
		std::memcpy(&version, frame.constData() + 4, sizeof(version));
		if (version > AppVersion) {
			continue;
		}
		const auto size = qint32(frame.size() - 24);
		auto hash = QCryptographicHash(QCryptographicHash::Md5);
		hash.addData(QByteArrayView(frame.constData() + 8, size));
		hash.addData(QByteArrayView(reinterpret_cast<const char*>(&size), sizeof(size)));
		hash.addData(QByteArrayView(reinterpret_cast<const char*>(&version), sizeof(version)));
		hash.addData(QByteArrayView(frame.constData(), 4));
		if (hash.result() == frame.last(16)) {
			return frame.mid(8, size);
		}
	}
	return {};
}

bool PersistedLoginAuthority(const QString &root, const QString &dataName,
	const MTP::AuthKeyPtr &key, const QByteArray &authority,
	const QByteArray &authKeyId) {
	using namespace Storage::details;
	const auto digest = QCryptographicHash::hash(dataName.toUtf8(), QCryptographicHash::Md5);
	auto name = uint64(0);
	std::memcpy(&name, digest.constData(), sizeof(name));
	const auto directory = root + '/' + ToFilePart(name) + '/';
	auto mapBytes = ReadAuthorityFile(directory + u"map"_q);
	if (!mapBytes) {
		return false;
	}
	auto mapStream = QDataStream(&*mapBytes, QIODevice::ReadOnly);
	mapStream.setVersion(QDataStream::Qt_5_1);
	auto salt = QByteArray();
	auto encryptedKey = QByteArray();
	auto encryptedMap = QByteArray();
	mapStream >> salt >> encryptedKey >> encryptedMap;
	auto map = EncryptedDescriptor();
	if (mapStream.status() != QDataStream::Ok || !DecryptLocal(map, encryptedMap, key)) {
		return false;
	}
	auto prefsKey = quint64(0);
	for (auto prefix = 0; prefix != 4 && !map.stream.atEnd(); ++prefix) {
		auto type = quint32(0);
		map.stream >> type;
		if (type == 0x15) {
			auto self = QByteArray();
			map.stream >> self;
		} else if (type == 0x01 || type == 0x02) {
			auto count = quint32(0);
			map.stream >> count;
			if (count > 100000 || map.stream.skipRawData(int(count) * 16) != int(count) * 16) {
				return false;
			}
		} else if (type == 0x1e) {
			map.stream >> prefsKey;
			break;
		} else {
			return false;
		}
	}
	if (!prefsKey || map.stream.status() != QDataStream::Ok) {
		return false;
	}
	auto prefsBytes = ReadAuthorityFile(directory + ToFilePart(prefsKey));
	if (!prefsBytes) {
		return false;
	}
	auto prefsStream = QDataStream(&*prefsBytes, QIODevice::ReadOnly);
	prefsStream.setVersion(QDataStream::Qt_5_1);
	auto encryptedPrefs = QByteArray();
	prefsStream >> encryptedPrefs;
	auto prefs = EncryptedDescriptor();
	if (prefsStream.status() != QDataStream::Ok
		|| !DecryptLocal(prefs, encryptedPrefs, key)) {
		return false;
	}
	auto count = quint32(0);
	prefs.stream >> count;
	if (count > 100000) {
		return false;
	}
	auto found = false;
	auto foundAuthKey = false;
	for (auto i = quint32(0); i != count; ++i) {
		auto nameSize = quint32(0);
		auto valueSize = quint32(0);
		prefs.stream >> nameSize >> valueSize;
		if (nameSize > 4096 || valueSize > 32 * 1024 * 1024) {
			return false;
		}
		auto name = QByteArray(int(nameSize), Qt::Uninitialized);
		auto value = QByteArray(int(valueSize), Qt::Uninitialized);
		if (prefs.stream.readRawData(name.data(), name.size()) != name.size()
			|| prefs.stream.readRawData(value.data(), value.size()) != value.size()) {
			return false;
		}
		if (name == kLoginAuthorityPref) {
			if (found || value != authority) {
				return false;
			}
			found = true;
		} else if (name == kLoginAuthKeyPref) {
			if (foundAuthKey || value != authKeyId) {
				return false;
			}
			foundAuthKey = true;
		}
	}
	return found && foundAuthKey && prefs.stream.status() == QDataStream::Ok && prefs.stream.atEnd();
}

class ArchiveWorker final {
  public:
	using Task = std::function<void(DesktopArchiveStore &)>;
	struct Measurement {
		uint64_t bytes;
		bool complete;
	};
	using Report = Fn<void(DesktopArchiveStatus, QString, std::optional<Measurement>)>;
	ArchiveWorker(DesktopArchiveOptions options,
		Report report,
		Fn<void(std::shared_ptr<DesktopArchiveJournal>)> ready,
		Fn<void(DesktopArchiveCaptureLosses)> captureLosses = {})
		: _state(std::make_shared<State>()) {
		_state->report = std::move(report);
		_state->ready = std::move(ready);
		_state->captureLosses = std::move(captureLosses);
		options.revocableSecrets = _state->secrets;
		options.active = [state = _state] { return !state->cancelled.load(); };
		std::thread([state = _state, options = std::move(options)]() mutable {
			Run(std::move(state), std::move(options));
		}).detach();
	}
	static std::unique_ptr<ArchiveWorker> RemovalOnly(DesktopArchiveOptions options,
		bool resolveSlot, Report report) {
		auto result = std::unique_ptr<ArchiveWorker>(new ArchiveWorker());
		const auto state = result->_state;
		state->report = std::move(report);
		state->cancelled = true;
		state->remove = true;
		state->resolveSlot = resolveSlot;
		std::thread([state, options = std::move(options)]() mutable {
			Run(state, std::move(options));
		}).detach();
		return result;
	}
	~ArchiveWorker() { Stop(false); }
	bool Post(Task task, bool cleanup = false) {
		const auto lock = std::unique_lock(_state->mutex, std::try_to_lock);
		if (!lock.owns_lock() || _state->cancelled
			|| _state->tasks.size() - _state->queuedStreams >= (cleanup ? 80 : 64)) {
			return false;
		}
		_state->tasks.push_back({
			[task = std::move(task)](DesktopArchiveStore &store,
					const std::shared_ptr<DesktopArchiveJournal> &) { task(store); }, 0});
		_state->condition.notify_one();
		return true;
	}
	enum class StreamAdmission { Queued, Retired, Full };
	StreamAdmission PostStream(Task task, uint64_t bytes) {
		constexpr auto kPendingBytes = uint64_t(16 * 1024 * 1024);
		constexpr auto kPendingParts = 128;
		const auto lock = std::lock_guard(_state->mutex);
		if (_state->cancelled) {
			return StreamAdmission::Retired;
		}
		if (_state->pendingStreams >= kPendingParts
			|| bytes > kPendingBytes - _state->streamBytes) {
			return StreamAdmission::Full;
		}
		_state->tasks.push_back({
			[task = std::move(task)](DesktopArchiveStore &store,
					const std::shared_ptr<DesktopArchiveJournal> &) { task(store); },
			0, bytes });
		_state->streamBytes += bytes;
		++_state->queuedStreams;
		++_state->pendingStreams;
		_state->condition.notify_one();
		return StreamAdmission::Queued;
	}
	bool NeedsStartupCapture() const { return _state->startupCount.load() != 0; }
	bool StartupFailed() const { return _state->startupFailed.load(); }
	bool StartupHeld() const { return _state->startupHeld.load(); }
	bool PostStartupCapture(ArchiveEvent event, int byteLimit) {
		const auto bytes = DesktopArchiveJournal::PendingCaptureBytes(event);
		const auto lock = std::unique_lock(_state->mutex, std::try_to_lock);
		if (!bytes || *bytes > 4 * 1024 * 1024 || !lock.owns_lock()
			|| _state->cancelled || _state->startupFailed || _state->purgeRequested
			|| _state->tasks.size() - _state->queuedStreams >= 64
			|| _state->startupCount >= 32
			|| *bytes > 16 * 1024 * 1024 - _state->startupBytes) {
			return false;
		}
		const auto weak = std::weak_ptr<State>(_state);
		_state->tasks.push_back({[weak, byteLimit, event = std::move(event)](
				DesktopArchiveStore &store,
				const std::shared_ptr<DesktopArchiveJournal> &journal) mutable {
			const auto state = weak.lock();
			if (!state || state->cancelled) {
				return;
			}
			const auto gap = [&](const QString &reason) {
				if (!store.Gap(event.id, reason, event.observation.observedAt)) {
					store.NoteCaptureLoss(reason);
				}
			};
			if (!journal) {
				if (!state->startupHeld) {
					gap(u"capture-startup-unavailable"_q);
				}
				return;
			}
			if (state->purgeRequested || !store.Current()
				|| store.MaintenancePending() || !journal->CaptureAllowed()
				|| !journal->access().current()
				|| !CanRunArchiveWork(journal->access().epoch, store.epoch())) {
				gap(u"capture-startup-unavailable"_q);
				return;
			}
			journal->BindCaptureGeneration(event);
			if (!journal->Append(event, byteLimit)) {
				const auto reason = journal->failure().isEmpty()
					? u"capture-startup-append"_q : journal->failure();
				gap(reason);
				return;
			}
			static_cast<void>(journal->Drain(store));
		}, *bytes});
		_state->startupBytes += *bytes;
		++_state->startupCount;
		_state->condition.notify_one();
		return true;
	}
	void PruneStreamsSoon() {
		const auto lock = std::lock_guard(_state->mutex);
		_state->streamCleanup = true;
		_state->condition.notify_one();
	}
	std::shared_ptr<std::atomic<uint64_t>> DeliveryFence() const {
		return _state->delivery;
	}
	void RevokeOnStop(const std::shared_ptr<std::atomic<bool>> &active) {
		const auto lock = std::lock_guard(_state->mutex);
		if (_state->cancelled) {
			active->store(false);
			return;
		}
		std::erase_if(_state->mediaReaders, [](const auto &value) { return value.expired(); });
		_state->mediaReaders.push_back(active);
	}
	bool ClaimPurge() { return !_state->purgeRequested.exchange(true); }
	void ReleasePurgeClaim() { _state->purgeRequested = false; }
	bool Removed() const { return _state->removed; }
	bool WaitForStopped(std::chrono::milliseconds timeout) {
		auto lock = std::unique_lock(_state->mutex);
		return _state->condition.wait_for(lock, timeout,
			[&] { return _state->finished.load(); });
	}
	bool Stop(bool remove) {
		_state->cancelled = true;
		_state->delivery->fetch_add(1);
		if (remove) {
			_state->remove = true;
		}
		_state->secrets->Revoke();
		{
			const auto lock = std::lock_guard(_state->mutex);
			for (const auto &reader : _state->mediaReaders) {
				if (const auto live = reader.lock()) {
					live->store(false);
				}
			}
			_state->mediaReaders.clear();
			for (const auto &task : _state->tasks) {
				if (task.startupBytes) {
					_state->startupBytes -= task.startupBytes;
					--_state->startupCount;
				}
				if (task.streamBytes) {
					_state->streamBytes -= *task.streamBytes;
					--_state->pendingStreams;
				}
			}
			_state->tasks.clear();
			_state->queuedStreams = 0;
		}
		_state->condition.notify_all();
		return true;
	}

  private:
	ArchiveWorker() : _state(std::make_shared<State>()) {}
	struct QueuedTask {
		std::function<void(DesktopArchiveStore &,
			const std::shared_ptr<DesktopArchiveJournal> &)> run;
		uint64_t startupBytes = 0;
		std::optional<uint64_t> streamBytes;
	};
	struct State {
		Report report;
		Fn<void(std::shared_ptr<DesktopArchiveJournal>)> ready;
		Fn<void(DesktopArchiveCaptureLosses)> captureLosses;
		std::mutex mutex;
		std::condition_variable condition;
		std::deque<QueuedTask> tasks;
		uint64_t startupBytes = 0;
		uint64_t streamBytes = 0;
		size_t queuedStreams = 0;
		int pendingStreams = 0;
		std::atomic<int> startupCount = 0;
		std::atomic<bool> startupFailed = false;
		std::atomic<bool> startupHeld = false;
		std::vector<std::weak_ptr<std::atomic<bool>>> mediaReaders;
		std::atomic<bool> cancelled = false;
		std::atomic<bool> remove = false;
		std::atomic<bool> removed = false;
		std::atomic<bool> finished = false;
		std::atomic<bool> streamCleanup = false;
		bool resolveSlot = false;
		std::atomic<bool> purgeRequested = false;
		std::shared_ptr<DesktopArchiveCrypto> secrets
			= std::make_shared<DesktopArchiveCrypto>();
		std::shared_ptr<std::atomic<uint64_t>> delivery
			= std::make_shared<std::atomic<uint64_t>>(0);
	};
	static void Run(std::shared_ptr<State> state, DesktopArchiveOptions options) {
		auto removalOptions = options;
		if (removalOptions.removalAuthority.isEmpty()) {
			removalOptions.removalAuthority = options.loginAuthority.size() == 32
				? QString::fromLatin1(QCryptographicHash::hash(
					options.loginAuthority, QCryptographicHash::Sha256).toHex())
				: u"unavailable"_q;
		}
		DesktopArchiveCrypto::Clear(removalOptions.localKey);
		DesktopArchiveCrypto::Clear(removalOptions.loginAuthority);
		removalOptions.authorityReady = {};
		removalOptions.revocableSecrets.reset();
		auto ownedStore = std::make_unique<DesktopArchiveStore>();
		auto &store = *ownedStore;
		auto journal = std::shared_ptr<DesktopArchiveJournal>();
		auto journalFailure = QString();
		auto journalOpened = false;
		auto startupHeld = false;
		auto retryDiscards = false;
		auto startupJournal = std::shared_ptr<DesktopArchiveJournal>();
		auto authorityReady = !options.authorityReady;
		for (const auto delay : {0, 100, 250, 500, 1000, 2000, 3000}) {
			if (state->cancelled || authorityReady) {
				break;
			}
			if (delay) {
				auto lock = std::unique_lock(state->mutex);
				state->condition.wait_for(lock, std::chrono::milliseconds(delay),
					[&] { return state->cancelled.load(); });
			}
			if (!state->cancelled) {
				authorityReady = options.authorityReady();
			}
		}
		options.authorityReady = {};
		auto discard = DesktopArchiveDiscardResult();
		auto showingDiscard = false;
		while (authorityReady && !state->cancelled) {
			discard = DesktopArchiveStore::ResumeDiscard(options, [&] {
				if (!std::exchange(showingDiscard, true)) {
					state->report(DesktopArchiveStatus::RemovalPending, u"discard-cleanup-pending"_q, std::nullopt);
				}
			});
			if (!discard.retryable) {
				break;
			}
			state->report(DesktopArchiveStatus::RemovalPending, discard.reason, std::nullopt);
			auto lock = std::unique_lock(state->mutex);
			state->condition.wait_for(lock, std::chrono::seconds(1),
				[&] { return state->cancelled.load(); });
		}
		if (!state->cancelled) {
			if (!authorityReady) {
				state->startupFailed = true;
				state->startupHeld = true;
				startupHeld = true;
				state->report(DesktopArchiveStatus::Unreadable, u"login-authority-held"_q, std::nullopt);
			} else if (!discard) {
				state->startupFailed = true;
				state->startupHeld = true;
				startupHeld = true;
				state->report(DesktopArchiveStatus::Unreadable, discard.reason, std::nullopt);
			} else if (!DesktopArchiveStore::Recover(options, &retryDiscards)) {
				state->startupFailed = true;
				state->startupHeld = true;
				startupHeld = true;
				state->report(DesktopArchiveStatus::RemovalPending, u"logout-recovery"_q, std::nullopt);
			} else {
				static_cast<void>(store.Open(std::move(options)));
				if (!store.epoch().storeId.isEmpty()) {
					removalOptions.removalStore = store.epoch().storeId;
				}
				if (const auto access = store.JournalAccess()) {
					journal = std::make_shared<DesktopArchiveJournal>(*access);
					journalOpened = journal->Open();
					if (journalOpened && journal->Drain(store)) {
						startupJournal = journal;
						if (!state->cancelled) {
							state->ready(journal);
						}
					} else {
						journalFailure = journalOpened ? journal->replayFailure() : journal->failure();
						if (journalFailure.isEmpty()) {
							journalFailure = u"capture-recovery-held"_q;
						}
					}
				}
			}
		}
		if (!startupJournal) {
			state->startupFailed = true;
		}
		DesktopArchiveCrypto::Clear(options.localKey);
		DesktopArchiveCrypto::Clear(options.loginAuthority);
		auto nextJournalRenewal = std::chrono::steady_clock::time_point();
		auto writerRetirementHeld = false;
		const auto renewJournal = [&] {
			const auto recovery = store.JournalRenewalPending();
			if (state->cancelled || state->purgeRequested || !store.Current()
				|| store.JournalRenewalHeld() || writerRetirementHeld
				|| (!recovery && journal && journal->CaptureAllowed())) {
				return;
			}
			if (recovery) {
				const auto now = std::chrono::steady_clock::now();
				if (now < nextJournalRenewal) return;
				nextJournalRenewal = now + std::chrono::seconds(5);
				if (journal) journal->PauseCapture();
			}
			if (journal && !journal->RetireWriter()) {
				journalFailure = u"capture-admission-writer-retirement-held"_q;
				if (recovery) {
					store.RefuseJournalRenewal(journalFailure);
				} else {
					writerRetirementHeld = true;
					journalFailure = u"purge-writer-retirement-held"_q;
				}
				return;
			}
			if (state->cancelled) return;
			const auto access = recovery ? store.RenewJournalAccess() : store.JournalAccess();
			const auto fresh = access ? std::make_shared<DesktopArchiveJournal>(*access) : nullptr;
			const auto opened = fresh && fresh->Open() && fresh->Drain(store);
			const auto accepted = recovery ? store.CompleteJournalRenewal(opened) : opened;
			if (!accepted) {
				journalFailure = recovery ? store.failure() : u"purge-journal-renewal-held"_q;
				return;
			}
			journal = fresh;
			journalOpened = true;
			journalFailure.clear();
			state->startupFailed = false;
			state->ready(fresh);
		};
		const auto drainJournal = [&] {
			if (!journalOpened || !journal || journal->WriterRetired()
				|| !store.Current() || !journal->access().current()) {
				return;
			}
			if (journal->Drain(store)) {
				journalFailure.clear();
				if (state->startupFailed.exchange(false)) {
					startupJournal = journal;
					state->ready(journal);
				}
			} else {
				journalFailure = journal->replayFailure();
			}
		};

		auto measurement = std::optional<Measurement>();
		auto nextUsage = std::chrono::steady_clock::time_point();
		auto nextReport = std::chrono::steady_clock::time_point();
		auto reportedStatus = DesktopArchiveStatus::Disabled;
		auto reportedFailure = QString();
		const auto report = [&] {
			if (state->cancelled || startupHeld) {
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			const auto failure = journalFailure.isEmpty() ? store.failure() : journalFailure;
			const auto captureStopped = !journalFailure.isEmpty()
				&& (!journalOpened || writerRetirementHeld || store.JournalRenewalHeld()
					|| journalFailure == u"outbox-entry"_q || journalFailure == u"outbox-evidence"_q);
			const auto status = captureStopped && store.status() == DesktopArchiveStatus::Ready
				? DesktopArchiveStatus::Unreadable : store.status();
			if (now < nextReport && reportedStatus == status && reportedFailure == failure) {
				return;
			}
			nextReport = now + std::chrono::seconds(1);
			reportedStatus = status;
			reportedFailure = failure;
			if (store.Current() && (!measurement || !measurement->complete || now >= nextUsage)) {
				const auto usage = store.Usage();
				if (const auto bytes = usage ? TotalArchiveUsage(*usage) : std::nullopt) {
					measurement = Measurement{ *bytes, store.UsageComplete() };
				}
				nextUsage = now + std::chrono::seconds(60);
			}
			auto losses = store.TakeCaptureLosses();
			if (state->captureLosses && !losses.empty()) {
				state->captureLosses(std::move(losses));
			}
			state->report(status, failure, measurement);
		};
		report();
		auto streamCursor = QString();
		auto streamMore = store.Current();
		auto nextViewsExpiry = std::chrono::steady_clock::time_point();
		auto garbageMore = store.Current();
		auto maintenanceMore = store.MaintenancePending() && !store.MaintenanceHeld();
		for (;;) {
			auto task = QueuedTask();
			auto idle = false;
			auto background = false;
			{
				auto lock = std::unique_lock(state->mutex);
				state->condition.wait_for(lock, std::chrono::seconds(5), [&] {
					return state->cancelled.load() || !state->tasks.empty() || streamMore || maintenanceMore || garbageMore
						|| state->streamCleanup.load();
				});
				if (state->cancelled) {
					break;
				}
				if (state->streamCleanup.exchange(false)) {
					task.run = [](DesktopArchiveStore &store, const auto &) { store.PruneStreams(); };
				} else if (state->tasks.empty() && maintenanceMore) {
					background = true;
					task.run = [&](DesktopArchiveStore &store, const auto &) {
						const auto step = store.ContinuePurge();
						if (!store.MaintenancePending()) state->purgeRequested = false;
						if (step.valid && step.finished) {
							garbageMore = true;
							streamCursor.clear();
							streamMore = true;
						}
					};
				} else if (state->tasks.empty() && !streamMore && garbageMore) {
					background = true;
					task.run = [&](DesktopArchiveStore &store, const auto &) {
						if (!store.ContinueGarbage(garbageMore)) {
							garbageMore = false;
						} else if (!garbageMore) {
							streamCursor.clear();
							streamMore = true;
						}
					};
				} else if (state->tasks.empty() && !streamMore) {
					idle = true;
				} else if (state->tasks.empty()) {
					task.run = [&](DesktopArchiveStore &store, const auto &) {
						if (!store.ResumeStreams(streamCursor, streamMore)) {
							streamMore = false;
							static_cast<void>(store.Gap(u"stream-recovery"_q, u"stream-recovery-held"_q, base::unixtime::now()));
						}
					};
				} else {
					task = std::move(state->tasks.front());
					state->tasks.pop_front();
					if (task.streamBytes) {
						--state->queuedStreams;
					}
				}
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextViewsExpiry) {
				static_cast<void>(store.ExpireDeletedViews());
				nextViewsExpiry = now + std::chrono::seconds(5);
			}
			if (idle) {
				if (retryDiscards) {
					retryDiscards = DesktopArchiveStore::RetryConfirmedDiscards(removalOptions);
				}
				renewJournal();
				drainJournal();
				report();
				continue;
			}
			if (!background) {
				state->delivery->fetch_add(1);
			}
			task.run(store, startupJournal);
			if (task.streamBytes) {
				task.run = {};
			}
			if (task.startupBytes || task.streamBytes) {
				const auto lock = std::lock_guard(state->mutex);
				if (task.startupBytes) {
					state->startupBytes -= task.startupBytes;
					--state->startupCount;
				}
				if (task.streamBytes) {
					state->streamBytes -= *task.streamBytes;
					--state->pendingStreams;
				}
			}
			store.PruneStreams();
			maintenanceMore = store.MaintenancePending() && !store.MaintenanceHeld();
			garbageMore = store.Current() && store.GarbagePending();
			if (store.MaintenancePending()) {
				streamMore = false;
				garbageMore = false;
			}
			renewJournal();
			drainJournal();
			report();
		}
		state->secrets->Revoke();
		static_cast<void>(store.Close());
		journal.reset();
		if (state->remove) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			removalOptions.maintenanceAllowed = [deadline] {
				return std::chrono::steady_clock::now() < deadline;
			};
			for (const auto delay : {0, 50, 200}) {
				if (delay) {
					std::this_thread::sleep_for(std::chrono::milliseconds(delay));
				}
				if (!removalOptions.maintenanceAllowed()) {
					break;
				}
				if (state->resolveSlot) {
					auto valid = false;
					const auto key = DesktopArchiveStore::AccountForSlot(
						removalOptions, removalOptions.slot, &valid);
					if (!key) {
						if (valid) {
							state->removed = true;
							break;
						}
						continue;
					}
					removalOptions.account = *key;
					state->resolveSlot = false;
				}
				if (DesktopArchiveStore::RemoveAccount(removalOptions, 64)) {
					state->removed = true;
					break;
				}
			}
			if (!state->removed) {
				state->report(DesktopArchiveStatus::RemovalPending, u"logout-removal-pending"_q, std::nullopt);
			}
		}
		ownedStore.reset();
		state->finished = true;
		state->condition.notify_all();
	}
	std::shared_ptr<State> _state;

};

struct ArchiveSession {
	std::shared_ptr<DesktopArchiveJournal> journal;
	std::shared_ptr<ArchiveWorker> worker;
	std::shared_ptr<std::atomic<bool>> resetting;
	std::shared_ptr<std::atomic<bool>> routing = std::make_shared<std::atomic<bool>>(true);
	DesktopArchiveStatus status = DesktopArchiveStatus::Disabled;
	QString failure;
	QString discardFailure;
	DesktopArchiveCaptureLosses captureLosses;
	DesktopArchiveCaptureLosses captureDetails;
	bool availabilityWarned = false;
	std::optional<ArchiveWorker::Measurement> usage;
};
std::map<Main::Session*, ArchiveSession> Sessions;
std::set<Main::Session*> Retired;

QString CaptureLossDetails(const DesktopArchiveCaptureLosses &losses) {
	auto detail = QStringList();
	for (const auto &[reason, count] : losses) {
		detail.push_back(reason + u": "_q + QLocale().toString(qulonglong(count)));
	}
	return detail.join(u", "_q);
}

bool AccumulateArchiveCaptureIncidents(const DesktopArchiveCaptureLosses &incidents,
	DesktopArchiveCaptureLosses &losses, DesktopArchiveCaptureLosses &details) {
	const auto firstLoss = losses.empty();
	for (const auto &[reason, count] : incidents) {
		auto &counts = ArchiveCaptureReasonWarns(reason) ? losses : details;
		counts[reason] += count;
	}
	return firstLoss && !losses.empty();
}

void ReportArchiveCaptureLoss(Main::Session *session, const DesktopArchiveCaptureLosses &losses) {
	const auto i = Sessions.find(session);
	if (i == Sessions.end() || !i->second.routing->load() || Retired.contains(session)) {
		return;
	}
	auto &value = i->second;
	const auto warn = AccumulateArchiveCaptureIncidents(losses, value.captureLosses, value.captureDetails);
	for (const auto &[reason, count] : losses) {
		LOG(("Purple Archive: event=%1 reason=%2 count=%3.")
			.arg(ArchiveCaptureReasonWarns(reason) ? u"capture_loss"_q : u"capture_detail"_q)
			.arg(reason).arg(count));
	}
	if (warn) {
		ShowPurpleNotice({
			.session = session,
			.scope = u"Local archive"_q,
			.summary = tr::lng_purple_archive_gap(tr::now) + '\n' + CaptureLossDetails(value.captureLosses),
			.coalesceKey = u"archive:gap:"_q + PurpleNoticeAccountKey(session),
			.severity = ModalNotices::NoticeSeverity::Warning,
			.background = true,
		});
	}
	Changes.fire({});
}

ArchiveSession &For(not_null<Main::Session*> session) {
	auto &value = Sessions[session.get()];
	if (value.worker) {
		return value;
	}
	auto options = Options(session->account());
	options.create = true;
	const auto authKeyId = options.loginAuthKeyIds;
	const auto storedNonce = session->local().readPref<QByteArray>(kLoginAuthorityPref);
	const auto storedKeyId = session->local().readPref<QByteArray>(kLoginAuthKeyPref);
	const auto login = ArchiveLoginAuthority(storedNonce, storedKeyId, authKeyId);
	if (login && (login->nonce != storedNonce || login->authKeys != storedKeyId)) {
		session->local().clearPref(kLoginStorePref);
		session->local().writePref<QByteArray>(kLoginAuthorityPref, login->nonce);
		session->local().writePref<QByteArray>(kLoginAuthKeyPref, login->authKeys);
	}
	const auto authority = login ? login->nonce : QByteArray();
	options.loginAuthority = authority;
	options.removalStore = QString::fromLatin1(session->local().readPref<QByteArray>(kLoginStorePref));
	const auto key = session->account().local().peekLegacyLocalKey();
	if (key) {
		const auto bytes = key->data();
		options.localKey
			= QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}
	auto dataName = cDataFile();
	dataName.remove(u'#');
	const auto slot = options.slot.toInt();
	if (slot > 0) {
		dataName += '#' + QString::number(slot + 1);
	}
	options.authorityReady = [key, authority, authKeyId, root = options.profileRoot, dataName] {
		return key && authority.size() == 32
			&& PersistedLoginAuthority(root, dataName, key, authority, authKeyId);
	};
	const auto weak = base::make_weak(session.get());
	const auto accountKey = PurpleNoticeAccountKey(session.get());
	const auto routing = value.routing;
	value.worker = std::make_shared<ArchiveWorker>(
		std::move(options),
		[weak, accountKey, routing, app = QPointer<Core::Application>(&Core::App())](
				DesktopArchiveStatus status, QString failure, std::optional<ArchiveWorker::Measurement> usage) {
			crl::on_main([weak, accountKey, routing, app, status, failure = std::move(failure), usage] {
				if (!app) {
					return;
				}
				if (status == DesktopArchiveStatus::RemovalPending && !failure.startsWith(u"discard-"_q)) {
					ShowArchiveRemovalPending();
				}
				if (!routing->load()) {
					return;
				}
				const auto live = weak.get();
				if (!live) {
					return;
				}
				const auto i = Sessions.find(live);
				if (i == Sessions.end()) {
					return;
				}
				auto &value = i->second;
				if (status == DesktopArchiveStatus::Ready || status == DesktopArchiveStatus::Disabled) {
					value.availabilityWarned = false;
				}
				const auto firstFailure = ArchiveFailureNeedsToast(status, failure, value.availabilityWarned);
				if (!failure.isEmpty() && (value.status != status || value.failure != failure)) {
					LOG(("Purple Archive: event=status status=%1 reason=%2 measured_bytes=%3.")
						.arg(int(status)).arg(failure).arg(usage ? usage->bytes : 0));
				}
				value.status = status;
				value.failure = failure;
				if (usage) {
					value.usage = usage;
				}
				if (firstFailure && Core::App().domain().started()) {
					value.availabilityWarned = true;
					LOG(("Purple Archive: unavailable (%1).").arg(failure));
					ShowPurpleNotice({
						.session = live,
						.scope = u"Local archive"_q,
						.summary = tr::lng_purple_archive_unavailable(tr::now) + u" ["_q + failure + ']',
						.coalesceKey = u"archive:unavailable:"_q + accountKey,
						.severity = ModalNotices::NoticeSeverity::Error,
						.background = true,
					});
				}
				Changes.fire({});
			});
		},
		[weak, routing](std::shared_ptr<DesktopArchiveJournal> journal) {
			crl::on_main([weak, routing, journal = std::move(journal)] {
				const auto live = weak.get();
				if (!live || !routing->load()) {
					return;
				}
				const auto i = Sessions.find(live);
				if (i == Sessions.end()) {
					return;
				}
				live->local().writePref<QByteArray>(kLoginStorePref, journal->access().epoch.storeId.toLatin1());
				if (i->second.journal && i->second.journal != journal) {
					InvalidateArchiveAcquisitions(live);
				}
				i->second.journal = journal;
				Changes.fire({});
			});
		},
		[weak, routing](DesktopArchiveCaptureLosses losses) {
			crl::on_main([weak, routing, losses = std::move(losses)] {
				if (weak && routing->load()) {
					ReportArchiveCaptureLoss(weak.get(), losses);
				}
			});
		});
	session->lifetime().add([pointer = session.get()] {
		Sessions.erase(pointer);
		Retired.erase(pointer);
	});
	return value;
}

}

void StartArchiveServices() {
	static auto started = false;
	if (std::exchange(started, true)) {
		return;
	}
	static auto lifetime = rpl::lifetime();
	static auto accounts = rpl::lifetime();
	const auto refresh = [] {
		accounts.destroy();
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto ready = [](Main::Session* session) {
				if (session && !Retired.contains(session)) {
					static_cast<void>(For(session));
				}
			};
			entry.account->sessionChanges() | rpl::on_next(ready, accounts);
			ready(entry.account->maybeSession());
		}
	};
	Core::App().domain().accountsChanges() | rpl::on_next(refresh, lifetime);
	// WHY: app services start before the domain, and upstream adds stored accounts without
	// accountsChanges(), so an active session change is the first sign of a normal launch's sessions.
	Core::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);
	refresh();
}

void RemoveArchiveAtLogout(Main::Account &account) {
	ForgetRecordingsAtLogout(account);
	const auto authority = account.local().readPref<QByteArray>(kLoginAuthorityPref);
	const auto store = QString::fromLatin1(account.local().readPref<QByteArray>(kLoginStorePref));
	account.local().clearPref(kLoginAuthorityPref);
	account.local().clearPref(kLoginStorePref);
	const auto session = account.maybeSession();
	if (session) {
		if (Retired.insert(session).second) {
			session->lifetime().add([session] { Retired.erase(session); });
		}
		const auto i = Sessions.find(session);
		if (i != Sessions.end() && i->second.worker) {
			const auto resetting = bool(i->second.resetting);
			i->second.routing->store(false);
			if (i->second.resetting) {
				i->second.resetting->store(false);
			}
			static_cast<void>(i->second.worker->Stop(!resetting));
			Sessions.erase(i);
			if (!resetting) {
				Changes.fire({});
				return;
			}
		}
	}
	try {
		auto options = Options(account);
		options.removalStore = store;
		options.removalAuthority = authority.size() == 32
			? QString::fromLatin1(QCryptographicHash::hash(authority, QCryptographicHash::Sha256).toHex())
			: u"unavailable"_q;
		static_cast<void>(ArchiveWorker::RemovalOnly(std::move(options), !session,
			[app = QPointer<Core::Application>(&Core::App())](DesktopArchiveStatus status, QString, std::optional<ArchiveWorker::Measurement>) {
				if (status == DesktopArchiveStatus::RemovalPending) {
					crl::on_main([app] {
						if (app) {
							ShowArchiveRemovalPending();
						}
					});
				}
			}));
	} catch (const std::system_error &) {
		ShowArchiveRemovalPending();
	}
	Changes.fire({});
}

Fn<void()> ArchiveDiscardAction(not_null<Main::Session*> session) {
	if (Retired.contains(session.get())) {
		return [] {};
	}
	const auto weak = base::make_weak(session.get());
	auto &value = For(session);
	const auto worker = value.worker;
	auto options = Options(session->account());
	options.loginAuthority = session->local().readPref<QByteArray>(kLoginAuthorityPref);
	const auto once = std::make_shared<std::atomic<bool>>(false);
	return [weak, worker, options = std::move(options), once]() mutable {
		const auto live = weak.get();
		if (!live || Retired.contains(live) || once->exchange(true)) {
			return;
		}
		const auto i = Sessions.find(live);
		if (i == Sessions.end() || i->second.worker != worker
			|| live->local().readPref<QByteArray>(kLoginAuthorityPref) != options.loginAuthority
			|| Options(live->account()).loginAuthKeyIds != options.loginAuthKeyIds
			|| (i->second.resetting && i->second.resetting->load())) {
			return;
		}
		const auto active = std::make_shared<std::atomic<bool>>(true);
		i->second.resetting = active;
		live->lifetime().add([active] { active->store(false); });
		live->mtp().writeKeysRequests() | rpl::on_next([weak, active, keys = options.loginAuthKeyIds] {
			const auto live = weak.get();
			if (!live || Options(live->account()).loginAuthKeyIds != keys) {
				active->store(false);
			}
		}, live->lifetime());
		options.active = [active] { return active->load(); };
		try {
			auto reset = std::thread([weak, worker, options, active] {
				while (!worker->WaitForStopped(std::chrono::seconds(1))) {
				}
				auto result = DesktopArchiveDiscardResult{u"discard-account-changed"_q};
				if (active->load()) {
					const auto prepared = DesktopArchiveStore::PrepareDiscard(options);
					result = prepared.token
						? DesktopArchiveStore::DiscardAccount(options, *prepared.token)
						: DesktopArchiveDiscardResult{prepared.reason};
					while (result.retryable && active->load()) {
						std::this_thread::sleep_for(std::chrono::seconds(1));
						result = DesktopArchiveStore::DiscardAccount(options, *prepared.token);
					}
				}
				crl::on_main([weak, worker, options, active, result = std::move(result)] {
					const auto live = weak.get();
					if (!live || Retired.contains(live)) {
						return;
					}
					const auto i = Sessions.find(live);
					if (i == Sessions.end() || i->second.worker != worker
						|| i->second.resetting != active) {
						return;
					}
					const auto current = active->exchange(false)
						&& live->local().readPref<QByteArray>(kLoginAuthorityPref) == options.loginAuthority
						&& Options(live->account()).loginAuthKeyIds == options.loginAuthKeyIds;
					if (result && current) {
						live->local().clearPref(kLoginAuthorityPref);
						live->local().clearPref(kLoginStorePref);
					}
					Sessions.erase(i);
					auto &restart = For(live);
					if (!result || !current) {
						restart.discardFailure = current ? result.reason : u"discard-account-changed"_q;
						ShowArchiveDiscardFailure(live, restart.discardFailure);
					}
					Changes.fire({});
				});
			});
			i->second.routing->store(false);
			i->second.status = DesktopArchiveStatus::RemovalPending;
			i->second.failure = u"explicit-discard"_q;
			i->second.discardFailure.clear();
			static_cast<void>(worker->Stop(false));
			reset.detach();
			Changes.fire({});
		} catch (const std::system_error &) {
			active->store(false);
			i->second.resetting.reset();
			i->second.discardFailure = u"discard-worker-unavailable"_q;
			ShowArchiveDiscardFailure(live, i->second.discardFailure);
			Changes.fire({});
		}
	};
}

void QueueArchiveRetentionGap(not_null<Main::Session*> session, QString reason) {
	if (Retired.contains(session.get())) {
		return;
	}
	const auto worker = For(session).worker;
	const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto time = base::unixtime::now();
	const auto posted = worker->Post([reason, id, time](DesktopArchiveStore &store) {
		if (!store.Gap(id, reason, time)) {
			store.NoteCaptureLoss(reason);
		}
	}, true);
	if (!posted) {
		ReportArchiveCaptureLoss(session.get(), {{reason, 1}});
	}
}

namespace {

void QueueArchiveJournalWrite(not_null<Main::Session*> session, ArchiveEvent event, int byteLimit) {
	auto &value = For(session);
	const auto journal = value.journal;
	const auto worker = value.worker;
	if (!journal || worker->NeedsStartupCapture()) {
		if (worker->StartupHeld()) {
			return;
		}
		if (!worker->PostStartupCapture(std::move(event), byteLimit)) {
			QueueArchiveRetentionGap(session, u"capture-write-unavailable"_q);
		}
		return;
	}
	static auto writers = std::make_shared<std::atomic<int>>(0);
	const auto slots = writers;
	if (slots->fetch_add(1) >= 8) {
		slots->fetch_sub(1);
		QueueArchiveRetentionGap(session, u"capture-write-unavailable"_q);
		return;
	}
	journal->BindCaptureGeneration(event);
	const auto ack = std::make_shared<std::promise<void>>();
	auto future = ack->get_future();
	const auto weak = base::make_weak(session.get());
	try {
		std::thread([weak, journal, worker, slots, ack, byteLimit, event = std::move(event)]() mutable {
			const auto release = gsl::finally([&] { slots->fetch_sub(1); });
			const auto captured = journal->Append(event, byteLimit);
			ack->set_value();
			if (captured) {
				static_cast<void>(worker->Post([journal](DesktopArchiveStore &store) {
					if (!journal->WriterRetired()) {
						static_cast<void>(journal->Drain(store));
					}
				}, true));
				return;
			}
			if (journal->WriterRetired() || !journal->CaptureAllowed()) {
				return;
			}
			auto reason = journal->failure();
			if (reason.isEmpty()) {
				reason = u"capture-journal-append"_q;
			}
			const auto id = QString::fromLatin1(QCryptographicHash::hash(
				event.id.toUtf8() + "/append", QCryptographicHash::Sha256).toHex());
			const auto time = event.observation.observedAt;
			const auto marked = journal->Attention(reason, time, id);
			const auto posted = worker->Post([journal, reason, id, time, marked](DesktopArchiveStore &store) {
				if (!journal->WriterRetired() && !store.Gap(id, reason, time) && !marked) {
					store.NoteCaptureLoss(reason);
				}
			}, true);
			if (!posted && !marked) {
				crl::on_main([weak, reason] {
					if (weak) {
						ReportArchiveCaptureLoss(weak.get(), {{reason, 1}});
					}
				});
			}
		}).detach();
	} catch (const std::system_error &) {
		slots->fetch_sub(1);
		QueueArchiveRetentionGap(session, u"capture-writer-unavailable"_q);
		return;
	}
	static_cast<void>(future.wait_for(std::chrono::milliseconds(10)));
}

}

void QueueArchiveLifecycleFact(not_null<Main::Session*> session, ArchiveEvent event) {
	if (Retired.contains(session.get()) || event.key.account != SessionAccountKey(session)) {
		return;
	}
	const auto bytes = event.content ? event.content->text.size() * 4
		+ event.content->caption.size() * 4 + QJsonDocument(event.content->rich.toObject()).toJson().size()
		+ QJsonDocument(event.content->entities).toJson().size() : 0;
	if (bytes > 48 * 1024 || !event.content || event.content->media.size() > 16) {
		QueueArchiveRetentionGap(session, u"lifecycle-small-write-limit"_q);
		return;
	}
	event.observation.payload.clear();
	event.observation.payloadFormat.clear();
	QueueArchiveJournalWrite(session, std::move(event), 60 * 1024);
}

Fn<void(uint64_t, QByteArray)> ArchiveStreamSink(not_null<Main::Session*> session,
	DesktopArchiveObservation observation, uint64_t size) {
	if (Retired.contains(session.get())) {
		return [](uint64_t, QByteArray) {};
	}
	auto &value = For(session);
	const auto worker = value.worker;
	const auto journal = value.journal;
	const auto binding = std::make_shared<std::optional<ArchiveStagingBinding>>();
	const auto owned = std::make_shared<bool>(false);
	const auto streamActive = std::make_shared<std::atomic<bool>>(true);
	const auto retirement = std::shared_ptr<void>(nullptr, [worker, streamActive](void*) {
		streamActive->store(false);
		worker->PruneStreamsSoon();
	});
	const auto weak = base::make_weak(session.get());
	return [weak, worker, journal, binding, owned, streamActive, retirement, observation = std::move(observation), size](uint64_t offset, QByteArray bytes) {
		if (!weak || Retired.contains(weak.get()) || !journal || !journal->CaptureAllowed() || !journal->access().active
			|| !journal->access().active()
			|| bytes.size() > kArchiveMediaChunkBytes || offset > size || uint64_t(bytes.size()) > size - offset) {
			return;
		}
		const auto count = uint64_t(bytes.size());
		const auto admission = worker->PostStream([journal, binding, owned, streamActive, observation, size, offset,
			bytes = std::move(bytes)](DesktopArchiveStore &store) {
			const auto current = [&] {
				return streamActive->load()
					&& journal->CaptureAllowed()
					&& journal->access().current()
					&& CanRunArchiveWork(journal->access().epoch, store.epoch());
			};
			if (*owned || !current()) {
				return;
			}
			if (!*binding) {
				const auto resolve = [&] {
					return store.ObservedMediaVersion(observation.key,
						observation.observation, observation.contentHash, observation.identity);
				};
				auto version = resolve();
				if (!version) {
					// The first part can reach the worker before the observation's replay task.
					if (!journal->Drain(store) || !current()) {
						return;
					}
					version = resolve();
				}
				if (!version) {
					return;
				}
				*binding = store.StageStream(observation.key, *version, observation.identity, size);
				if (!*binding && store.ReadMedia(observation.key, *version, observation.identity,
					[](const QByteArray &) { return true; })) {
					*owned = true;
					return;
				}
			}
			if (!*binding || !store.StreamPart(**binding, offset, bytes, streamActive)) {
				static_cast<void>(store.Gap(observation.observation + u":stream"_q,
					*binding ? u"stream-incomplete"_q : u"stream-binding-unavailable"_q,
					base::unixtime::now()));
			} else {
				const auto slice = store.ReadSlice(observation.key, {}, (**binding).source.version);
				const auto version = slice ? FindArchiveVersion(slice->state, observation.key,
					(**binding).source.version) : nullptr;
				*owned = version && std::any_of(version->media.begin(), version->media.end(),
					[&](const auto &ref) { return ref.identity == observation.identity
						&& ref.state == ArchiveMediaState::Complete; });
				if (*owned) {
					static_cast<void>(CaptureDesktopMarkdownFile(store, observation.key,
						(**binding).source.version, observation.identity));
				}
			}
		}, count);
		if (admission == decltype(admission)::Full) {
			QueueArchiveRetentionGap(weak.get(), u"stream-queue-full"_q);
		}
	};
}

std::optional<DesktopMarkdownCopyScope> OpenDesktopMarkdownCopy(
		not_null<Main::Session*> session) {
	const auto found = Sessions.find(session.get());
	if (Retired.contains(session.get()) || found == Sessions.end() || !found->second.journal) {
		return {};
	}
	const auto journal = found->second.journal;
	const auto routing = found->second.routing;
	const auto publication = journal->access().markdownPublication;
	const auto current = [journal, routing] {
		return routing->load() && journal->CaptureAllowed()
			&& journal->access().active && journal->access().active();
	};
	return publication && current() ? std::optional(DesktopMarkdownCopyScope{
		journal->access().epoch, publication, current, publication->stamp()}) : std::nullopt;
}

void CopyArchiveVersionAsMarkdown(not_null<Main::Session*> session,
		DesktopArchiveMarkdownRequest request, std::function<bool()> current) {
	const auto token = BeginMarkdownCopy();
	const auto scope = OpenDesktopMarkdownCopy(session);
	if (!scope || !current || !current() || request.epoch != scope->epoch
		|| request.key.account != SessionAccountKey(session)) { return; }
	const auto found = Sessions.find(session.get());
	if (found == Sessions.end()) { return; }
	const auto weak = base::make_weak(session.get());
	const auto journal = found->second.journal;
	static_cast<void>(found->second.worker->Post([weak, token, scope = *scope, current,
			journal, request = std::move(request)](DesktopArchiveStore &store) {
		if (!scope.current() || !journal->Drain(store)) { return; }
		auto text = ReadDesktopArchiveMarkdown(store, request);
		if (!text) { return; }
		crl::on_main([weak, token, scope, current, text = std::move(*text)] {
			const auto permitted = [&] {
				return weak && !Retired.contains(weak.get()) && !weak->account().loggingOut()
					&& weak->account().maybeSession() == weak.get()
					&& IsCurrentMarkdownCopy(token) && scope.current() && current();
			};
			static_cast<void>(scope.publication->publish(scope.stamp, permitted, [&] {
				SetDesktopMarkdownClipboard(text);
			}));
		});
	}));
}

void QueueDesktopResolvedRichBody(not_null<Main::Session*> session, ArchiveEvent event) {
	const auto found = Sessions.find(session.get());
	if (Retired.contains(session.get()) || found == Sessions.end() || !found->second.journal
		|| event.key.account != SessionAccountKey(session) || !event.content) {
		return;
	}
	const auto journal = found->second.journal;
	const auto generation = journal->CaptureGeneration(event.key.chat);
	static_cast<void>(found->second.worker->Post([journal, generation, event = std::move(event)](
			DesktopArchiveStore &store) mutable {
		if (!journal->CaptureAllowed() || !journal->access().active() || !journal->Drain(store)
			|| !CanRunArchiveWork(journal->access().epoch, store.epoch())) {
			return;
		}
		const auto slice = store.ReadSlice(event.key);
		const auto message = slice ? FindArchiveMessage(slice->state, event.key) : nullptr;
		const auto version = message && !message->versions.empty() ? &message->versions.back() : nullptr;
		if (!version || message->deleted || version->completeness != ArchiveCompleteness::Partial
			|| ArchiveGenerationFor(slice->state, event.key.chat) != generation
			|| version->observation.serverTime != event.observation.serverTime
			|| version->observation.context.value(u"editDate"_q) != event.observation.context.value(u"editDate"_q)) {
			return;
		}
		event.origin = ArchiveOrigin::Hydration;
		event.generation = generation;
		event.targetVersion = version->id;
		event.targetRevision = version->revision;
		event.expectedContentHash = ArchiveContentHash(version->content);
		static_cast<void>(store.Capture(std::move(event)));
	}));
}

void ReadDesktopMarkdownSource(not_null<Main::Session*> session,
		DesktopMarkdownCopyScope scope, DesktopMarkdownBody body,
		Fn<void(DesktopMarkdownCopySource)> done) {
	const auto found = Sessions.find(session.get());
	if (Retired.contains(session.get()) || found == Sessions.end() || !found->second.journal
		|| !scope.current || !scope.current() || scope.epoch.account != body.key.account
		|| found->second.journal->access().epoch != scope.epoch
		|| found->second.journal->access().markdownPublication != scope.publication) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto journal = found->second.journal;
	const auto accepted = found->second.worker->Post([weak, scope, body = std::move(body), done,
		journal](DesktopArchiveStore &store) {
		auto result = DesktopMarkdownCopySource();
		if (scope.current() && CanRunArchiveWork(scope.epoch, store.epoch()) && journal->Drain(store)) {
			const auto choice = ChooseDesktopCurrentMarkdown(store, body);
			result.current = store.Current() && scope.current();
			if (choice.kind == ArchiveCopySourceKind::GenuineSource) {
				result.text = DecodeDesktopMarkdownSource(choice.bytes);
				result.current = result.current && result.text.has_value();
			}
		}
		crl::on_main([weak, scope, result = std::move(result), done]() mutable {
			if (!weak || !scope.current() || scope.publication->stamp() != scope.stamp) {
				result.current = false;
				result.text.reset();
			}
			done(std::move(result));
		});
	});
	if (!accepted) { done({}); }
}

std::optional<DesktopComposeIngress> DesktopComposeSourceIngress(
		not_null<Main::Session*> session, ArchiveChatKey chat) {
	const auto found = Sessions.find(session.get());
	if (Retired.contains(session.get()) || found == Sessions.end() || !found->second.journal) {
		return {};
	}
	const auto journal = found->second.journal;
	const auto worker = found->second.worker;
	const auto routing = found->second.routing;
	const auto current = [journal, routing] {
		return routing->load() && journal->CaptureAllowed()
			&& journal->access().active && journal->access().active();
	};
	if (!current()) {
		return {};
	}
	const auto generation = journal->CaptureGeneration(chat);
	const auto weak = base::make_weak(session.get());
	return DesktopComposeIngress{journal->access().epoch, current,
		[weak, journal, worker, current, generation, chat](ArchiveEvent observed, ArchiveSource source) {
			if (!weak || !current() || !observed.content || observed.key.chat != chat) {
				return;
			}
			const auto found = Sessions.find(weak.get());
			if (found == Sessions.end() || found->second.worker != worker
				|| found->second.journal != journal) {
				return;
			}
			static_cast<void>(worker->Post([journal, current, generation, observed = std::move(observed),
				source = std::move(source)](DesktopArchiveStore &store) mutable {
				if (!current() || !CanRunArchiveWork(journal->access().epoch, store.epoch())
					|| !journal->Drain(store)) {
					return;
				}
				const auto target = DesktopMarkdownTarget(store, observed.key);
				const auto slice = target ? store.ReadSlice(observed.key, {}, target->request.version) : std::nullopt;
				const auto message = slice ? FindArchiveMessage(slice->state, observed.key) : nullptr;
				const auto version = message && message->versions.size() == 1 ? &message->versions.front() : nullptr;
				if (!target || !version || message->deleted || version->source
					|| version->origin != ArchiveOrigin::Baseline || version->missingPreimage
					|| version->revision != version->id || version->content != *observed.content
					|| target->request.generation != generation
					|| version->observation.serverTime != observed.observation.serverTime
					|| version->observation.context.value(u"editDate"_q).toInt() != 0) {
					return;
				}
				static_cast<void>(AttachDesktopMarkdownSource(store, *target, std::move(source), current));
			}));
		}};
}

void QueueArchiveObservation(not_null<Main::Session*> session, ArchiveEvent event) {
	if (Retired.contains(session.get()) || event.key.account != SessionAccountKey(session)) {
		return;
	}
	QueueArchiveJournalWrite(session, std::move(event), 0);
}

void QueueArchiveAvailableMedia(not_null<Main::Session*> session, ArchiveMessageKey key,
	QString observationId, QString contentHash, QString identity, QByteArray bytes,
	QString path, uint64_t size, std::optional<DesktopArchiveSourceIdentity> proof) {
	if (Retired.contains(session.get()) || key.account != SessionAccountKey(session)) {
		return;
	}
	auto &value = For(session);
	const auto journal = value.journal;
	const auto accepted = value.worker->Post(
		[key = std::move(key), identity = std::move(identity),
			observationId = std::move(observationId),
			contentHash = std::move(contentHash), bytes = std::move(bytes),
			path = std::move(path), size, journal, proof](DesktopArchiveStore &store) {
			if (!journal || !journal->CaptureAllowed() || !journal->access().current()) {
				return;
			}
			const auto version
				= store.ObservedMediaVersion(key, observationId, contentHash, identity);
			if (!version) {
				return;
			}
			auto memory = QBuffer();
			memory.setData(bytes);
			auto file = QFile(path);
			auto source = static_cast<QIODevice*>(nullptr);
			if (!bytes.isEmpty() || (!size && path.isEmpty())) {
				if (memory.open(QIODevice::ReadOnly) && uint64_t(bytes.size()) == size) {
					source = &memory;
				}
			} else if (!path.isEmpty() && proof
					   && DesktopArchiveOpenSource(path, *proof, file)) {
				source = &file;
			}
			if (!source) {
				static_cast<void>(store.Gap(*version + u"-media"_q,
					u"media-source-unavailable"_q, base::unixtime::now()));
				return;
			}
			auto hash = QCryptographicHash(QCryptographicHash::Sha256);
			auto total = uint64_t(0);
			while (!source->atEnd() && store.Current()) {
				const auto chunk = source->read(kArchiveMediaChunkBytes);
				if (chunk.isEmpty() || uint64_t(chunk.size()) > size - total) {
					source = nullptr;
					break;
				}
				total += chunk.size();
				hash.addData(chunk);
			}
			if (!source || total != size
				|| (source == &file
					&& (!proof || !DesktopArchiveSourceMatches(file, *proof)))
				|| !source->seek(0) || !store.Current()
				|| !store.CopyMedia(key, *version, identity, *source,
					{QString::fromLatin1(hash.result().toHex()), size})) {
				static_cast<void>(store.Gap(*version + u"-media"_q,
					u"media-copy-incomplete"_q, base::unixtime::now()));
			} else {
				static_cast<void>(CaptureDesktopMarkdownFile(store, key, *version, identity));
			}
		});
	if (!accepted) {
		QueueArchiveRetentionGap(session, u"media-queue-full"_q);
	}
}

QString FormatArchiveSize(uint64_t bytes) {
	constexpr auto units = std::array{ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
	auto unit = size_t(0);
	auto divisor = uint64_t(1);
	while (bytes / divisor >= 1024 && unit + 1 < units.size()) {
		divisor *= 1024;
		++unit;
	}
	const auto whole = bytes / divisor;
	const auto fraction = (bytes % divisor) * 10 / divisor;
	const auto number = QLocale().toString(
		double(whole) + double(fraction) / 10,
		'f',
		fraction ? 1 : 0);
	return number + ' ' + QString::fromLatin1(units[unit]);
}

QString ArchiveStorageSizeLabel(not_null<Main::Session*> session) {
	if (Retired.contains(session.get())) {
		return {};
	}
	const auto &usage = For(session).usage;
	if (!usage) {
		return {};
	}
	auto label = FormatArchiveSize(usage->bytes);
	if (!usage->complete) {
		label += u'+';
	}
	return st::wrap_rtl(label);
}

QString ArchiveStorageSummary(not_null<Main::Session*> session) {
	if (Retired.contains(session.get())) {
		return tr::lng_purple_archive_removal_pending(tr::now);
	}
	const auto &value = For(session);
	if (!value.discardFailure.isEmpty()) {
		return tr::lng_purple_archive_discard_failed(tr::now)
			+ u" ["_q + value.discardFailure + ']';
	}
	if (value.failure.startsWith(u"discard-"_q) || value.failure == u"explicit-discard"_q) {
		const auto text = value.status == DesktopArchiveStatus::RemovalPending
			? tr::lng_purple_archive_removal_pending(tr::now)
			: tr::lng_purple_archive_discard_failed(tr::now);
		return text + u" ["_q + value.failure + ']';
	}
	if (value.status == DesktopArchiveStatus::Maintenance) {
		return tr::lng_purple_archive_purge_running(tr::now);
	}
	if (value.status == DesktopArchiveStatus::MaintenanceHeld || value.failure.startsWith(u"purge-"_q)) {
		return tr::lng_purple_archive_purge_held(tr::now);
	}
	const auto usage = [&] {
		return tr::lng_purple_archive_usage(
			tr::now, lt_size, FormatArchiveSize(value.usage ? value.usage->bytes : 0));
	};
	if (value.status == DesktopArchiveStatus::Ready) {
		auto result = usage();
		if (!value.captureLosses.empty()) {
			result = tr::lng_purple_archive_gap(tr::now) + '\n'
				+ CaptureLossDetails(value.captureLosses) + '\n' + result;
		}
		if (!value.captureDetails.empty()) {
			result += '\n' + CaptureLossDetails(value.captureDetails);
		}
		return result;
	}
	if (value.status == DesktopArchiveStatus::Disabled) {
		return tr::lng_purple_archive_opening(tr::now);
	}
	return tr::lng_purple_archive_unavailable(tr::now);
}

rpl::producer<> ArchiveStorageChanges() { return Changes.events(); }

void ReadArchiveVersions(not_null<Main::Session*> session, ArchiveMessageKey key,
		QString before, std::optional<uint64_t> revision,
		Fn<void(DesktopArchiveHistoryPage)> done, QString history, bool observations) {
	if (Retired.contains(session.get())) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session.get());
	auto &value = For(session);
	const auto journal = value.journal;
	const auto worker = value.worker;
	const auto fence = worker->DeliveryFence();
	const auto accepted = worker->Post([key = std::move(key), before = std::move(before),
		revision, weak, done, journal, fence, worker, history = std::move(history), observations](DesktopArchiveStore &store) {
		const auto ticket = fence->load();
		const auto revoked = [weak] {
			crl::on_main([weak] {
				if (weak && !Retired.contains(weak.get())) {
					Changes.fire({});
				}
			});
		};
		auto page = journal && !journal->Drain(store) ? DesktopArchiveHistoryPage()
			: store.History(key, 32, before, revision, history, revoked, observations);
		if (!observations) {
			for (const auto &version : page.versions) {
				page.markdownDigests.push_back(DesktopArchiveMarkdownDigest(version));
			}
		}
		if (page.active) {
			worker->RevokeOnStop(page.active);
		}
		crl::on_main([weak, page = std::move(page), done, fence, ticket]() mutable {
			if (!weak || Retired.contains(weak.get())) {
				if (page.active) page.active->store(false);
				return;
			}
			if (fence->load() != ticket || (page.active && !page.active->load())) {
				page.versions.clear();
				page.status = DesktopArchiveStatus::Stale;
			}
			done(std::move(page));
		});
	});
	if (!accepted) {
		done({});
	}
}

void CloseArchiveHistory(not_null<Main::Session*> session, QString id, ArchiveStoreEpoch epoch) {
	const auto i = Sessions.find(session.get());
	if (i != Sessions.end() && epoch.account == SessionAccountKey(session)) {
		static_cast<void>(i->second.worker->Post([id = std::move(id), epoch](DesktopArchiveStore &store) {
			store.CloseHistory(id, epoch);
		}, true));
	}
}

void OpenArchiveMedia(not_null<Main::Session*> session, ArchiveMediaRequest request,
	std::function<bool()> current,
	Fn<void(std::shared_ptr<ArchiveMediaAccess>)> done) {
	if (Retired.contains(session.get()) || request.source.key.account != SessionAccountKey(session)
		|| !current || !current()) {
		done(nullptr);
		return;
	}
	const auto weak = base::make_weak(session.get());
	auto &value = For(session);
	const auto worker = value.worker;
	const auto journal = value.journal;
	const auto route = [weak, current] {
		return weak && !Retired.contains(weak.get()) && current();
	};
	const auto accepted = worker->Post([worker, journal, request = std::move(request), weak, route, done](DesktopArchiveStore &store) {
		auto committed = ArchiveLeaseRevisionChange();
		const auto leaseCommitted = request.leaseCommitted;
		auto handle = journal && journal->access().current() && journal->Drain(store)
			? store.OpenMedia(request, &committed) : std::nullopt;
		const auto leaseDelivery = std::make_shared<std::atomic<bool>>(true);
		if (handle) {
			worker->RevokeOnStop(handle->active);
			worker->RevokeOnStop(leaseDelivery);
		}
		crl::on_main([worker, journal, handle = std::move(handle), weak, route, done,
			committed, leaseCommitted, leaseDelivery]() mutable {
			if (!handle) {
				done(nullptr);
				return;
			}
			const auto bound = *handle;
			const auto release = [worker, bound, weak, leaseCommitted, leaseDelivery] {
				bound.active->store(false);
				static_cast<void>(worker->Post([bound, weak, leaseCommitted, leaseDelivery](DesktopArchiveStore &store) {
					auto committed = ArchiveLeaseRevisionChange();
					const auto sameEpoch = CanRunArchiveWork(bound.source.epoch, store.epoch());
					static_cast<void>(store.ReleaseMedia(bound, &committed));
					if (sameEpoch && committed.leaseOnly && leaseCommitted) {
						crl::on_main([weak, leaseCommitted, committed, leaseDelivery] {
							if (weak && !Retired.contains(weak.get()) && leaseDelivery->load()) {
								leaseCommitted(committed);
							}
						});
					}
				}, true));
			};
			if (!route() || !bound.active->load()) {
				release();
				done(nullptr);
				return;
			}
			if (committed.leaseOnly && leaseCommitted) {
				leaseCommitted(committed);
			}
			auto read = [worker, journal, bound, route](uint64_t offset, uint32_t length,
				ArchiveMediaAccess::ReadDone complete) {
				if (!route() || !bound.active->load()) {
					bound.active->store(false);
					complete({});
					return;
				}
				const auto accepted = worker->Post([journal, bound, offset, length, route, complete](DesktopArchiveStore &store) {
					auto bytes = journal && journal->access().current()
						? store.MediaRange(bound, offset, length) : std::nullopt;
					crl::on_main([bound, route, complete, bytes = std::move(bytes)]() mutable {
						if (!bound.active->load() || !route()) {
							bound.active->store(false);
							bytes.reset();
						}
						complete(std::move(bytes));
					});
				});
				if (!accepted) {
					complete({});
				}
			};
			auto save = [worker, journal, bound, route](QString path, ArchiveMediaAccess::SaveDone complete) {
				if (!route() || !bound.active->load()) {
					bound.active->store(false);
					complete(false);
					return;
				}
				const auto accepted = worker->Post([journal, bound, path = std::move(path), route, complete](DesktopArchiveStore &store) {
					const auto ok = journal && journal->access().current() && store.ExportMedia(bound, path);
					crl::on_main([bound, route, complete, ok] { complete(ok && bound.active->load() && route()); });
				});
				if (!accepted) {
					complete(false);
				}
			};
			done(std::make_shared<ArchiveMediaAccess>(bound, std::move(read), std::move(save), release, route));
		});
	});
	if (!accepted) {
		done(nullptr);
	}
}

}

namespace Purple {
namespace {

void DeliverDeletedPage(not_null<Main::Session*> session,
		std::function<ArchiveDeletedPage(DesktopArchiveStore &)> read,
		Fn<void(ArchiveDeletedPage)> done, bool opening = false) {
	if (Retired.contains(session.get())) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto worker = For(session).worker;
	const auto accepted = worker->Post([weak, worker, read = std::move(read), done, opening](DesktopArchiveStore &store) {
		auto page = read(store);
		if (opening && page.active) {
			worker->RevokeOnStop(page.active);
		}
		crl::on_main([weak, worker, page = std::move(page), done]() mutable {
			if (!weak || Retired.contains(weak.get())) {
				if (page.active) {
					page.active->store(false);
				}
				return;
			}
			if (page.active && !page.active->load()) {
				page.entries.clear();
				page.status = ArchiveQueryStatus::Stale;
			}
			done(std::move(page));
		});
	});
	if (!accepted) {
		auto page = ArchiveDeletedPage();
		page.status = ArchiveQueryStatus::Busy;
		done(std::move(page));
	}
}

}

void OpenArchiveDeletedView(not_null<Main::Session*> session, ArchiveViewScope scope,
		ArchiveTranscriptOrder order, Fn<void(ArchiveDeletedPage)> done) {
	if (Retired.contains(session.get()) || scope.messages.account != SessionAccountKey(session)) {
		done({});
		return;
	}
	const auto journal = For(session).journal;
	DeliverDeletedPage(session, [scope = std::move(scope), order, journal](DesktopArchiveStore &store) {
		return journal && !journal->Drain(store) ? ArchiveDeletedPage() : store.OpenDeletedView(scope, order);
	}, std::move(done), true);
}

void ReadArchiveDeletedView(not_null<Main::Session*> session, QString id,
		ArchiveStoreEpoch epoch, Fn<void(ArchiveDeletedPage)> done) {
	if (epoch.account != SessionAccountKey(session)) {
		done({});
		return;
	}
	DeliverDeletedPage(session, [id = std::move(id), epoch](DesktopArchiveStore &store) {
		return store.ReadDeletedView(id, epoch);
	}, std::move(done));
}

void CloseArchiveDeletedView(not_null<Main::Session*> session, QString id, ArchiveStoreEpoch epoch) {
	const auto i = Sessions.find(session.get());
	if (i == Sessions.end() || epoch.account != SessionAccountKey(session)) {
		return;
	}
	static_cast<void>(i->second.worker->Post([id = std::move(id), epoch](DesktopArchiveStore &store) {
		static_cast<void>(store.CloseDeletedView(id, epoch));
	}, true));
}

}

namespace Purple {

void PrepareArchivePurge(not_null<Main::Session*> session, ArchivePurgeKind kind,
		ArchivePurgeScope scope, Fn<void(DesktopArchivePurgePreview)> done) {
	if (Retired.contains(session.get()) || scope.account != SessionAccountKey(session)) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto &value = For(session);
	const auto worker = value.worker;
	const auto journal = value.journal;
	if (!worker->Post([weak, worker, journal, kind, scope = std::move(scope), done](DesktopArchiveStore &store) {
		const auto preview = store.MaintenancePending() || store.MaintenanceHeld()
			? store.PreparePurge(kind, scope)
			: journal && journal->CaptureAllowed() && journal->Drain(store)
			? store.PreparePurge(kind, scope) : DesktopArchivePurgePreview();
		crl::on_main([weak, preview, done] {
			if (weak && !Retired.contains(weak.get())) {
				done(preview);
			}
		});
	})) {
		auto preview = DesktopArchivePurgePreview();
		preview.status = ArchiveQueryStatus::Busy;
		done(preview);
	}
}

void ExecuteArchivePurge(not_null<Main::Session*> session, ArchivePurgeConfirmation confirmation,
		std::shared_ptr<std::atomic<bool>> permitted, Fn<void(DesktopArchiveStatus)> done) {
	if (Retired.contains(session.get()) || !permitted || !permitted->load()
		|| confirmation.epoch.account != SessionAccountKey(session)) {
		done(DesktopArchiveStatus::Stale);
		return;
	}
	auto &value = For(session);
	const auto worker = value.worker;
	const auto journal = value.journal;
	if (!journal || !worker->ClaimPurge()) {
		done(DesktopArchiveStatus::Busy);
		return;
	}
	journal->PauseCapture();
	const auto weak = base::make_weak(session.get());
	const auto accepted = worker->Post([weak, worker, journal, confirmation, permitted, done](DesktopArchiveStore &store) {
		const auto started = permitted->load() && journal->Drain(store) && permitted->load()
			&& store.BeginPurge(confirmation);
		if (!store.MaintenancePending()) {
			journal->ResumeCapture();
			worker->ReleasePurgeClaim();
		}
		const auto status = started ? DesktopArchiveStatus::Maintenance
			: store.MaintenancePending() ? DesktopArchiveStatus::MaintenanceHeld
			: DesktopArchiveStatus::Stale;
		crl::on_main([weak, permitted, status, done] {
			if (weak && !Retired.contains(weak.get()) && permitted->load()) {
				done(status);
			}
		});
	});
	if (!accepted) {
		journal->ResumeCapture();
		worker->ReleasePurgeClaim();
		done(DesktopArchiveStatus::Busy);
	}
}

void RetryArchiveMaintenance(not_null<Main::Session*> session, ArchiveStoreEpoch epoch,
		std::shared_ptr<std::atomic<bool>> permitted, Fn<void(DesktopArchiveStatus)> done) {
	if (Retired.contains(session.get()) || !permitted || !permitted->load()
		|| epoch.account != SessionAccountKey(session)) {
		done(DesktopArchiveStatus::Stale);
		return;
	}
	const auto worker = For(session).worker;
	const auto weak = base::make_weak(session.get());
	if (!worker->Post([worker, weak, epoch, permitted, done](DesktopArchiveStore &store) {
		const auto retried = permitted->load() && CanRunArchiveWork(epoch, store.epoch())
			&& store.RetryMaintenance();
		if (retried && !store.MaintenancePending()) {
			worker->ReleasePurgeClaim();
		}
		const auto status = retried ? store.status() : DesktopArchiveStatus::Stale;
		crl::on_main([weak, permitted, status, done] {
			if (weak && !Retired.contains(weak.get()) && permitted->load()) {
				done(status);
			}
		});
	})) {
		done(DesktopArchiveStatus::Busy);
	}
}

}
