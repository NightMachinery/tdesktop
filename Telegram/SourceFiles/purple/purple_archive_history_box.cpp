#include "purple/purple_archive_history_box.h"
#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include <QtCore/QDateTime>
#include <QtCore/QPointer>

namespace Purple {
namespace {

QString Preview(const ArchiveVersionSnapshot &snapshot) {
	const auto &version = snapshot.version;
	auto result
		= QDateTime::fromSecsSinceEpoch(version.firstObservedAt).toString(Qt::ISODate)
		  + u"\n"_q;
	result += version.origin == ArchiveOrigin::AuthoritativeEdit
				  ? tr::lng_purple_archive_edit(tr::now)
				  : tr::lng_purple_archive_baseline(tr::now);
	result += u"\n"_q
			  + (version.completeness == ArchiveCompleteness::Complete
					  ? tr::lng_purple_archive_complete(tr::now)
				  : version.completeness == ArchiveCompleteness::Opaque
					  ? tr::lng_purple_archive_opaque(tr::now)
					  : tr::lng_purple_archive_partial(tr::now));
	if (version.missingPreimage) {
		result += u"\n"_q + tr::lng_purple_archive_missing_preimage(tr::now);
	}
	auto body = version.content.text;
	if (!version.content.caption.isEmpty()) {
		body += u"\n"_q + version.content.caption;
	}
	if (version.content.rich.isObject()) {
		const auto rich = version.content.rich.toObject();
		body += u"\n"_q + rich.value(u"title"_q).toString() + u"\n"_q
				+ rich.value(u"description"_q).toString() + u"\n"_q
				+ rich.value(u"text"_q).toString();
		result += u"\n"_q + tr::lng_purple_archive_structure(tr::now);
	}
	if (body.size() > 8192) {
		body = body.left(8192) + u"\n"_q + tr::lng_purple_archive_preview_limit(tr::now);
	}
	result += u"\n\n"_q + body;
	if (!version.media.empty()) {
		const auto complete = std::count_if(version.media.begin(), version.media.end(),
			[](const auto &ref) { return ref.state == ArchiveMediaState::Complete; });
		result += u"\n"_q
				  + tr::lng_purple_archive_media(tr::now, lt_owned,
					  QString::number(complete), lt_total,
					  QString::number(version.media.size()));
	}
	return result;
}

void ShowHistory(not_null<Window::SessionController*> controller, ArchiveMessageKey key) {
	const auto session = base::make_weak(&controller->session());
	const auto window = base::make_weak(controller.get());
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_purple_archive_history());
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_purple_archive_history_notice(tr::now), st::boxLabel));
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_purple_archive_opening(tr::now), st::boxLabel));
		label->setSelectable(true);
		struct State {
			QString before;
			std::optional<uint64_t> revision;
			std::optional<ArchiveStoreEpoch> epoch;
			bool loading = false;
			bool refresh = false;
			QPointer<Ui::RoundButton> next;
			Fn<void()> load;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto guard = QPointer<Ui::GenericBox>(box);
		const auto current = [=] {
			const auto live = session.get();
			const auto own = window.get();
			return guard && live && own && &own->session() == live
				   && own->window().sessionController() == own
				   && SessionAccountKey(live) == key.account;
		};
		session.get()->lifetime().add([guard] {
			if (guard) {
				guard->closeBox();
			}
		});
		controller->window().sessionControllerChanges()
			| rpl::on_next(
				[guard, window](Window::SessionController* now) {
					if (guard && now != window.get()) {
						guard->closeBox();
					}
				},
				box->lifetime());
		state->load = [=] {
			if (!current()) {
				if (guard) {
					box->closeBox();
				}
				return;
			}
			if (state->loading) {
				return;
			}
			if (state->refresh) {
				state->before.clear();
				state->revision.reset();
				state->refresh = false;
			}
			state->loading = true;
			ReadArchiveVersions(session.get(), key, state->before, state->revision,
				[=](DesktopArchiveHistoryPage page) {
					if (!current()) {
						if (guard) {
							box->closeBox();
						}
						return;
					}
					state->loading = false;
					if (page.status != DesktopArchiveStatus::Ready
						|| (state->epoch && *state->epoch != page.epoch)) {
						label->setText(tr::lng_purple_archive_history_changed(tr::now));
						state->refresh = true;
						return;
					}
					state->epoch = page.epoch;
					state->before = page.beforeOrder;
					state->revision = page.revision;
					auto previews = QStringList();
					for (const auto &version : page.versions) {
						previews.push_back(Preview(version));
					}
					const auto body = previews.empty()
										  ? tr::lng_purple_archive_history_empty(tr::now)
										  : previews.join(u"\n\n────────\n\n"_q);
					label->setText(
						(page.hasGap ? tr::lng_purple_archive_gap(tr::now) + u"\n\n"_q
									 : QString())
						+ body);
					if (state->next) {
						state->next->setDisabled(!page.more);
					}
				});
		};
		state->next
			= box->addButton(tr::lng_purple_archive_older(), [=] { state->load(); });
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		state->load();
	}));
}

}

void AddArchiveHistoryAction(not_null<Ui::PopupMenu*> menu,
	not_null<HistoryView::ListWidget*> list, FullMsgId itemId) {
	const auto item = list->session().data().message(itemId);
	if (!item || !item->isRegular() || item->isScheduled() || item->id.bare <= 0) {
		return;
	}
	const auto key = ArchiveMessageKey{SessionAccountKey(&list->session()),
		DesktopArchiveChat(item->history()->peer->id), item->id.bare};
	const auto window = base::make_weak(list->controller().get());
	const auto session = base::make_weak(&list->session());
	menu->addAction(
		tr::lng_purple_archive_history(tr::now),
		[=] {
			const auto own = window.get();
			const auto live = session.get();
			if (own && live && &own->session() == live
				&& SessionAccountKey(live) == key.account) {
				ShowHistory(own, key);
			}
		},
		&st::menuIconViewReplies);
}

}
