#include "purple/purple_emoji_export.h"

#include "purple/purple_emoji_export_store.h"
#include "purple/purple_emoji_export_queue.h"
#include "purple/purple_emoji_export_worker.h"
#include "purple/purple_emoji_large.h"
#include "purple/purple_emoji_large_render.h"
#include "purple/purple_emoji_large_setting.h"
#include "purple/purple_emoji_large_store.h"
#include "purple/purple_sync_request_sender.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "platform/platform_specific.h"
#include "settings.h"
#include "settings/settings_builder.h"
#include "ui/style/style_core.h"
#include "ui/widgets/buttons.h"
#include "styles/style_settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <array>
#include <algorithm>

namespace Purple {
namespace {

constexpr auto kDebounceMs = 5000;
constexpr auto kTaskTimeoutMs = 30000;
constexpr auto kMaxEntries = 100000;
constexpr auto kMaxResolvesPerSlice = 100;
constexpr auto kMaxMediaBytes = 4 * 1024 * 1024;
constexpr auto kExtraIds = std::array<DocumentId, 1>{ 5339220291157500400ULL };
constexpr auto kPreference = "purple-emoji-export";
rpl::event_stream<bool> PreferenceChanges;

QString CanonicalDirectory(const QString &path) {
	const auto canonical = QFileInfo(path).canonicalFilePath();
	return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

EmojiExportSource ExportSource(StickerType type) {
	switch (type) {
	case StickerType::Tgs: return EmojiExportSource::Tgs;
	case StickerType::Webm: return EmojiExportSource::Webm;
	case StickerType::Webp: return EmojiExportSource::Webp;
	}
	Unexpected("Sticker type");
}

std::optional<qint64> NextEmojiExportRetry(
		std::optional<qint64> queue, std::optional<qint64> titles) {
	return !queue ? titles : !titles ? queue : std::optional(std::min(*queue, *titles));
}

class EmojiExporter final : public QObject {
public:
	EmojiExporter();
	void logout(Main::Account &account);

private:
	void reconcile();
	void revoke(bool clear = true);
	void resetOutput(bool clear);
	void publish();
	void dirty();
	void start();
	void pump();
	void next();
	void outputFailed();
	void checkMedia();
	void resolve();
	void maintainRequests();
	void admitRequests(const std::vector<quint64> &ids);
	void chooseDemand();
	void holdDemandForMetadataWait();
	[[nodiscard]] bool resolveSetTitle(DocumentData *document);
	void applySetTitle(quint64 id);
	void deferred(mtpRequestId request, const SyncRequestTransient &transient);
	void fillMetadata(DocumentData *document);
	[[nodiscard]] bool admitted() const;
	[[nodiscard]] bool transferBusy();
	void rememberTransfer();

	const QString _directory = CurrentEmojiExportDirectory();
	EmojiExportWorker _worker;
	quint64 _workerGeneration = 0;
	QTimer _continuation;
	QTimer _debounce;
	QElapsedTimer _clock;
	EmojiExportQueue _queue{ kMaxResolvesPerSlice };
	EmojiSetTitleCache _setTitles;
	std::set<quint64> _largeIds;
	std::set<quint64> _demandIds;
	std::set<quint64> _retiredDemandIds;
	size_t _baseCount = 0;
	size_t _demandReplace = 0;
	EmojiLargeRequestQueue _largeRequests;
	std::set<quint64> _acknowledge;
	std::set<quint64> _afterPublicationAck;
	std::optional<quint64> _demandId;
	std::optional<quint64> _deferredDemand;
	bool _demandRendered = false;
	bool _scanDue = true;
	bool _pruneLarge = true;
	bool _publishPending = false;
	QTimer _poll;
	QElapsedTimer _taskTime;
	base::weak_ptr<Main::Session> _session;
	base::weak_ptr<Main::Session> _transferSession;
	base::weak_ptr<Main::Session> _retiredSession;
	DocumentId _transferId = 0;
	std::unique_ptr<SyncRequestSender> _api;
	mtpRequestId _request = 0;
	std::shared_ptr<Data::DocumentMedia> _media;
	std::vector<EmojiExportEntry> _entries;
	std::vector<std::optional<EmojiExportEntry>> _ready;
	std::shared_ptr<const std::vector<EmojiExportEntry>> _published
		= std::make_shared<const std::vector<EmojiExportEntry>>();
	EmojiExportEpoch _epoch;
	quint64 _token = 0;
	size_t _index = 0;
	bool _cleanupPending = false;
	bool _bound = false;
	bool _clean = false;
	bool _running = false;
	bool _rendering = false;
	bool _pending = false;
	bool _outputFailed = false;
	rpl::lifetime _sessionLifetime;
	rpl::lifetime _lifetime;

};

EmojiExporter::EmojiExporter()
: QObject(QCoreApplication::instance())
, _worker(_directory, this) {
	_clock.start();
	_continuation.setSingleShot(true);
	QObject::connect(&_continuation, &QTimer::timeout, this, [=] {
		_queue.continueSlice();
		pump();
	});
	_debounce.setSingleShot(true);
	QObject::connect(&_debounce, &QTimer::timeout, this, [=] { start(); });
	QObject::connect(&_poll, &QTimer::timeout, this, [=] {
		reconcile();
		if (!admitted()) {
			return;
		}
		_scanDue = true;
		if (!_rendering && _epoch.accepts(_token)) maintainRequests();
		if (_transferId && !transferBusy()) {
			dirty();
		}
		if (_media) {
			checkMedia();
		} else if (_running || _largeRequests.pending() || _deferredDemand) {
			pump();
		}
	});
	Core::App().domain().activeSessionChanges() | rpl::on_next([=] {
		_outputFailed = false;
		reconcile();
	}, _lifetime);
	Core::App().passcodeLockChanges() | rpl::on_next([=] {
		reconcile();
	}, _lifetime);
	PreferenceChanges.events() | rpl::on_next([=] {
		_outputFailed = false;
		reconcile();
	}, _lifetime);
	EmojiLargeCacheCapValue() | rpl::on_next([=](qint64) {
		_pruneLarge = true;
		_scanDue = true;
		if (admitted() && !_rendering) maintainRequests();
	}, _lifetime);
	Core::App().settings().recentEmojiUpdated() | rpl::on_next([=] {
		dirty();
	}, _lifetime);
	style::PaletteChanged() | rpl::on_next([=] {
		revoke();
		reconcile();
	}, _lifetime);
	reconcile();
	_poll.start(1000);
}

bool EmojiExporter::admitted() const {
	const auto session = _session.get();
	return !Core::Quitting() && session && EmojiExportEnabled() && !Core::App().passcodeLocked()
		&& Core::App().maybePrimarySession() == session
		&& !session->account().loggingOut() && _clean;
}

void EmojiExporter::logout(Main::Account &account) {
	const auto session = _session.get();
	if (session && &session->account() == &account) {
		_retiredSession = _session;
		revoke();
	}
}

void EmojiExporter::reconcile() {
	if (Core::Quitting()) {
		_poll.stop();
		revoke(false);
		return;
	}
	const auto primary = Core::App().maybePrimarySession();
	const auto desired = EmojiExportEnabled() && !Core::App().passcodeLocked()
		&& primary && primary != _retiredSession.get()
		&& !primary->account().loggingOut() ? primary : nullptr;
	if (_bound && (!desired || desired != _session.get())) {
		revoke();
	}
	if (!_clean && !_cleanupPending && !_outputFailed) {
		resetOutput(true);
	}
	if (desired && !_bound && _clean && !_outputFailed) {
		_session = base::make_weak(desired);
		_bound = true;
		_api = std::make_unique<SyncRequestSender>(&desired->mtp(),
			[=] { return admitted(); },
			[=](mtpRequestId request, SyncRequestTransient transient) {
				deferred(request, transient);
			});
		desired->data().stickers().updated(Data::StickersType::Emoji)
			| rpl::on_next([=] { dirty(); }, _sessionLifetime);
		desired->account().sessionChanges() | rpl::on_next([=](Main::Session *live) {
			if (live != desired) {
				revoke(!Core::Quitting());
			}
		}, _sessionLifetime);
		desired->downloaderTaskFinished() | rpl::on_next([=] {
			checkMedia();
		}, _sessionLifetime);
		dirty();
	}
}

void EmojiExporter::rememberTransfer() {
	if (!_media || !_session || transferBusy()) {
		return;
	}
	const auto document = _media->owner();
	if (document->loading()) {
		_transferSession = _session;
		_transferId = document->id;
	}
}

bool EmojiExporter::transferBusy() {
	const auto session = _transferSession.get();
	if (session && _transferId && session->data().document(_transferId)->loading()) {
		return true;
	}
	_transferSession = {};
	_transferId = 0;
	return false;
}

void EmojiExporter::revoke(bool clear) {
	_epoch.revoke();
	_debounce.stop();
	_continuation.stop();
	rememberTransfer();
	_sessionLifetime.destroy();
	_api.reset();
	_request = 0;
	_media.reset();
	_session = {};
	_entries.clear();
	_ready.clear();
	_published = std::make_shared<const std::vector<EmojiExportEntry>>();
	_queue.clear();
	_setTitles.clear();
	_largeIds.clear();
	_demandIds.clear();
	_retiredDemandIds.clear();
	_baseCount = _demandReplace = 0;
	_largeRequests.clear();
	_acknowledge.clear();
	_afterPublicationAck.clear();
	_demandId.reset();
	_deferredDemand.reset();
	_demandRendered = false;
	_scanDue = true;
	_pruneLarge = true;
	_publishPending = false;
	_rendering = false;
	_running = _pending = _bound = false;
	resetOutput(clear);
}

void EmojiExporter::resetOutput(bool clear) {
	_clean = false;
	_cleanupPending = true;
	_workerGeneration = _worker.revoke(clear, [=](quint64 generation, bool ok) {
		if (generation != _workerGeneration) return;
		_cleanupPending = false;
		_clean = ok;
		if (!ok) _outputFailed = true;
		if (!Core::Quitting()) reconcile();
	});
}

void EmojiExporter::dirty() {
	if (admitted()) {
		_pending = true;
		_debounce.start(kDebounceMs);
	}
}

void EmojiExporter::start() {
	if (!admitted() || !_pending || _running || _rendering) {
		return;
	}
	_entries.clear();
	_ready.clear();
	_largeIds.clear();
	const auto session = _session.get();
	auto rank = 0;
	AppendBoundedEmojiExportEntries(_entries, Core::App().settings().recentEmoji(),
		kMaxEntries, [&](const Core::RecentEmoji &recent) -> std::optional<EmojiExportEntry> {
			++rank;
			const auto document = std::get_if<Core::RecentEmojiDocument>(&recent.id.data);
			if (!document || !document->id || document->test != session->isTestMode()) {
				return std::nullopt;
			}
			if (_largeIds.size() < 50) _largeIds.insert(document->id);
			return EmojiExportEntry{ .id = document->id, .recentRank = rank };
		});
	const auto &stickers = session->data().stickers();
	for (const auto id : stickers.emojiSetsOrder()) {
		const auto i = stickers.sets().find(id);
		if (i == stickers.sets().end()) {
			continue;
		}
		AppendBoundedEmojiExportEntries(_entries, i->second->stickers, kMaxEntries,
			[](DocumentData *document) -> std::optional<EmojiExportEntry> {
				return document ? std::optional(EmojiExportEntry{ .id = document->id }) : std::nullopt;
			});
	}
	for (const auto id : kExtraIds) {
		_entries.push_back({ .id = id });
	}
	_entries = UniqueEmojiExportEntries(_entries);
	_baseCount = _entries.size();
	_demandReplace = 0;
	for (const auto id : _demandIds) _entries.push_back({ .id = id });
	_entries = UniqueEmojiExportEntries(_entries);
	_ready.resize(_entries.size());
	auto ids = std::vector<quint64>();
	ids.reserve(_entries.size());
	for (const auto &entry : _entries) ids.push_back(entry.id);
	_queue.begin(ids, std::vector<quint64>(kExtraIds.begin(), kExtraIds.end()));
	_index = 0;
	_token = _epoch.begin();
	_pending = false;
	_running = true;
	pump();
}

void EmojiExporter::fillMetadata(DocumentData *document) {
	auto &entry = _entries[_index];
	const auto sticker = document->sticker();
	entry.alt = sticker->alt;
	entry.setId = sticker->set.id;
	entry.textColor = document->emojiUsesTextColor();
	entry.setShortName = sticker->set.shortName;
	const auto &sets = _session.get()->data().stickers().sets();
	const auto &installed = _session.get()->data().stickers().emojiSetsOrder();
	entry.installed = std::find(installed.begin(), installed.end(), entry.setId) != installed.end();
	const auto i = sets.find(entry.setId);
	if (i != sets.end()) {
		entry.setTitle = i->second->title;
		entry.setShortName = i->second->shortName;
		if (!entry.setTitle.isEmpty()) {
			_setTitles.known(entry.setId, {entry.setTitle, entry.setShortName});
		}
	}
	if (const auto title = _setTitles.lookup(entry.setId)) {
		entry.setTitle = title->title;
		entry.setShortName = title->shortName;
	}
}

void EmojiExporter::admitRequests(const std::vector<quint64> &ids) {
	for (const auto id : ids) {
		if (_acknowledge.contains(id) || _afterPublicationAck.contains(id)) continue;
		const auto admission = _largeRequests.offer(id, _clock.elapsed());
		if (admission == EmojiLargeAdmission::Cooldown) _acknowledge.insert(id);
	}
}

void EmojiExporter::maintainRequests() {
	if (!admitted() || _rendering || !_scanDue || !_epoch.accepts(_token)) return;
	const auto token = _token;
	const auto directory = _directory;
	const auto scanGeneration = _workerGeneration;
	const auto cap = EmojiLargeCacheCap();
	const auto acknowledge = std::make_shared<std::set<quint64>>(std::move(_acknowledge));
	const auto requests = std::make_shared<std::vector<quint64>>();
	_rendering = true;
	const auto accepted = _worker.submit(_workerGeneration,
		[=](const EmojiExportAuthority &current) {
			if (!current() || !EnsureEmojiLargeDirectories(directory)) return false;
			if (!PruneEmojiLargeExport(directory, cap, current)) return false;
			for (const auto id : *acknowledge) {
				if (!current()) return false;
				static_cast<void>(AcknowledgeEmojiLargeRequest(directory, id, current));
			}
			*requests = ScanEmojiLargeRequests(directory, scanGeneration, current);
			return current();
		}, [=](quint64 generation, bool ok) {
			if (generation != _workerGeneration || !_epoch.accepts(token)) return;
			_rendering = false;
			if (!admitted()) { reconcile(); return; }
			if (!ok) { outputFailed(); return; }
			admitRequests(*requests);
			if (_media) checkMedia();
			else pump();
		});
	if (accepted) {
		_scanDue = false;
		_pruneLarge = false;
	} else {
		_rendering = false;
		_acknowledge.insert(acknowledge->begin(), acknowledge->end());
	}
}

void EmojiExporter::chooseDemand() {
	if (_demandId) return;
	auto selected = std::optional<quint64>();
	if (_deferredDemand) {
		if (_clock.elapsed() < _queue.metadataRetryAt()) return;
		selected = _deferredDemand;
		_deferredDemand.reset();
	} else {
		selected = _largeRequests.take(_clock.elapsed());
	}
	if (!selected) return;
	const auto id = *selected;
	const auto entry = std::find_if(_entries.begin(), _entries.end(),
		[=](const EmojiExportEntry &entry) { return entry.id == id; });
	auto index = size_t(entry - _entries.begin());
	if (entry == _entries.end()) {
		if (_entries.size() >= _baseCount + 1024) {
			index = _baseCount + (_demandReplace++ % 1024);
			const auto retired = _entries[index].id;
			_demandIds.erase(retired);
			_retiredDemandIds.insert(retired);
			_publishPending = true;
			_entries[index] = { .id = id };
			_ready[index].reset();
		} else {
			_entries.push_back({ .id = id });
			_ready.emplace_back();
		}
	}
	if (_demandIds.size() >= 1024) _demandIds.erase(_demandIds.begin());
	_demandIds.insert(id);
	_demandId = id;
	_demandRendered = false;
	_queue.prioritize(index);
	_running = true;
}

void EmojiExporter::holdDemandForMetadataWait() {
	if (_demandId && _clock.elapsed() < _queue.metadataRetryAt()) {
		_deferredDemand = _demandId;
		_demandId.reset();
	}
}

void EmojiExporter::pump() {
	if (!admitted()) {
		reconcile();
		return;
	}
	if (_rendering) return;
	if (_scanDue && _epoch.accepts(_token)) { maintainRequests(); return; }
	if (_media || _request || _continuation.isActive()) return;
	if (_publishPending) { publish(); return; }
	chooseDemand();
	if (!_running) return;
	const auto current = _queue.current();
	if (!current) {
		publish();
		return;
	}
	_index = *current;
	if (_queue.cooling(_entries[_index].id, _clock.elapsed())) {
		next();
		return;
	}
	const auto document = _session.get()->data().document(_entries[_index].id);
	const auto sticker = document->sticker();
	if (!sticker) {
		resolve();
		return;
	}
	if (sticker->setType != Data::StickersType::Emoji || document->size > kMaxMediaBytes) {
		next();
		return;
	}
	fillMetadata(document);
	if (resolveSetTitle(document)) return;
	_media = document->createMediaView();
	_taskTime.start();
	if (_media->bytes().isEmpty() && document->filepath().isEmpty() && transferBusy()) {
		next();
		return;
	}
	const auto media = _media;
	media->automaticLoad(Data::FileOrigin(), nullptr);
	checkMedia();
}

void EmojiExporter::resolve() {
	const auto admission = _queue.resolve(_entries[_index].id, _clock.elapsed());
	if (admission == EmojiResolveAdmission::Continue) {
		_continuation.start(250);
		return;
	} else if (admission == EmojiResolveAdmission::Cooldown) {
		holdDemandForMetadataWait();
		next();
		return;
	}
	const auto token = _token;
	const auto id = _entries[_index].id;
	_request = _api->request(MTPmessages_GetCustomEmojiDocuments(
		MTP_vector<MTPlong>(QVector<MTPlong>{ MTP_long(id) })
	)).done([=](const MTPVector<MTPDocument> &result) {
		if (!_epoch.accepts(token) || !_queue.current() || _entries[_index].id != id) return;
		_request = 0;
		if (!admitted()) {
			reconcile();
			return;
		}
		for (const auto &value : result.v) {
			_session.get()->data().processDocument(value);
		}
		const auto sticker = _session.get()->data().document(id)->sticker();
		if (!sticker) {
			_queue.failed(id, _clock.elapsed());
			next();
		} else {
			_queue.succeeded(id);
			pump();
		}
	}).fail([=](const MTP::Error &) {
		if (!_epoch.accepts(token) || !_queue.current() || _entries[_index].id != id) return;
		_request = 0;
		if (admitted()) {
			_queue.failed(id, _clock.elapsed());
			next();
		} else {
			reconcile();
		}
	}).handleAllErrors().send();
}

void EmojiExporter::applySetTitle(quint64 id) {
	const auto title = _setTitles.lookup(id);
	if (!title) return;
	for (auto &entry : _entries) {
		if (entry.setId != id) continue;
		entry.setTitle = title->title;
		entry.setShortName = title->shortName;
	}
	for (auto &entry : _ready) {
		if (!entry || entry->setId != id) continue;
		entry->setTitle = title->title;
		entry->setShortName = title->shortName;
	}
}

bool EmojiExporter::resolveSetTitle(DocumentData *document) {
	const auto set = document->sticker()->set;
	if (_entries[_index].installed.value_or(false) || !set.accessHash
		|| !_setTitles.canBegin(set.id, _clock.elapsed())) return false;
	const auto admission = _queue.resolve(document->id, _clock.elapsed());
	if (admission == EmojiResolveAdmission::Continue) {
		_continuation.start(250);
		return true;
	} else if (admission == EmojiResolveAdmission::Cooldown) {
		return false;
	}
	if (!_setTitles.begin(set.id, _clock.elapsed())) return false;
	const auto token = _token;
	const auto id = document->id;
	_request = _api->request(MTPmessages_GetStickerSet(
		MTP_inputStickerSetID(MTP_long(set.id), MTP_long(set.accessHash)),
		MTP_int(0)
	)).done([=](const MTPmessages_StickerSet &result) {
		if (!_epoch.accepts(token) || !_queue.current() || _entries[_index].id != id
			|| _setTitles.active() != set.id) return;
		_request = 0;
		if (!admitted()) {
			reconcile();
			return;
		}
		result.match([&](const MTPDmessages_stickerSet &data) {
			const auto &metadata = data.vset().data();
			if (metadata.vid().v == set.id && !qs(metadata.vtitle()).isEmpty()) {
				_setTitles.complete(set.id, {qs(metadata.vtitle()), qs(metadata.vshort_name())});
			} else {
				_setTitles.failed(set.id, _clock.elapsed());
			}
		}, [&](const MTPDmessages_stickerSetNotModified &) {
			_setTitles.failed(set.id, _clock.elapsed());
		});
		applySetTitle(set.id);
		pump();
	}).fail([=](const MTP::Error &) {
		if (!_epoch.accepts(token) || !_queue.current() || _entries[_index].id != id
			|| _setTitles.active() != set.id) return;
		_request = 0;
		_setTitles.failed(set.id, _clock.elapsed());
		if (admitted()) pump();
		else reconcile();
	}).handleAllErrors().send();
	return true;
}

void EmojiExporter::deferred(mtpRequestId request, const SyncRequestTransient &transient) {
	if (request != _request || !_queue.current()) {
		return;
	}
	_request = 0;
	if (!admitted()) {
		reconcile();
		return;
	}
	const auto title = _setTitles.active();
	if (title) _setTitles.defer(*title);
	_queue.transient(transient.retryAt, base::unixtime::now(), _clock.elapsed(), transient.delayMs);
	if (title) {
		pump();
	} else {
		holdDemandForMetadataWait();
		next();
	}
}

void EmojiExporter::outputFailed() {
	revoke();
	_outputFailed = true;
}

void EmojiExporter::next() {
	if (_demandId && _queue.current() && _entries[_index].id == *_demandId) {
		const auto id = *_demandId;
		_largeRequests.complete(id, _demandRendered, _clock.elapsed());
		if (_demandRendered) {
			_afterPublicationAck.insert(id);
			_publishPending = true;
		} else {
			_acknowledge.insert(id);
		}
		_demandId.reset();
	}
	rememberTransfer();
	_media.reset();
	_queue.next();
	QTimer::singleShot(0, this, [=] { pump(); });
}

void EmojiExporter::checkMedia() {
	if (!admitted()) {
		reconcile();
		return;
	}
	if (!_media || _rendering) return;
	if (_scanDue) { maintainRequests(); return; }
	const auto document = _media->owner();
	const auto data = _media->bytes();
	const auto filepath = document->filepath();
	if (data.isEmpty() && filepath.isEmpty()) {
		if (!document->loading() || _taskTime.elapsed() >= kTaskTimeoutMs) {
			_queue.failed(_entries[_index].id, _clock.elapsed());
			next();
		}
		return;
	}
	const auto token = _token;
	const auto source = ExportSource(document->sticker()->type);
	const auto wantLarge = _largeIds.contains(document->id)
		|| _demandIds.contains(document->id) || _demandId == document->id;
	const auto tint = _entries[_index].textColor
		? std::make_optional(st::windowFg->c) : std::nullopt;
	const auto entry = _entries[_index];
	_media.reset();
	_rendering = true;
	const auto index = _index;
	const auto storageFailed = std::make_shared<bool>(false);
	const auto renderedEntry = std::make_shared<EmojiExportEntry>(entry);
	const auto cap = EmojiLargeCacheCap();
	const auto directory = _directory;
	const auto accepted = _worker.submit(_workerGeneration,
		[=](const EmojiExportAuthority &current) {
			const auto rendered = RenderEmojiExport(data, filepath, source, wantLarge);
			if (!current() || rendered.preview.isNull()) return false;
			const auto image = EmojiExportFrame(rendered.preview, tint);
			const auto ok = WriteEmojiExportPng(directory, entry.id, image, current);
			*storageFailed = !ok && current();
			if (!ok) return false;
			renderedEntry->sourceKind = EmojiExportSourceName(source);
			renderedEntry->sourcePx = rendered.sourcePx;
			if (!rendered.large.isNull()
				&& WriteEmojiLargePng(directory, entry.id, rendered.large, tint, current)) {
				renderedEntry->large = EmojiLargePngName(entry.id, rendered.sourcePx);
				if (!PruneEmojiLargeExport(directory, cap, current)) {
					*storageFailed = current();
					return false;
				}
			}
			return current();
		}, [=](quint64 generation, bool ok) {
			if (generation != _workerGeneration || !_epoch.accepts(token)) return;
			_rendering = false;
			if (!admitted()) {
				reconcile();
				if (_pending && !_running) _debounce.start(kDebounceMs);
				return;
			}
			if (*storageFailed) {
				outputFailed();
				return;
			}
			if (ok) {
				_ready[index] = *renderedEntry;
				if (_demandId == entry.id) _demandRendered = !renderedEntry->large.isEmpty();
				_queue.succeeded(entry.id);
			} else {
				_queue.failed(entry.id, _clock.elapsed());
			}
			next();
		});
	if (!accepted) {
		_rendering = false;
		_queue.failed(entry.id, _clock.elapsed());
		next();
	}
}

void EmojiExporter::publish() {
	const auto token = _token;
	const auto directory = _directory;
	const auto finished = !_queue.current();
	const auto previous = _published;
	const auto retired = std::make_shared<std::set<quint64>>(std::move(_retiredDemandIds));
	const auto published = std::make_shared<std::vector<EmojiExportEntry>>();
	const auto ready = std::make_shared<decltype(_ready)>(std::move(_ready));
	const auto acknowledge = std::make_shared<std::set<quint64>>(std::move(_afterPublicationAck));
	_rendering = true;
	const auto accepted = _worker.submit(_workerGeneration,
		[ready, previous, retired, published, acknowledge, finished, directory](const EmojiExportAuthority &current) {
			*published = MergeEmojiLargePublication(*previous, *ready, finished, current, *retired);
			return PublishEmojiLargeRequests(directory, *published,
				QDateTime::currentDateTimeUtc(), *acknowledge, current);
		}, [=](quint64 generation, bool ok) {
			if (generation != _workerGeneration || !_epoch.accepts(token)) return;
			_rendering = false;
			if (!admitted()) {
				reconcile();
				if (_pending && !_running) _debounce.start(kDebounceMs);
				return;
			}
			if (!ok) {
				outputFailed();
				return;
			}
			_ready = std::move(*ready);
			_published = published;
			_publishPending = false;
			if (!finished) { pump(); return; }
			_running = false;
			if (_pending) {
				_debounce.start(kDebounceMs);
			} else if (const auto retry = NextEmojiExportRetry(
				_queue.retryAt(_clock.elapsed()), _setTitles.retryAt(_clock.elapsed()))) {
				_pending = true;
				_debounce.start(int(std::clamp(*retry - _clock.elapsed(), qint64(1000), qint64(60000))));
			}
		});
	if (!accepted) {
		_rendering = false;
		outputFailed();
	}
}

QPointer<EmojiExporter> Exporter;

}

bool EmojiExportEnabled() {
	return Core::App().settings().readPref<bool>(kPreference, false);
}

void SetEmojiExportEnabled(bool enabled) {
	if (enabled != EmojiExportEnabled()) {
		Core::App().settings().writePref<bool>(kPreference, enabled);
		Core::App().saveSettingsDelayed();
		PreferenceChanges.fire_copy(enabled);
	}
}

rpl::producer<bool> EmojiExportEnabledValue() {
	return rpl::single(EmojiExportEnabled()) | rpl::then(PreferenceChanges.events());
}

QString CurrentEmojiExportDirectory() {
	return EmojiExportDirectory(CanonicalDirectory(QDir::homePath()),
		CanonicalDirectory(cWorkingDir()), CanonicalDirectory(psAppDataPath()));
}

void StartEmojiExport() {
	if (!Exporter) {
		Exporter = new EmojiExporter();
	}
}

void StopEmojiExportAtLogout(Main::Account &account) {
	if (Exporter) {
		Exporter->logout(account);
	}
}

void BuildEmojiExportSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto toggle = builder.addButton({
		.id = u"advanced/purple_emoji_export"_q,
		.title = tr::lng_purple_emoji_export(),
		.st = &st::settingsButtonNoIcon,
		.toggled = EmojiExportEnabledValue(),
		.keywords = { u"purple"_q, u"emoji"_q, u"export"_q, u"local"_q },
	});
	if (toggle) {
		toggle->toggledValue() | rpl::filter([](bool enabled) {
			return enabled != EmojiExportEnabled();
		}) | rpl::on_next([](bool enabled) {
			SetEmojiExportEnabled(enabled);
		}, toggle->lifetime());
	}
	BuildEmojiLargeCacheSetting(builder);
	builder.addDividerText(tr::lng_purple_emoji_export_about(
		lt_path, rpl::single(CurrentEmojiExportDirectory())));
}

}
