#include "purple/purple_archive_deleted_box.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_archive_history_box.h"
#include "purple/purple_archive_media_box.h"
#include "purple/purple_archive_purge_box.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "styles/style_layers.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include <QtCore/QDateTime>
#include <QtCore/QPointer>

#include <algorithm>

namespace Purple {
namespace {

QString CapturedPeer(const QJsonObject &peer, const QString &field) {
	const auto id = peer.value(field).toString();
	if (id.isEmpty()) {
		return tr::lng_purple_archive_not_captured(tr::now);
	}
	switch (peer.value(u"kind"_q).toInt(-1)) {
	case int(ArchiveChatKind::User): return tr::lng_purple_archive_peer_user(tr::now, lt_id, id);
	case int(ArchiveChatKind::Group): return tr::lng_purple_archive_peer_group(tr::now, lt_id, id);
	case int(ArchiveChatKind::Channel): return tr::lng_purple_archive_peer_channel(tr::now, lt_id, id);
	case int(ArchiveChatKind::Secret): return tr::lng_purple_archive_peer_secret(tr::now, lt_id, id);
	}
	return tr::lng_purple_archive_not_captured(tr::now);
}

QString DeletedPreview(const ArchiveDeletedEntry &entry) {
	const auto status = ArchiveMessageDeletedStatus(entry.message);
	const auto badge = status == ArchiveDeletedStatus::Expired ? tr::lng_purple_archive_expired(tr::now)
		: status == ArchiveDeletedStatus::Consumed ? tr::lng_purple_archive_consumed(tr::now)
		: tr::lng_purple_archive_deleted_badge(tr::now);
	auto result = tr::lng_purple_archive_deleted_observed(tr::now,
		lt_status, badge, lt_date, QDateTime::fromSecsSinceEpoch(entry.message.deletedObservedAt).toString(Qt::ISODate));
	if (!entry.head) {
		return result + u"\n"_q + tr::lng_purple_archive_no_body(tr::now);
	}
	const auto &observation = entry.head->version.observation;
	const auto sender = observation.context.value(u"sender"_q).toObject();
	const auto reply = observation.context.value(u"reply"_q).toObject();
	const auto unknown = tr::lng_purple_archive_not_captured(tr::now);
	result += u"\n"_q + tr::lng_purple_archive_deleted_context(tr::now,
		lt_sender, sender.isEmpty() ? unknown : CapturedPeer(sender, u"id"_q),
		lt_reply, reply.isEmpty() ? unknown : CapturedPeer(reply, u"parent"_q)
			+ '/' + reply.value(u"message"_q).toString(),
		lt_topic, observation.topicId ? QString::number(observation.topicId) : unknown);
	auto preview = *entry.head;
	preview.version.content = entry.displayContent;
	return result + u"\n"_q + ArchiveVersionPreview(preview);
}

QString QueryFailure(ArchiveQueryStatus status) {
	return status == ArchiveQueryStatus::Busy ? tr::lng_purple_archive_query_busy(tr::now)
		: status == ArchiveQueryStatus::Stale ? tr::lng_purple_archive_refresh_required(tr::now)
		: status == ArchiveQueryStatus::ReadOnly ? tr::lng_purple_archive_query_future(tr::now)
		: status == ArchiveQueryStatus::SchemaMismatch ? tr::lng_purple_archive_query_schema(tr::now)
		: tr::lng_purple_archive_query_failed(tr::now);
}

}

void ShowArchiveDeleted(not_null<Window::SessionController*> controller,
		ArchiveViewScope scope, QString name, std::function<bool()> scopeCurrent) {
	const auto session = base::make_weak(&controller->session());
	const auto window = base::make_weak(controller.get());
	const auto source = CachedAppWideSettingsAccount(&controller->session());
	const auto mode = AccountWorkModeForSession(&controller->session());
	const auto account = scope.messages.account;
	if (!scopeCurrent || !scopeCurrent() || account != SessionAccountKey(&controller->session())) {
		return;
	}
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_purple_archive_deleted());
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_archive_deleted_scope(tr::now, lt_account, session.get()->user()->name(), lt_chat, name), st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(box, tr::lng_purple_archive_deleted_notice(tr::now), st::boxLabel));
		if (scope.topic) {
			box->addRow(object_ptr<Ui::FlatLabel>(box,
				tr::lng_purple_archive_topic_filter(tr::now, lt_topic, QString::number(scope.topic->id)), st::boxLabel));
		}
		const auto summary = box->addRow(object_ptr<Ui::FlatLabel>(box, tr::lng_purple_archive_opening(tr::now), st::boxLabel));
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel));
		label->setSelectable(true);
		const auto actions = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		struct State {
			QString view;
			ArchiveStoreEpoch epoch;
			std::shared_ptr<std::atomic<bool>> active;
			std::vector<QPointer<Ui::RoundButton>> actions;
			QPointer<Ui::RoundButton> next;
			base::Timer timer;
			uint64_t ticket = 0;
			crl::time lastUse = 0;
			bool loading = false;
			bool closed = false;
			Fn<void(bool)> load;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto guard = QPointer<Ui::GenericBox>(box);
		const auto current = [=] {
			const auto own = window.get();
			const auto live = session.get();
			return guard && !state->closed && own && live && &own->session() == live
				&& own->window().sessionController() == own && !live->account().loggingOut()
				&& live->account().maybeSession() == live && SessionAccountKey(live) == account
				&& CachedAppWideSettingsAccount(live) == source && AccountWorkModeForSession(live) == mode
				&& scopeCurrent();
		};
		const auto clear = [=] {
			for (const auto &button : state->actions) {
				delete button.data();
			}
			state->actions.clear();
			label->setText(QString());
		};
		const auto release = [=] {
			if (state->active) {
				state->active->store(false);
			}
			if (session && !state->view.isEmpty()) {
				CloseArchiveDeletedView(session.get(), state->view, state->epoch);
			}
			state->active.reset();
			state->view.clear();
		};
		box->boxClosing() | rpl::on_next([=] {
			state->closed = true;
			++state->ticket;
			release();
		}, box->lifetime());
		box->lifetime().add(release);
		rpl::merge(SettingsChanges(), ActiveChanges()) | rpl::on_next([=] {
			if (!current()) {
				box->closeBox();
			}
		}, box->lifetime());
		session.get()->lifetime().add([guard] {
			if (guard) {
				guard->closeBox();
			}
		});
		controller->window().sessionControllerChanges() | rpl::on_next([guard, window](Window::SessionController *now) {
			if (guard && now != window.get()) {
				guard->closeBox();
			}
		}, box->lifetime());
		state->load = [=](bool refresh) {
			if (!current()) {
				if (guard) {
					box->closeBox();
				}
				return;
			}
			if (state->loading && !refresh) {
				return;
			}
			if (refresh) {
				release();
				clear();
			}
			state->loading = true;
			const auto ticket = ++state->ticket;
			const auto expectedView = state->view;
			const auto expectedEpoch = state->epoch;
			const auto done = [=](ArchiveDeletedPage page) {
				if (!current() || ticket != state->ticket) {
					if (page.active) {
						page.active->store(false);
					}
					if (session && !page.view.isEmpty()) {
						CloseArchiveDeletedView(session.get(), page.view, page.epoch);
					}
					return;
				}
				state->loading = false;
				clear();
				if (page.status != ArchiveQueryStatus::Ready || !page.active || !page.active->load()
					|| page.epoch.account != account
					|| (!expectedView.isEmpty() && (page.view != expectedView || page.epoch != expectedEpoch))) {
					release();
					summary->setText(QueryFailure(page.status));
					state->next->setDisabled(true);
					return;
				}
				state->view = page.view;
				state->epoch = page.epoch;
				state->active = page.active;
				state->lastUse = crl::now();
				const auto active = page.active;
				auto totals = tr::lng_purple_archive_deleted_counts(tr::now,
					lt_messages, QString::number(page.counts[0]), lt_deleted, QString::number(page.counts[1]),
					lt_versions, QString::number(page.counts[2]), lt_prior, QString::number(page.counts[3]),
					lt_owned, QString::number(page.counts[5]), lt_total, QString::number(page.counts[4]),
					lt_blobs, QString::number(page.counts[6]));
				totals += u"\n"_q + tr::lng_purple_archive_logical_usage(tr::now, lt_size, QString::number(page.logicalBytes));
				if (!page.logicalComplete) {
					totals += u"\n"_q + tr::lng_purple_archive_usage_pending(tr::now);
				}
				if (page.hasGap) {
					totals += u"\n"_q + tr::lng_purple_archive_gap(tr::now);
				}
				if (page.refreshAvailable) {
					totals += u"\n"_q + tr::lng_purple_archive_refresh_available(tr::now);
				}
				summary->setText(totals);
				auto previews = QStringList();
				for (const auto &entry : page.entries) {
					previews.push_back(DeletedPreview(entry));
					const auto key = entry.message.key;
					const auto history = actions->add(object_ptr<Ui::RoundButton>(actions,
						tr::lng_purple_archive_deleted_history(lt_id, rpl::single(QString::number(key.messageId))), st::defaultBoxButton));
					state->actions.push_back(history);
					history->setClickedCallback([=] {
						if (current() && active->load()) {
							ShowArchiveHistory(window.get(), key);
						}
					});
					if (!entry.head) {
						continue;
					}
					const auto &version = entry.head->version;
					if (!entry.markdownDigest.isEmpty()) {
						const auto request = DesktopArchiveMarkdownRequest{page.epoch, key,
							version.id, entry.markdownDigest, entry.generation};
						const auto button = actions->add(object_ptr<Ui::RoundButton>(actions,
							rpl::single(tr::lng_purple_copy_as_markdown(tr::now)
								+ u" ("_q + QString::number(key.messageId) + ')'), st::defaultBoxButton));
						state->actions.push_back(button);
						button->setClickedCallback([=] {
							const auto origin = [=] {
								return current() && active->load() && !state->loading && state->ticket == ticket;
							};
							if (session) { CopyArchiveVersionAsMarkdown(session.get(), request, origin); }
						});
					}
					auto index = 0;
					for (const auto &ref : version.media) {
						++index;
						if (ref.state != ArchiveMediaState::Complete) {
							continue;
						}
						const auto descriptor = std::find_if(version.content.media.begin(), version.content.media.end(),
							[&](const auto &media) { return media.identity == ref.identity; });
						const auto photo = descriptor != version.content.media.end() && descriptor->kind == u"photo"_q;
						const auto mime = descriptor != version.content.media.end()
							? descriptor->attributes.value(u"mime"_q).toString() : QString();
						const auto request = ArchiveMediaRequest{{page.epoch, key, version.id, version.revision,
							entry.contentHash, ref.identity, entry.generation}, ref.blobId};
						const auto button = actions->add(object_ptr<Ui::RoundButton>(actions,
							tr::lng_purple_archive_deleted_attachment(lt_id, rpl::single(QString::number(key.messageId)),
								lt_index, rpl::single(QString::number(index))), st::defaultBoxButton));
						state->actions.push_back(button);
						button->setClickedCallback([=] {
							const auto origin = [=] { return current() && active->load(); };
							if (origin()) {
								ShowArchiveMedia(window.get(), request, mime, photo, origin);
							}
						});
					}
				}
				label->setText(previews.empty() ? tr::lng_purple_archive_deleted_empty(tr::now)
					: previews.join(u"\n\n"_q));
				state->next->setDisabled(!page.more && page.logicalComplete);
			};
			if (state->view.isEmpty()) {
				OpenArchiveDeletedView(session.get(), scope, ArchiveTranscriptOrder::Chronological, done);
			} else {
				ReadArchiveDeletedView(session.get(), state->view, state->epoch, done);
			}
		};
		state->next = box->addButton(tr::lng_purple_archive_deleted_next(), [=] { state->load(false); });
		box->addButton(tr::lng_purple_archive_refresh(), [=] { state->load(true); });
		for (const auto kind : {ArchivePurgeKind::Trashbin, ArchivePurgeKind::EditHistory}) {
			box->addButton(kind == ArchivePurgeKind::Trashbin ? tr::lng_purple_archive_empty_trash()
				: tr::lng_purple_archive_clear_edits(), [=] {
				if (current()) {
					ShowArchivePurgeBox(window.get(), kind, scope.messages, name, scopeCurrent);
				}
			});
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		state->timer.setCallback([=] {
			if (!current()) {
				box->closeBox();
			} else if (state->active && (!state->active->load() || crl::now() - state->lastUse >= 30000)) {
				++state->ticket;
				state->loading = false;
				release();
				clear();
				summary->setText(tr::lng_purple_archive_refresh_required(tr::now));
				state->next->setDisabled(true);
			}
		});
		state->timer.callEach(500);
		state->load(true);
	}));
}

}
