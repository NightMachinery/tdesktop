#include "purple/purple_archive_service.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "config.h"
#include "core/application.h"
#include "core/version.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_config.h"
#include "settings.h"
#include "storage/storage_account.h"
#include "storage/details/storage_file_utilities.h"
#include <QtCore/QPointer>
#include "ui/toast/toast.h"
#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <algorithm>
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
	ArchiveWorker(DesktopArchiveOptions options,
		Fn<void(DesktopArchiveStatus, QString, uint64_t)> report,
		Fn<void(std::shared_ptr<DesktopArchiveJournal>)> ready)
		: _state(std::make_shared<State>()) {
		_state->report = std::move(report);
		_state->ready = std::move(ready);
		options.revocableSecrets = _state->secrets;
		options.active = [state = _state] { return !state->cancelled.load(); };
		std::thread([state = _state, options = std::move(options)]() mutable {
			Run(std::move(state), std::move(options));
		}).detach();
	}
	static std::unique_ptr<ArchiveWorker> RemovalOnly(DesktopArchiveOptions options,
		bool resolveSlot, Fn<void(DesktopArchiveStatus, QString, uint64_t)> report) {
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
		if (!lock.owns_lock() || _state->cancelled || _state->tasks.size() >= (cleanup ? 80 : 64)) {
			return false;
		}
		_state->tasks.push_back(std::move(task));
		_state->condition.notify_one();
		return true;
	}
	void PruneStreamsSoon() {
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
			_state->tasks.clear();
		}
		_state->condition.notify_all();
		return true;
	}

  private:
	ArchiveWorker() : _state(std::make_shared<State>()) {}
	struct State {
		Fn<void(DesktopArchiveStatus, QString, uint64_t)> report;
		Fn<void(std::shared_ptr<DesktopArchiveJournal>)> ready;
		std::mutex mutex;
		std::condition_variable condition;
		std::deque<Task> tasks;
		std::vector<std::weak_ptr<std::atomic<bool>>> mediaReaders;
		std::atomic<bool> cancelled = false;
		std::atomic<bool> remove = false;
		std::atomic<bool> removed = false;
		std::atomic<bool> finished = false;
		std::atomic<bool> streamCleanup = false;
		bool resolveSlot = false;
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
		auto startupHeld = false;
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
		if (!state->cancelled) {
			if (!authorityReady) {
				startupHeld = true;
				state->report(DesktopArchiveStatus::Unreadable, u"login-authority-held"_q, 0);
			} else if (!DesktopArchiveStore::Recover(options)) {
				startupHeld = true;
				state->report(DesktopArchiveStatus::RemovalPending, u"logout-recovery"_q, 0);
			} else {
				static_cast<void>(store.Open(std::move(options)));
				if (!store.epoch().storeId.isEmpty()) {
					removalOptions.removalStore = store.epoch().storeId;
				}
				if (const auto access = store.JournalAccess()) {
					journal = std::make_shared<DesktopArchiveJournal>(*access);
					if (journal->Open() && journal->Drain(store)) {
						if (!state->cancelled) {
							state->ready(journal);
						}
					} else {
						journalFailure = journal->failure().isEmpty()
							? u"capture-recovery-held"_q : journal->failure();
						static_cast<void>(journal->Attention(journalFailure, base::unixtime::now()));
					}
				}
			}
		}
		DesktopArchiveCrypto::Clear(options.localKey);
		DesktopArchiveCrypto::Clear(options.loginAuthority);
		auto usageBytes = uint64_t(0);
		auto usageComplete = false;
		auto nextUsage = std::chrono::steady_clock::time_point();
		const auto report = [&] {
			if (state->cancelled || startupHeld) {
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (store.Current() && (!usageComplete || now >= nextUsage)) {
				const auto usage = store.Usage();
				usageBytes = usage ? TotalArchiveUsage(*usage).value_or(0) : usageBytes;
				usageComplete = usage && store.UsageComplete();
				nextUsage = now + std::chrono::seconds(60);
			}
			if (journal && !journal->failure().isEmpty()) {
				journalFailure = journal->failure();
			}
			const auto gap = !journalFailure.isEmpty()
				&& store.status() == DesktopArchiveStatus::Ready;
			state->report(gap ? DesktopArchiveStatus::RetentionGap : store.status(),
				journalFailure.isEmpty() ? store.failure() : journalFailure, usageBytes);
		};
		report();
		auto streamCursor = QString();
		auto streamMore = store.Current();
		auto nextViewsExpiry = std::chrono::steady_clock::time_point();
		for (;;) {
			auto task = Task();
			auto idle = false;
			{
				auto lock = std::unique_lock(state->mutex);
				state->condition.wait_for(lock, std::chrono::seconds(5), [&] {
					return state->cancelled.load() || !state->tasks.empty() || streamMore
						|| state->streamCleanup.load();
				});
				if (state->cancelled) {
					break;
				}
				if (state->streamCleanup.exchange(false)) {
					task = [](DesktopArchiveStore &store) { store.PruneStreams(); };
				} else if (state->tasks.empty() && !streamMore) {
					idle = true;
				} else if (state->tasks.empty()) {
					task = [&](DesktopArchiveStore &store) {
						if (!store.ResumeStreams(streamCursor, streamMore)) {
							streamMore = false;
							static_cast<void>(store.Gap(u"stream-recovery"_q, u"stream-recovery-held"_q, base::unixtime::now()));
						}
					};
				} else {
					task = std::move(state->tasks.front());
					state->tasks.pop_front();
				}
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextViewsExpiry) {
				static_cast<void>(store.ExpireDeletedViews());
				nextViewsExpiry = now + std::chrono::seconds(5);
			}
			if (idle) {
				continue;
			}
			state->delivery->fetch_add(1);
			task(store);
			store.PruneStreams();
			if (journal && journal->access().current()) {
				static_cast<void>(journal->Drain(store));
			}
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
				state->report(DesktopArchiveStatus::RemovalPending, u"logout-removal-pending"_q, 0);
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
	uint64_t bytes = 0;
};
std::map<Main::Session*, ArchiveSession> Sessions;
std::set<Main::Session*> Retired;

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
	const auto routing = value.routing;
	value.worker = std::make_shared<ArchiveWorker>(
		std::move(options),
		[weak, routing, app = QPointer<Core::Application>(&Core::App())](DesktopArchiveStatus status, QString failure, uint64_t bytes) {
			crl::on_main([weak, routing, app, status, failure = std::move(failure), bytes] {
				if (!app) {
					return;
				}
				if (!routing->load()) {
					if (status == DesktopArchiveStatus::RemovalPending
						&& (!weak || Retired.contains(weak.get()))) {
						Ui::Toast::Show(tr::lng_purple_archive_removal_pending(tr::now));
					}
					return;
				}
				const auto live = weak.get();
				if (!live) {
					if (status == DesktopArchiveStatus::RemovalPending) {
						Ui::Toast::Show(tr::lng_purple_archive_removal_pending(tr::now));
					}
					return;
				}
				const auto i = Sessions.find(live);
				if (i == Sessions.end()) {
					if (status == DesktopArchiveStatus::RemovalPending) {
						Ui::Toast::Show(tr::lng_purple_archive_removal_pending(tr::now));
					}
					return;
				}
				const auto firstFailure
					= i->second.failure.isEmpty()
					  && (!failure.isEmpty()
						  || (status != DesktopArchiveStatus::Ready
							  && status != DesktopArchiveStatus::Disabled));
				i->second.status = status;
				if (!failure.isEmpty()) {
					i->second.failure = failure;
				}
				i->second.bytes = bytes;
				if (firstFailure && Core::App().domain().started()
					&& Core::App().domain().active().maybeSession() == live) {
					const auto gap = status == DesktopArchiveStatus::Ready
						|| status == DesktopArchiveStatus::RetentionGap;
					Ui::Toast::Show(gap ? tr::lng_purple_archive_gap(tr::now)
						: tr::lng_purple_archive_unavailable(tr::now));
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
				i->second.journal = journal;
				Changes.fire({});
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
	refresh();
}

void RemoveArchiveAtLogout(Main::Account &account) {
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
			[app = QPointer<Core::Application>(&Core::App())](DesktopArchiveStatus status, QString, uint64_t) {
				if (status == DesktopArchiveStatus::RemovalPending) {
					crl::on_main([app] {
						if (app) {
							Ui::Toast::Show(tr::lng_purple_archive_removal_pending(tr::now));
						}
					});
				}
			}));
	} catch (const std::system_error &) {
		Ui::Toast::Show(tr::lng_purple_archive_removal_pending(tr::now));
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
		i->second.routing->store(false);
		live->lifetime().add([active] { active->store(false); });
		live->mtp().writeKeysRequests() | rpl::on_next([weak, active, keys = options.loginAuthKeyIds] {
			const auto live = weak.get();
			if (!live || Options(live->account()).loginAuthKeyIds != keys) {
				active->store(false);
			}
		}, live->lifetime());
		i->second.status = DesktopArchiveStatus::RemovalPending;
		i->second.failure = u"explicit-discard"_q;
		static_cast<void>(worker->Stop(false));
		Changes.fire({});
		options.active = [active] { return active->load(); };
		try {
			std::thread([weak, worker, options = std::move(options), active]() mutable {
				auto removed = false;
				if (worker->WaitForStopped(std::chrono::milliseconds(500)) && active->load()) {
					const auto token = DesktopArchiveStore::PrepareDiscard(options);
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
					options.maintenanceAllowed = [deadline] {
						return std::chrono::steady_clock::now() < deadline;
					};
					removed = token && DesktopArchiveStore::DiscardAccount(options, *token, 64);
				}
				crl::on_main([weak, worker, options = std::move(options), active, removed] {
					const auto live = weak.get();
					if (!live || Retired.contains(live)) {
						return;
					}
					const auto i = Sessions.find(live);
					if (i == Sessions.end() || i->second.worker != worker
						|| i->second.resetting != active) {
						return;
					}
					const auto current = active->load()
						&& live->local().readPref<QByteArray>(kLoginAuthorityPref) == options.loginAuthority
						&& Options(live->account()).loginAuthKeyIds == options.loginAuthKeyIds;
					active->store(false);
					if (removed && current) {
						live->local().clearPref(kLoginAuthorityPref);
						live->local().clearPref(kLoginStorePref);
						Sessions.erase(i);
						static_cast<void>(For(live));
					} else {
						i->second.status = DesktopArchiveStatus::Unreadable;
						i->second.failure = u"discard-held"_q;
						Ui::Toast::Show(tr::lng_purple_archive_discard_failed(tr::now));
					}
					Changes.fire({});
				});
			}).detach();
		} catch (const std::system_error &) {
			active->store(false);
			i->second.status = DesktopArchiveStatus::Unreadable;
			i->second.failure = u"discard-worker-unavailable"_q;
			Ui::Toast::Show(tr::lng_purple_archive_discard_failed(tr::now));
			Changes.fire({});
		}
	};
}

void QueueArchiveRetentionGap(not_null<Main::Session*> session, QString reason) {
	if (Retired.contains(session.get())) {
		return;
	}
	auto &value = For(session);
	const auto firstFailure = value.failure.isEmpty();
	value.failure = reason;
	static_cast<void>(value.worker->Post([reason](DesktopArchiveStore &store) {
		static_cast<void>(store.Gap(reason, reason, base::unixtime::now()));
	}, true));
	Changes.fire({});
	if (firstFailure && Core::App().domain().started()
		&& Core::App().domain().active().maybeSession() == session.get()) {
		Ui::Toast::Show(tr::lng_purple_archive_gap(tr::now));
	}
}

namespace {

void QueueArchiveJournalWrite(not_null<Main::Session*> session, ArchiveEvent event, int byteLimit) {
	auto &value = For(session);
	const auto journal = value.journal;
	const auto worker = value.worker;
	static auto writers = std::make_shared<std::atomic<int>>(0);
	const auto slots = writers;
	if (!journal || slots->fetch_add(1) >= 8) {
		if (journal) {
			slots->fetch_sub(1);
		}
		QueueArchiveRetentionGap(session, u"capture-write-unavailable"_q);
		return;
	}
	event.generation = journal->access().generation;
	const auto ack = std::make_shared<std::promise<bool>>();
	auto future = ack->get_future();
	const auto timedOut = std::make_shared<std::atomic<bool>>(false);
	try {
		std::thread([journal, worker, slots, ack, timedOut, byteLimit, event = std::move(event)]() mutable {
			const auto release = gsl::finally([&] { slots->fetch_sub(1); });
			const auto captured = journal->Append(event, byteLimit);
			ack->set_value(captured);
			const auto reason = captured ? QString() : journal->failure();
			if (!captured) {
				static_cast<void>(journal->Attention(reason, event.observation.observedAt));
			}
			static_cast<void>(worker->Post([journal, captured, reason, timedOut, id = event.id,
				time = event.observation.observedAt,
				limited = event.observation.context.value(u"desktopSnapshotLimited"_q).toBool()](DesktopArchiveStore &store) {
				if (captured) {
					static_cast<void>(journal->Drain(store));
					if (limited) {
						static_cast<void>(store.Gap(id, u"snapshot-limited"_q, time));
					}
				}
				if (!captured || timedOut->load()) {
					static_cast<void>(store.Gap(id + u":deadline"_q,
						captured ? u"capture-durability-deadline"_q : reason, time));
				}
			}, true));
		}).detach();
	} catch (const std::system_error &) {
		slots->fetch_sub(1);
		QueueArchiveRetentionGap(session, u"capture-writer-unavailable"_q);
		return;
	}
	if (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready || !future.get()) {
		timedOut->store(true);
		QueueArchiveRetentionGap(session, u"capture-durability-deadline"_q);
	}
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
	const auto pending = std::make_shared<std::atomic<uint64_t>>(0);
	const auto binding = std::make_shared<std::optional<ArchiveStagingBinding>>();
	const auto owned = std::make_shared<bool>(false);
	const auto streamActive = std::make_shared<std::atomic<bool>>(true);
	const auto retirement = std::shared_ptr<void>(nullptr, [worker, streamActive](void*) {
		streamActive->store(false);
		worker->PruneStreamsSoon();
	});
	const auto weak = base::make_weak(session.get());
	return [weak, worker, journal, pending, binding, owned, streamActive, retirement, observation = std::move(observation), size](uint64_t offset, QByteArray bytes) {
		if (!weak || Retired.contains(weak.get()) || !journal || !journal->access().active
			|| !journal->access().active()
			|| bytes.size() > kArchiveMediaChunkBytes || offset > size || uint64_t(bytes.size()) > size - offset) {
			return;
		}
		const auto count = uint64_t(bytes.size());
		if (pending->fetch_add(count) + count > 8 * 1024 * 1024) {
			pending->fetch_sub(count);
			QueueArchiveRetentionGap(weak.get(), u"stream-queue-full"_q);
			return;
		}
		const auto accepted = worker->Post([journal, pending, binding, owned, streamActive, observation, size, offset,
			bytes = std::move(bytes), count](DesktopArchiveStore &store) {
			const auto release = gsl::finally([&] { pending->fetch_sub(count); });
			if (*owned || !streamActive->load() || !journal->access().current() || !CanRunArchiveWork(journal->access().epoch, store.epoch())) {
				return;
			}
			if (!*binding) {
				const auto version = store.ObservedMediaVersion(observation.key, observation.observation,
					observation.contentHash, observation.identity);
				if (version) {
					*binding = store.StageStream(observation.key, *version, observation.identity, size);
					if (!*binding && store.ReadMedia(observation.key, *version, observation.identity,
						[](const QByteArray &) { return true; })) {
						*owned = true;
						return;
					}
				}
			}
			if (!*binding || !store.StreamPart(**binding, offset, bytes, streamActive)) {
				static_cast<void>(store.Gap(observation.observation + u":stream"_q,
					u"stream-incomplete"_q, base::unixtime::now()));
			} else {
				const auto slice = store.ReadSlice(observation.key, {}, (**binding).source.version);
				const auto version = slice ? FindArchiveVersion(slice->state, observation.key,
					(**binding).source.version) : nullptr;
				*owned = version && std::any_of(version->media.begin(), version->media.end(),
					[&](const auto &ref) { return ref.identity == observation.identity
						&& ref.state == ArchiveMediaState::Complete; });
			}
		});
		if (!accepted) {
			pending->fetch_sub(count);
			QueueArchiveRetentionGap(weak.get(), u"stream-queue-full"_q);
		}
	};
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
			if (!journal || !journal->access().current()) {
				return;
			}
			const auto version
				= store.ObservedMediaVersion(key, observationId, contentHash, identity);
			if (!version) {
				static_cast<void>(store.Gap(observationId + u"-media"_q,
					u"media-observation-stale"_q, base::unixtime::now()));
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
			}
		});
	if (!accepted) {
		value.failure = u"media-queue-full"_q;
		Changes.fire({});
	}
}

QString ArchiveStorageSummary(not_null<Main::Session*> session) {
	if (Retired.contains(session.get())) {
		return tr::lng_purple_archive_removal_pending(tr::now);
	}
	const auto &value = For(session);
	if (value.status == DesktopArchiveStatus::RetentionGap) {
		return tr::lng_purple_archive_gap(tr::now);
	}
	if (value.status == DesktopArchiveStatus::Ready) {
		return value.failure.isEmpty() ? tr::lng_purple_archive_usage(tr::now, lt_size,
											 QString::number(value.bytes))
									   : tr::lng_purple_archive_gap(tr::now);
	}
	if (value.status == DesktopArchiveStatus::Disabled) {
		return tr::lng_purple_archive_opening(tr::now);
	}
	return tr::lng_purple_archive_unavailable(tr::now);
}

rpl::producer<> ArchiveStorageChanges() { return Changes.events(); }

void ReadArchiveVersions(not_null<Main::Session*> session, ArchiveMessageKey key,
		QString before, std::optional<uint64_t> revision,
		Fn<void(DesktopArchiveHistoryPage)> done, QString history) {
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
		revision, weak, done, journal, fence, worker, history = std::move(history)](DesktopArchiveStore &store) {
		const auto ticket = fence->load();
		const auto revoked = [weak] {
			crl::on_main([weak] {
				if (weak && !Retired.contains(weak.get())) {
					Changes.fire({});
				}
			});
		};
		auto page = journal && !journal->Drain(store) ? DesktopArchiveHistoryPage()
			: store.History(key, 32, before, revision, history, revoked);
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
					if (!store.ReleaseMedia(bound, &committed) && store.Current()) {
						static_cast<void>(store.Gap(bound.lease + u":release"_q,
							u"media-reader-release-held"_q, base::unixtime::now()));
					}
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
