#include "purple/purple_user_control.h"

#include "base/base_file_utilities.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/media/history_view_media.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_add_action_callback.h"

#include "styles/style_menu_icons.h"

namespace Purple {
namespace {

struct SelfDestructDocumentSaveSource {
	explicit SelfDestructDocumentSaveSource(const void *identity)
	: state(identity) {
	}

	SelfDestructSaveSource state;
	rpl::lifetime lifetime;
};

rpl::event_stream<not_null<HistoryItem*>> &SelfDestructSaveInvalidations() {
	static auto result = rpl::event_stream<not_null<HistoryItem*>>();
	return result;
}

std::optional<crl::time> SelfDestructDeadline(not_null<HistoryItem*> item) {
	const auto selfdestruct = item->Get<HistoryServiceSelfDestruct>();
	if (!selfdestruct) {
		return crl::time(0);
	}
	const auto at = std::get_if<crl::time>(&selfdestruct->destructAt);
	return at ? std::make_optional(*at) : std::nullopt;
}

class SelfDestructSaveMedia final
: public Data::Media
, public base::has_weak_ptr {
public:
	SelfDestructSaveMedia(
		not_null<HistoryItem*> parent,
		std::unique_ptr<Data::Media> media,
		crl::time ttlSeconds);

	[[nodiscard]] bool active() const;
	void consume();
	void save();
	std::unique_ptr<Data::Media> clone(not_null<HistoryItem*> parent) override;
	DocumentData *document() const override;
	PhotoData *photo() const override;
	TextWithEntities notificationText() const override;
	QString pinnedTextSubstring() const override;
	TextForMimeData clipboardText() const override;
	bool allowsForward() const override;
	crl::time ttlSeconds() const override;
	bool updateInlineResultMedia(const MTPMessageMedia &media) override;
	bool updateSentMedia(const MTPMessageMedia &media) override;
	std::unique_ptr<HistoryView::Media> createView(
		not_null<HistoryView::Element*> message,
		not_null<HistoryItem*> realParent,
		HistoryView::Element *replacing) override;

private:
	void saveToPath(const QString &path);
	void savePhotoWhenLoaded();

	const std::unique_ptr<Data::Media> _media;
	const crl::time _ttlSeconds = 0;
	SelfDestructSaveSource _source{ this };
	std::shared_ptr<Data::PhotoMedia> _savePhoto;
	QString _savePhotoPath;
	rpl::lifetime _saveLifetime;

};

SelfDestructSaveMedia::SelfDestructSaveMedia(
	not_null<HistoryItem*> parent,
	std::unique_ptr<Data::Media> media,
	crl::time ttlSeconds)
: Media(parent)
, _media(std::move(media))
, _ttlSeconds(ttlSeconds) {
}

bool SelfDestructSaveMedia::active() const {
	return parent()->Has<HistoryServiceSelfDestruct>()
		&& _source.active(
			parent()->media(), SelfDestructDeadline(parent()), crl::now());
}

void SelfDestructSaveMedia::consume() {
	_source.consume();
}

void SelfDestructSaveMedia::save() {
	if (!active()) {
		return;
	}
	const auto doc = document();
	const auto filename = doc
		? base::FileNameFromUserString(doc->filename())
		: QString();
	const auto initialPath = !filename.isEmpty()
		? filename
		: doc
		? filedialogDefaultName(u"video"_q, u".mp4"_q)
		: filedialogDefaultName(u"photo"_q, u".jpg"_q);
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		doc ? tr::lng_save_video(tr::now) : tr::lng_save_photo(tr::now),
		doc
			? FileDialog::AllFilesFilter()
			: u"JPEG Image (*.jpg);;"_q + FileDialog::AllFilesFilter(),
		initialPath,
		crl::guard(this, [=](QString &&path) {
			saveToPath(path);
		}));
}

void SelfDestructSaveMedia::saveToPath(const QString &path) {
	if (path.isEmpty() || !active()) {
		return;
	}
	if (const auto doc = document()) {
		doc->save(parent()->fullId(), path);
		return;
	}
	_saveLifetime.destroy();
	_savePhoto = photo()->createMediaView();
	_savePhotoPath = path;
	_savePhoto->wanted(Data::PhotoSize::Large, parent()->fullId());
	if (_savePhoto->loaded()) {
		savePhotoWhenLoaded();
	} else {
		parent()->history()->session().downloaderTaskFinished(
		) | rpl::on_next([=] {
			savePhotoWhenLoaded();
		}, _saveLifetime);
	}
}

void SelfDestructSaveMedia::savePhotoWhenLoaded() {
	if (!active()) {
		_saveLifetime.destroy();
		_savePhoto = nullptr;
		_savePhotoPath.clear();
	} else if (_savePhoto && _savePhoto->loaded()) {
		_saveLifetime.destroy();
		const auto photo = base::take(_savePhoto);
		photo->saveToFile(base::take(_savePhotoPath));
	}
}

std::unique_ptr<Data::Media> SelfDestructSaveMedia::clone(
		not_null<HistoryItem*> parent) {
	return std::make_unique<SelfDestructSaveMedia>(
		parent,
		_media->clone(parent),
		_ttlSeconds);
}

DocumentData *SelfDestructSaveMedia::document() const {
	return _media->document();
}

PhotoData *SelfDestructSaveMedia::photo() const {
	return _media->photo();
}

TextWithEntities SelfDestructSaveMedia::notificationText() const {
	return _media->notificationText();
}

QString SelfDestructSaveMedia::pinnedTextSubstring() const {
	return _media->pinnedTextSubstring();
}

TextForMimeData SelfDestructSaveMedia::clipboardText() const {
	return {};
}

bool SelfDestructSaveMedia::allowsForward() const {
	return false;
}

crl::time SelfDestructSaveMedia::ttlSeconds() const {
	return _ttlSeconds;
}

bool SelfDestructSaveMedia::updateInlineResultMedia(
		const MTPMessageMedia &media) {
	return false;
}

bool SelfDestructSaveMedia::updateSentMedia(const MTPMessageMedia &media) {
	return false;
}

std::unique_ptr<HistoryView::Media> SelfDestructSaveMedia::createView(
		not_null<HistoryView::Element*> message,
		not_null<HistoryItem*> realParent,
		HistoryView::Element *replacing) {
	return nullptr;
}

}

SelfDestructSaveSource::SelfDestructSaveSource(const void *identity)
: _identity(identity) {
}

bool SelfDestructSaveSource::active(
		const void *current,
		std::optional<crl::time> destructAt,
		crl::time now) const {
	return !_consumed
		&& _identity
		&& current == _identity
		&& destructAt
		&& (!*destructAt || *destructAt > now);
}

void SelfDestructSaveSource::consume() {
	_consumed = true;
}

bool SelfDestructHasServicePlaceholder(const MTPMessageMedia *media) {
	return media && media->match([](const MTPDmessageMediaPhoto &data) {
		return data.vttl_seconds() && data.vttl_seconds()->v > 0;
	}, [](const MTPDmessageMediaDocument &data) {
		return data.is_video()
			&& data.vttl_seconds()
			&& data.vttl_seconds()->v > 0;
	}, [](const auto &) {
		return false;
	});
}

bool AddSelfDestructDocumentSaveAction(
		const Ui::Menu::MenuCallback &addAction,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	const auto media = item->media();
	if (!media || !media->ttlSeconds()) {
		return false;
	}
	const auto source = std::make_shared<SelfDestructDocumentSaveSource>(media);
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	SelfDestructSaveInvalidations().events(
	) | rpl::filter([=](not_null<HistoryItem*> changed) {
		return changed == item;
	}) | rpl::on_next([raw = source.get()] {
		raw->state.consume();
	}, source->lifetime);
	session->data().itemRemoved(id) | rpl::on_next([raw = source.get()] {
		raw->state.consume();
	}, source->lifetime);
	const auto active = [=] {
		const auto current = session->data().message(id);
		return current
			&& current->media()
			&& current->media()->document() == document
			&& source->state.active(
				current->media(), SelfDestructDeadline(current), crl::now());
	};
	if (active()) {
		addAction(
			document->isVoiceMessage()
				? tr::lng_context_save_audio(tr::now)
				: tr::lng_context_save_video(tr::now),
			crl::guard(session, [=] {
				if (!active()) {
					return;
				}
				const auto filename = base::FileNameFromUserString(
					document->filename());
				FileDialog::GetWritePath(
					Core::App().getFileDialogParent(),
					tr::lng_save_file(tr::now),
					FileDialog::AllFilesFilter(),
					filename.isEmpty()
						? filedialogDefaultName(
							u"file"_q,
							document->isVoiceMessage() ? u".ogg"_q : u".mp4"_q)
						: filename,
					crl::guard(session, [=](QString &&path) {
						if (!path.isEmpty() && active()) {
							document->save(id, path);
						}
					}));
			}),
			&st::menuIconDownload);
	}
	return true;
}

bool CanSaveSelfDestructMedia(const MTPMessageMedia &media, bool unread) {
	if (!SaveRestrictionsDisabled() || !unread) {
		return false;
	}
	return media.match([](const MTPDmessageMediaPhoto &data) {
		const auto photo = data.vphoto();
		return data.vttl_seconds()
			&& data.vttl_seconds()->v > 0
			&& photo
			&& photo->type() == mtpc_photo;
	}, [](const MTPDmessageMediaDocument &data) {
		const auto document = data.vdocument();
		return data.vttl_seconds()
			&& data.vttl_seconds()->v > 0
			&& data.is_video()
			&& document
			&& document->type() == mtpc_document;
	}, [](const auto &) {
		return false;
	});
}

std::unique_ptr<Data::Media> SelfDestructMediaForSaving(
		not_null<HistoryItem*> item,
		const MTPMessageMedia &media) {
	SelfDestructSaveInvalidations().fire_copy(item);
	if (!item->isService()
		|| !item->Has<HistoryServiceSelfDestruct>()
		|| !CanSaveSelfDestructMedia(media, item->hasUnreadMediaFlag())) {
		return nullptr;
	}
	using Result = std::unique_ptr<Data::Media>;
	const auto owner = &item->history()->owner();
	return media.match([&](const MTPDmessageMediaPhoto &data) -> Result {
		return std::make_unique<SelfDestructSaveMedia>(
			item,
			std::make_unique<Data::MediaPhoto>(
				item,
				owner->processPhoto(*data.vphoto()),
				data.is_spoiler()),
			data.vttl_seconds()->v);
	}, [&](const MTPDmessageMediaDocument &data) -> Result {
		return std::make_unique<SelfDestructSaveMedia>(
			item,
			std::make_unique<Data::MediaFile>(
				item,
				owner->processDocument(*data.vdocument()),
				Data::MediaFile::Args{}),
			data.vttl_seconds()->v);
	}, [](const auto &) -> Result {
		return nullptr;
	});
}

void ExpireSelfDestructMedia(not_null<HistoryItem*> item, bool consumed) {
	if (consumed) {
		SelfDestructSaveInvalidations().fire_copy(item);
	}
	const auto media = dynamic_cast<SelfDestructSaveMedia*>(item->media());
	if (media && consumed) {
		media->consume();
	}
	if (media && (consumed || !media->active())) {
		item->clearMediaAsExpired();
	}
}

void AddSelfDestructSaveAction(
		not_null<Ui::PopupMenu*> menu,
		const HistoryView::ContextMenuRequest &request) {
	if (request.selectedItems.empty()) {
		AddSelfDestructSaveAction(menu, request.item);
	}
}

void AddSelfDestructSaveAction(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item) {
	const auto media = item
		? dynamic_cast<SelfDestructSaveMedia*>(item->media())
		: nullptr;
	if (!media || !media->active()) {
		return;
	}
	menu->addAction(
		tr::lng_context_save_image(tr::now),
		crl::guard(media, [=] { media->save(); }),
		&st::menuIconDownload);
}

}
