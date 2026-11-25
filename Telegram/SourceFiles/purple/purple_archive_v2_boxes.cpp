#include "purple/purple_archive_v2_boxes.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "history/history_item.h"
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
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

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
			&& SessionLease(session) == lease && !session->account().loggingOut() && window->window().maybeSession() == session
			&& !window->window().locked() && !Core::App().screenIsLocked() && !PrivacyScreenActiveAny()
			? session : nullptr;
	}

	void show(Fn<void(not_null<Ui::GenericBox*>)> fill, Ui::LayerOption option = Ui::LayerOption::CloseOther) const {
		if (!current()) return;
		const auto route = *this;
		host->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
			if (const auto live = route.current()) {
				RegisterView(live, box);
				fill(box);
			}
		}), option);
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
bool Read(const Route &route, Work read, Result ready) {
	const auto live = route.current();
	if (live && Query(live, [=](Store &store) {
		if (route.epoch != ViewEpoch() || !route.lease->live.load()) return;
		auto value = read(store);
		crl::on_main([=, value = std::move(value)]() mutable {
			if (route.current()) ready(std::move(value));
		});
	})) return true;
	Failed(route, u"store_busy"_q);
	return false;
}

void Save(const Route &route, MessageIdentity item, int64 sequence, int64 ordinal, QString name) {
	FileDialog::GetWritePath(Core::App().getFileDialogParent(), tr::lng_save_file(tr::now),
		FileDialog::AllFilesFilter(), name, [=](QString &&path) {
			if (path.isEmpty()) return;
			Read(route, [=](Store &store) {
				auto file = QSaveFile(path);
				file.setDirectWriteFallback(false);
				const auto saved = file.open(QIODevice::WriteOnly) && store.readSaved(store.id(),
					item, sequence, ordinal, [&](const QByteArray &part) {
						return route.epoch == ViewEpoch() && file.write(part) == part.size();
					});
				return saved && route.epoch == ViewEpoch() && store.current() && file.commit();
			}, [=](bool ok) {
				if (ok) route.host->showToast(tr::lng_purple_archive_attachment_saved(tr::now));
				else Failed(route, u"copy_incomplete"_q);
			});
		});
}

void History(const Route &route, MessageIdentity item, Ui::LayerOption layer = Ui::LayerOption::CloseOther) {
	const auto load = [=](int64 cursor, Fn<void(std::optional<HistoryPage>)> done) {
		if (!Read(route, [=](Store &store) { return store.history(item, cursor, 12); },
				[=](auto page) { if (!page) Failed(route, u"store_unreadable"_q); done(std::move(page)); })) done({});
	};
	load(0, [=](auto page) {
		if (!page) return;
		route.show([=](not_null<Ui::GenericBox*> box) {
			FillHistoryBox(box, *page, ArchivedText, [=] { return route.current() != nullptr; }, load, {
				.saving = [=](int64 seq, int64 slot) { return Saving(route.current(), item, seq, slot); },
				.openSaved = [=](int64 seq, int64 slot, QString name) { Save(route, item, seq, slot, std::move(name)); },
			});
		}, layer);
	});
}

QString TimelineName(not_null<Main::Session*> session, const TimelineEntry &entry) {
	const auto token = entry.identity.peer;
	const auto peerId = token.kind == PeerTokenKind::User ? peerFromUser(UserId(token.id))
		: token.kind == PeerTokenKind::Chat ? peerFromChat(ChatId(token.id)) : peerFromChannel(ChannelId(token.id));
	const auto chat = session->data().peerLoaded(peerId);
	auto name = chat ? chat->name() : tr::lng_purple_archive_v2_unknown_chat(tr::now);
	if (!entry.latest || (entry.kind != ChatClass::Group && Classify(chat) != ChatClass::Group)) return name;
	const auto value = QJsonDocument::fromJson(entry.latest->body).object().value(u"sender"_q).toString().toULongLong();
	if (value && value != peerId.value) {
		if (const auto sender = session->data().peerLoaded(PeerId(PeerIdHelper(value)))) name += u" · "_q + sender->name();
	}
	return name;
}

void Timeline(const Route &route, std::optional<PeerToken> peer, bool deleted) {
	const auto load = [=](std::optional<TimelineCursor> cursor, Fn<void(std::optional<TimelinePage>)> done) {
		if (!Read(route, [=](Store &store) { return store.timeline(peer, deleted, cursor, 16); },
				[=](auto page) { if (!page) Failed(route, u"store_unreadable"_q); done(std::move(page)); })) done({});
	};
	load({}, [=](auto page) {
		if (!page) return;
		route.show([=](not_null<Ui::GenericBox*> box) {
			FillTimelineBox(box, *page, deleted, ArchivedText, [=] { return route.current() != nullptr; },
				[=](TimelineCursor cursor, Fn<void(std::optional<TimelinePage>)> done) { load(cursor, std::move(done)); },
				[=](const TimelineEntry &entry) { return TimelineName(route.current(), entry); },
				[=](MessageIdentity item) { History(route, item, Ui::LayerOption::KeepOther); });
		});
	});
}

void Confirm(const Route &route, QString title, QString detail, Fn<void()> submit) {
	route.show([=](not_null<Ui::GenericBox*> box) {
		FillArchiveConfirm(box, title, detail, [=] { return route.current() != nullptr; }, submit);
	});
}

void ShowStorage(const Route &route);

void CleanupNext(const Route &route, CleanupTicket ticket, std::optional<int64> before = {}) {
	Read(route, [=](Store &store) {
		const auto size = [&]() -> std::optional<int64> {
			const auto usage = store.usage();
			return usage ? std::optional(usage->physical()) : std::nullopt;
		};
		const auto start = before ? before : size();
		const auto result = store.cleanupPage(ticket);
		const auto after = result.complete ? size() : std::nullopt;
		return std::tuple(result, start, start && after ? *start - *after : 0);
	}, [=](auto outcome) {
		const auto [result, start, reclaimed] = outcome;
		if (!result.effect) Failed(route, result.effect.reason);
		else if (!result.complete) CleanupNext(route, ticket, start);
		else {
			ShowStorage(route);
			route.host->showToast(ArchiveCleanupText(tr::lng_purple_archive_v2_cleanup_done(tr::now), reclaimed));
		}
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
		const auto &summary = value.first;
		const auto &previous = value.second;
		const auto reviewPrevious = [=] {
			if (!previous.review) {
				Failed(route, previous.reason);
				return;
			}
			route.show([=](not_null<Ui::GenericBox*> box) {
				FillPreviousArchiveConfirm(box, label, previous.review->bytes(), [=] { return route.current() != nullptr; },
					[=] {
						Read(route, [=](Store &) { return PreviousArchive::DeletePreviousArchive(options, *previous.review); },
							[=](auto result) {
								if (!result) return Failed(route, result.reason);
								ShowStorage(route);
								route.host->showToast(ArchiveCleanupText(tr::lng_purple_archive_v2_previous_done(tr::now), previous.review->bytes()));
							});
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

void AddArchiveHistoryAction(not_null<Ui::PopupMenu*> menu, not_null<Window::SessionController*> window, FullMsgId id) {
	if (!id.peer || !IsServerMsgId(id.msg)) return;
	const auto route = Route(window);
	menu->addAction(tr::lng_purple_archive_history(tr::now), [=] {
		if (route.current()) OpenArchiveHistory(route.host.get(), id);
	}, &st::menuIconViewReplies);
}

}
