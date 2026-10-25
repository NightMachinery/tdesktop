#include "purple/purple_archive_media_box.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_player.h"
#include "media/streaming/media_streaming_reader.h"
#include "purple/purple_archive_media_loader.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include <QtCore/QBuffer>
#include <QtCore/QMimeDatabase>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QImageReader>
#include "styles/style_layers.h"

namespace Purple {
namespace {

constexpr auto kPhotoPreviewBytes = uint64_t(16 * 1024 * 1024);
constexpr auto kPhotoPreviewPixels = int64_t(8 * 1024 * 1024);
constexpr auto kFrameInterval = 33;

struct MediaBoxState {
	std::shared_ptr<ArchiveMediaAccess> access;
	std::unique_ptr<Media::Streaming::Player> player;
	QImage image;
	QByteArray photo;
	Fn<void()> readPhoto;
	bool closed = false;
	bool saving = false;
};

QString ExportName(const QString &mime, bool photo) {
	const auto suffix = photo ? u"jpg"_q
		: mime == u"audio/ogg"_q || mime == u"application/ogg"_q ? u"ogg"_q
		: QMimeDatabase().mimeTypeForName(mime).preferredSuffix();
	return u"archive-attachment."_q + (suffix.isEmpty() ? u"bin"_q : suffix);
}

void DecodePhoto(QPointer<Ui::GenericBox> box, MediaBoxState *state,
	not_null<Ui::RpWidget*> canvas, not_null<Ui::FlatLabel*> label,
	std::function<bool()> current) {
	auto bytes = std::move(state->photo);
	crl::async([box, state, canvas, label, current, bytes = std::move(bytes)]() mutable {
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::ReadOnly);
		auto reader = QImageReader(&buffer);
		const auto size = reader.size();
		auto image = size.width() > 0 && size.height() > 0
			&& int64_t(size.width()) * size.height() <= kPhotoPreviewPixels
			? reader.read() : QImage();
		DesktopArchiveCrypto::Clear(bytes);
		crl::on_main([box, state, canvas, label, current, image = std::move(image)]() mutable {
			if (!box || !current()) {
				return;
			}
			if (image.isNull()) {
				label->setText(tr::lng_purple_archive_attachment_unsupported(tr::now));
				return;
			}
			state->image = std::move(image);
			label->setText(QString());
			canvas->update();
		});
	});
}

}

void ShowArchiveMedia(not_null<Window::SessionController*> controller,
	ArchiveMediaRequest request, QString mime, bool photo,
	std::function<bool()> originCurrent) {
	const auto session = base::make_weak(&controller->session());
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_purple_archive_attachment());
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_archive_attachment_notice(tr::now), st::boxLabel));
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_archive_opening(tr::now), st::boxLabel));
		const auto canvas = box->addRow(object_ptr<Ui::RpWidget>(box));
		canvas->resize(st::boxWideWidth, st::boxWideWidth);
		const auto state = box->lifetime().make_state<MediaBoxState>();
		const auto guard = QPointer<Ui::GenericBox>(box);
		const auto current = [=] {
			return guard && !state->closed && session && originCurrent && originCurrent();
		};
		const auto stop = [state] {
			state->closed = true;
			if (state->access) {
				state->access->close();
			}
			state->player.reset();
			state->image = QImage();
			DesktopArchiveCrypto::Clear(state->photo);
		};
		box->boxClosing() | rpl::on_next(stop, box->lifetime());
		box->lifetime().add(stop);
		session.get()->lifetime().add([guard, stop] {
			if (guard) {
				stop();
				guard->closeBox();
			}
		});
		controller->window().sessionControllerChanges() | rpl::on_next([=](auto) {
			if (!current()) {
				stop();
				box->closeBox();
			}
		}, box->lifetime());
		const auto check = [=] {
			const auto valid = current();
			if (!valid || (state->access && !state->access->current())) {
				if (valid) {
					box->uiShow()->showToast(tr::lng_purple_archive_attachment_failed(tr::now));
				}
				stop();
				box->closeBox();
			}
		};
		rpl::merge(SettingsChanges(), ActiveChanges(), ArchiveStorageChanges())
			| rpl::on_next(check, box->lifetime());
		canvas->paintRequest() | rpl::on_next([=] {
			if (!current() || !state->access || !state->access->current()) {
				return;
			}
			auto request = Media::Streaming::FrameRequest();
			request.resize = request.outer = canvas->size();
			auto image = state->player ? state->player->frame(request) : state->image;
			if (!image.isNull()) {
				const auto size = image.size().scaled(canvas->size(), Qt::KeepAspectRatio);
				auto painter = Painter(canvas);
				painter.drawImage(QRect(QPoint((canvas->width() - size.width()) / 2,
					(canvas->height() - size.height()) / 2), size), image);
				if (state->player) {
					state->player->markFrameShown();
				}
			}
		}, canvas->lifetime());
		const auto timer = new QTimer(box);
		timer->setInterval(kFrameInterval);
		QObject::connect(timer, &QTimer::timeout, box, [=] {
			check();
			if (!current()) {
				return;
			}
			if (state->player) {
				canvas->update();
			}
		});
		timer->start();
		const auto pause = box->addRow(object_ptr<Ui::RoundButton>(box,
			tr::lng_purple_archive_attachment_pause(), st::defaultBoxButton));
		pause->hide();
		pause->setClickedCallback([=] {
			if (current() && state->player) {
				if (state->player->paused()) {
					state->player->resume();
				} else {
					state->player->pause();
				}
			}
		});
		const auto restart = box->addRow(object_ptr<Ui::RoundButton>(box,
			tr::lng_purple_archive_attachment_restart(), st::defaultBoxButton));
		restart->hide();
		restart->setClickedCallback([=] {
			if (current() && state->player) {
				state->player->play(Media::Streaming::PlaybackOptions());
			}
		});
		const auto save = box->addRow(object_ptr<Ui::RoundButton>(box,
			tr::lng_purple_archive_attachment_save(), st::defaultBoxButton));
		save->setDisabled(true);
		save->setClickedCallback([=] {
			if (!current() || !state->access || state->saving) {
				return;
			}
			FileDialog::GetWritePath(Core::App().getFileDialogParent(),
				tr::lng_purple_archive_attachment_save(tr::now), FileDialog::AllFilesFilter(),
				ExportName(mime, photo),
				[=](QString &&path) {
					if (path.isEmpty() || !current() || !state->access->current()) {
						return;
					}
					state->saving = true;
					label->setText(tr::lng_purple_archive_attachment_saving(tr::now));
					state->access->save(std::move(path), [=](bool ok) {
						if (current()) {
							state->saving = false;
							label->setText(ok ? tr::lng_purple_archive_attachment_saved(tr::now)
								: tr::lng_purple_archive_attachment_save_failed(tr::now));
						}
					});
				});
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		if (!current()) {
			box->closeBox();
			return;
		}
		OpenArchiveMedia(session.get(), request, current,
			[=](std::shared_ptr<ArchiveMediaAccess> access) {
				if (!current()) {
					return;
				}
				if (!access) {
					label->setText(tr::lng_purple_archive_attachment_failed(tr::now));
					return;
				}
				state->access = std::move(access);
				save->setDisabled(false);
				if (photo && state->access->size() <= kPhotoPreviewBytes) {
					state->readPhoto = [=] {
						if (!current() || !state->access->current()) {
							return;
						}
						if (uint64_t(state->photo.size()) == state->access->size()) {
							DecodePhoto(guard, state, canvas, label, current);
							return;
						}
						const auto length = uint32_t(std::min(uint64_t(kArchiveMediaChunkBytes),
							state->access->size() - state->photo.size()));
						state->access->read(state->photo.size(), length, [=](std::optional<QByteArray> bytes) {
							if (!current()) {
								return;
							}
							if (!bytes) {
								label->setText(tr::lng_purple_archive_attachment_failed(tr::now));
								return;
							}
							state->photo += *bytes;
							state->readPhoto();
						});
					};
					state->readPhoto();
				} else if (!photo && (mime.startsWith(u"video/"_q) || mime.startsWith(u"audio/"_q)
					|| mime == u"application/ogg"_q) && ArchiveMediaLoader::CanOpen(state->access->size())) {
					const auto reader = std::make_shared<Media::Streaming::Reader>(
						std::make_unique<ArchiveMediaLoader>(state->access), nullptr, true);
					state->player = std::make_unique<Media::Streaming::Player>(reader);
					pause->show();
					restart->show();
					state->player->updates() | rpl::on_error([=](Media::Streaming::Error) {
						if (current()) {
							pause->hide();
							restart->hide();
							label->setText(tr::lng_purple_archive_attachment_unsupported(tr::now));
						}
					}, box->lifetime());
					label->setText(QString());
					state->player->play(Media::Streaming::PlaybackOptions());
				} else {
					label->setText(tr::lng_purple_archive_attachment_unsupported(tr::now));
				}
			});
	}));
}

}
