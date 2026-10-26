#include "purple/purple_emoji_export.h"

#include "purple/purple_emoji_export_store.h"
#include "purple/purple_emoji_export_queue.h"
#include "purple/purple_emoji_export_worker.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "ffmpeg/ffmpeg_frame_generator.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_common.h"
#include "lottie/lottie_frame_generator.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
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

QImage RenderEmoji(
		const QByteArray &data,
		const QString &filepath,
		StickerType type,
		std::optional<QColor> tint) {
	if (data.size() > kMaxMediaBytes
		|| (data.isEmpty() && QFileInfo(filepath).size() > kMaxMediaBytes)) {
		return {};
	}
	const auto bytes = Lottie::ReadContent(data, filepath);
	auto generator = std::unique_ptr<Ui::FrameGenerator>();
	switch (type) {
	case StickerType::Tgs:
		generator = std::make_unique<Lottie::FrameGenerator>(bytes);
		break;
	case StickerType::Webm:
		generator = std::make_unique<FFmpeg::FrameGenerator>(bytes);
		break;
	case StickerType::Webp:
		generator = std::make_unique<Ui::ImageFrameGenerator>(bytes);
		break;
	}
	return generator ? EmojiExportFrame(generator->renderNext(QImage(),
		QSize(kEmojiExportPixels, kEmojiExportPixels), Qt::KeepAspectRatio).image, tint)
		: QImage();
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
	QTimer _poll;
	QElapsedTimer _taskTime;
	base::weak_ptr<Main::Session> _session;
	base::weak_ptr<Main::Session> _transferSession;
	base::weak_ptr<Main::Session> _retiredSession;
	DocumentId _transferId = 0;
	std::unique_ptr<MTP::Sender> _api;
	mtpRequestId _request = 0;
	std::shared_ptr<Data::DocumentMedia> _media;
	std::vector<EmojiExportEntry> _entries;
	std::vector<std::optional<EmojiExportEntry>> _ready;
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
		if (_transferId && !transferBusy()) {
			dirty();
		}
		if (_request && _taskTime.elapsed() >= kTaskTimeoutMs) {
			_api->request(_request).cancel();
			_request = 0;
			_queue.failed(_entries[_index].id, _clock.elapsed());
			next();
		} else if (_media) {
			checkMedia();
		} else if (_running) {
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
	Core::App().settings().recentEmojiUpdated() | rpl::on_next([=] {
		dirty();
	}, _lifetime);
	style::PaletteChanged() | rpl::on_next([=] { dirty(); }, _lifetime);
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
		_api = std::make_unique<MTP::Sender>(&desired->mtp());
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
	_queue.clear();
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
	const auto session = _session.get();
	auto rank = 0;
	AppendBoundedEmojiExportEntries(_entries, Core::App().settings().recentEmoji(),
		kMaxEntries, [&](const Core::RecentEmoji &recent) -> std::optional<EmojiExportEntry> {
			++rank;
			const auto document = std::get_if<Core::RecentEmojiDocument>(&recent.id.data);
			return document && document->id && document->test == session->isTestMode()
				? std::optional(EmojiExportEntry{ .id = document->id, .recentRank = rank })
				: std::nullopt;
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
	const auto i = sets.find(entry.setId);
	if (i != sets.end()) {
		entry.setTitle = i->second->title;
		entry.setShortName = i->second->shortName;
	}
}

void EmojiExporter::pump() {
	if (!admitted()) {
		reconcile();
		return;
	}
	if (!_running || _rendering || _media || _request || _continuation.isActive()) {
		return;
	}
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
		next();
		return;
	}
	const auto token = _token;
	const auto id = _entries[_index].id;
	_taskTime.start();
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
	}).fail([=] {
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

void EmojiExporter::outputFailed() {
	revoke();
	_outputFailed = true;
}

void EmojiExporter::next() {
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
	if (!_media || _rendering) {
		return;
	}
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
	const auto type = document->sticker()->type;
	const auto tint = _entries[_index].textColor
		? std::make_optional(st::windowFg->c) : std::nullopt;
	const auto entry = _entries[_index];
	_media.reset();
	_rendering = true;
	const auto index = _index;
	const auto storageFailed = std::make_shared<bool>(false);
	const auto directory = _directory;
	const auto accepted = _worker.submit(_workerGeneration,
		[=](const EmojiExportAuthority &current) {
			const auto image = RenderEmoji(data, filepath, type, tint);
			if (!current() || image.isNull()) return false;
			const auto ok = WriteEmojiExportPng(directory, entry.id, image, current);
			*storageFailed = !ok && current();
			return ok;
		}, [=](quint64 generation, bool ok) {
			_rendering = false;
			if (generation != _workerGeneration || !_epoch.accepts(token) || !admitted()) {
				reconcile();
				if (_pending && !_running) _debounce.start(kDebounceMs);
				return;
			}
			if (*storageFailed) {
				outputFailed();
				return;
			}
			if (ok) {
				_ready[index] = entry;
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
	_rendering = true;
	const auto accepted = _worker.submit(_workerGeneration,
		[ready = std::move(_ready), directory](const EmojiExportAuthority &current) {
			auto entries = std::vector<EmojiExportEntry>();
			entries.reserve(ready.size());
			for (const auto &entry : ready) {
				if (!current()) return false;
				if (entry) entries.push_back(*entry);
			}
			return PublishEmojiExportCatalog(directory, entries,
				QDateTime::currentDateTimeUtc(), current);
		}, [=](quint64 generation, bool ok) {
			_rendering = false;
			if (generation != _workerGeneration || !_epoch.accepts(token) || !admitted()) {
				reconcile();
				if (_pending && !_running) _debounce.start(kDebounceMs);
				return;
			}
			if (!ok) {
				outputFailed();
				return;
			}
			_running = false;
			if (_pending) {
				_debounce.start(kDebounceMs);
			} else if (const auto retry = _queue.retryAt()) {
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
	builder.addDividerText(tr::lng_purple_emoji_export_about(
		lt_path, rpl::single(CurrentEmojiExportDirectory())));
}

}
