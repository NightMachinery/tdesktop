#include "purple/purple_archive_v2_boxes.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "core/file_utilities.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_previous.h"
#include "purple/purple_archive_v2_service.h"
#include "purple/purple_archive_v2_ui.h"
#include "purple/purple_config.h"
#include "purple/purple_privacy_screen.h"
#include "storage/storage_account.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QSaveFile>

namespace Purple {
namespace {

using namespace ArchiveV2;

struct Route {
	explicit Route(not_null<Window::SessionController*> window)
	: host(base::make_weak(window.get()))
	, account(base::make_weak(&window->session()))
	, lease(SessionLease(&window->session()))
	, epoch(ViewEpoch()) {
	}

	Main::Session *current() const {
		const auto window = host.get();
		const auto session = account.get();
		return epoch == ViewEpoch() && lease && lease->live.load() && window && session && !Core::Quitting()
			&& SessionLease(session) == lease && window->window().maybeSession() == session
			&& !window->window().locked() && !Core::App().screenIsLocked() && !PrivacyScreenActiveAny()
			? session : nullptr;
	}

	void show(Fn<void(not_null<Ui::GenericBox*>)> fill) const {
		if (!current()) return;
		const auto route = *this;
		host->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
			if (const auto live = route.current()) {
				RegisterView(live, box);
				fill(box);
			}
		}), Ui::LayerOption::CloseOther);
	}

	base::weak_ptr<Window::SessionController> host;
	base::weak_ptr<Main::Session> account;
	std::shared_ptr<Lease> lease;
	uint64_t epoch;
};

void Failed(const Route &route, QString reason) {
	if (const auto live = route.current()) {
		crl::async([reason] {
			auto attention = Attention();
			(void)LogIncident(attention, IncidentStage::Cleanup, reason, false);
		});
		PublishIncident(live, { std::move(reason), false, IncidentStage::Cleanup }, ArchiveWarning::None);
	}
}

template <typename Work, typename Result>
void Read(const Route &route, Work read, Result ready) {
	const auto live = route.current();
	if (live && !Query(live, [=](Store &store) {
		if (route.epoch != ViewEpoch() || !route.lease->live.load()) return;
		auto value = read(store);
		crl::on_main([=, value = std::move(value)]() mutable {
			if (route.current()) ready(std::move(value));
		});
	})) Failed(route, u"store_busy"_q);
}

void Save(const Route &route, MessageIdentity item, int64 sequence, int64 ordinal) {
	FileDialog::GetWritePath(Core::App().getFileDialogParent(), tr::lng_save_file(tr::now),
		FileDialog::AllFilesFilter(), u"attachment"_q, [=](QString &&path) {
			if (path.isEmpty()) return;
			Read(route, [=](Store &store) {
				auto file = QSaveFile(path);
				file.setDirectWriteFallback(false);
				const auto saved = file.open(QIODevice::WriteOnly) && store.readSaved(store.id(),
					item, sequence, ordinal, [&](const QByteArray &part) {
						return route.epoch == ViewEpoch() && file.write(part) == part.size();
					});
				return saved && route.epoch == ViewEpoch() && store.current() && file.commit();
			}, [=](bool ok) { if (!ok) Failed(route, u"copy_incomplete"_q); });
		});
}

void History(const Route &route, MessageIdentity item, int64 before = 0) {
	Read(route, [=](Store &store) { return store.history(item, before, 12); }, [=](auto page) {
		if (!page) return Failed(route, u"store_unreadable"_q);
		route.show([=](not_null<Ui::GenericBox*> box) {
			FillHistoryBox(box, *page, ArchivedText, [=] { return route.current() != nullptr; },
				[=](int64 cursor) { History(route, item, cursor); }, {
					.saving = [=](int64 seq, int64 slot) { return Saving(route.current(), item, seq, slot); },
					.openSaved = [=](int64 seq, int64 slot) { Save(route, item, seq, slot); },
				});
		});
	});
}

void Timeline(const Route &route, std::optional<PeerToken> peer, bool deleted,
		std::optional<TimelineCursor> before = {}) {
	Read(route, [=](Store &store) { return store.timeline(peer, deleted, before, 16); }, [=](auto page) {
		if (!page) return Failed(route, u"store_unreadable"_q);
		route.show([=](not_null<Ui::GenericBox*> box) {
			FillTimelineBox(box, *page, deleted, ArchivedText, [=] { return route.current() != nullptr; },
				[=](TimelineCursor cursor) { Timeline(route, peer, deleted, cursor); });
		});
	});
}

void Confirm(const Route &route, QString title, QString detail, Fn<void()> submit) {
	route.show([=](not_null<Ui::GenericBox*> box) {
		FillArchiveConfirm(box, title, detail, [=] { return route.current() != nullptr; }, submit);
	});
}

void CleanupNext(const Route &route, CleanupTicket ticket) {
	Read(route, [=](Store &store) { return store.cleanupPage(ticket); }, [=](CleanupResult result) {
		if (!result.effect) Failed(route, result.effect.reason);
		else if (!result.complete) CleanupNext(route, ticket);
	});
}

void Cleanup(const Route &route, CleanupKind kind) {
	Read(route, [=](Store &store) { return store.prepareCleanup(kind); }, [=](auto ticket) {
		if (!ticket) return Failed(route, u"store_unreadable"_q);
		const auto trash = kind == CleanupKind::DeletedMessages;
		Confirm(route, trash ? tr::lng_purple_archive_empty_trash(tr::now) : tr::lng_purple_archive_clear_edits(tr::now),
			tr::lng_purple_archive_purge_scope_notice(tr::now), [=] {
				if (const auto live = route.current()) CancelArchiveDownloads(live);
				CleanupNext(route, *ticket);
			});
	});
}

PreviousArchiveOptions LegacyOptions(const Route &route) {
	const auto session = route.current();
	auto keys = QList<QByteArray>();
	for (const auto &key : session->account().mtp().getKeysForWrite()) {
		if (key && key->type() != MTP::AuthKey::Type::Temporary && key->type() != MTP::AuthKey::Type::Local) {
			keys.push_back(QByteArray::number(key->keyId(), 16).rightJustified(16, '0'));
		}
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return { ConfigDirectory() + u"/content-archive"_q, cWorkingDir() + u"tdata"_q,
		SessionAccountKey(session), cDataFile(), session->local().readPref<QByteArray>("purple.archive.login_authority.v1"),
		keys.join(','), [lease = route.lease, epoch = route.epoch] { return lease->live.load() && epoch == ViewEpoch(); } };
}

void ShowStorage(const Route &route) {
	const auto live = route.current();
	if (!live) return;
	const auto label = live->user()->name();
	const auto options = LegacyOptions(route);
	Read(route, [=](Store &store) {
		const auto summary = store.storageSummary();
		if (summary) {
			const auto &active = summary->active;
			LOG(("Purple Archive: event=storage db_bytes=%1 wal_bytes=%2 shm_bytes=%3 support_bytes=%4.")
				.arg(active.database).arg(active.wal).arg(active.sharedMemory).arg(active.support));
		}
		return std::pair(summary, PreviousArchive::ReviewPreviousArchive(options));
	}, [=](auto value) {
		const auto &[summary, previous] = value;
		const auto reviewPrevious = [=] {
			if (!previous.review) {
				Failed(route, previous.reason);
				return;
			}
			route.show([=](not_null<Ui::GenericBox*> box) {
				FillPreviousArchiveConfirm(box, label, previous.review->bytes(), [=] { return route.current() != nullptr; },
					[=] {
						Read(route, [=](Store &) { return PreviousArchive::DeletePreviousArchive(options, *previous.review); },
							[=](auto result) { if (!result) Failed(route, result.reason); });
					});
			});
		};
		route.show([=](not_null<Ui::GenericBox*> box) {
			FillStorageBox(box, summary, previous.review ? std::optional(previous.review->bytes()) : std::nullopt,
				[=] { return route.current() != nullptr; }, [=](CleanupKind kind) { Cleanup(route, kind); },
				previous.review || !previous.reason.isEmpty() ? Fn<void()>(reviewPrevious) : Fn<void()>());
		});
	});
}

}

void OpenArchiveHistory(not_null<Window::SessionController*> window, FullMsgId item) {
	if (item.peer && IsServerMsgId(item.msg)) History(Route(window), { Peer(item.peer), uint64(item.msg.bare) });
}

void OpenArchivedMessages(not_null<Window::SessionController*> window, std::optional<PeerToken> peer, bool deleted) {
	Timeline(Route(window), peer, deleted);
}

void OpenArchiveStorage(not_null<Window::SessionController*> window) {
	ShowStorage(Route(window));
}

void ReviewArchiveReset(not_null<Window::SessionController*> window) {
	const auto route = Route(window);
	Read(route, [](Store &store) { return store.reviewReset(store.id()); }, [=](auto result) {
		const auto review = std::get_if<ResetReview>(&result);
		if (!review) return Failed(route, std::get<QString>(result));
		const auto detail = tr::lng_purple_archive_discard_confirm(tr::now) + u" "_q + ArchiveSize(review->bytes());
		Confirm(route, tr::lng_purple_archive_discard(tr::now), detail, [=, review = *review] {
			if (const auto live = route.current()) RequestReset(live, review);
		});
	});
}

void AddArchiveHistoryAction(not_null<Ui::PopupMenu*> menu, not_null<HistoryView::ListWidget*> list, FullMsgId id) {
	if (!id.peer || !IsServerMsgId(id.msg)) return;
	const auto route = Route(list->controller());
	menu->addAction(tr::lng_purple_archive_history(tr::now), [=] {
		if (route.current()) OpenArchiveHistory(route.host.get(), id);
	}, &st::menuIconViewReplies);
}

}
