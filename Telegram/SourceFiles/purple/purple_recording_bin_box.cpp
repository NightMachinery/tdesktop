/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_bin_box.h"

#include "purple/purple_config.h"
#include "purple/purple_recording_store.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_changes.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "editor/video/video_segment_player.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtGui/QPainterPath>

#include <set>

namespace Purple {
namespace {

using Kind = RecordingRegistry::Kind;
using Mark = RecordingRegistry::Mark;

constexpr auto kRoundPreview = 160;

struct Playing {
	QString id;
	std::unique_ptr<Editor::SegmentPlayer> player;
	rpl::lifetime lifetime;
};

struct BinState {
	Playing playing;
	std::set<std::int64_t> requestedTopics;
	Fn<void()> rebuild;
};

[[nodiscard]] QString MarkText(Mark mark) {
	switch (mark) {
	case Mark::Recovered: return tr::lng_purple_recording_bin_recovered(tr::now);
	case Mark::Partial: return tr::lng_purple_recording_bin_partial(tr::now);
	case Mark::NotSent: return tr::lng_purple_recording_bin_not_sent(tr::now);
	case Mark::Discarded: return tr::lng_purple_recording_bin_discarded(tr::now);
	case Mark::Unrecoverable: return tr::lng_purple_recording_bin_unrecoverable(tr::now);
	case Mark::None: break;
	}
	return QString();
}

[[nodiscard]] History *HistoryOf(const RecordingBinItem &item) {
	const auto session = RecordingAccountSession(item.account);
	const auto peer = (session && item.peer)
		? session->data().peerLoaded(item.peer)
		: nullptr;
	return peer ? session->data().history(peer).get() : nullptr;
}

[[nodiscard]] QString ChatText(const RecordingBinItem &item) {
	const auto history = HistoryOf(item);
	if (!history) {
		return tr::lng_purple_recording_bin_unknown_chat(tr::now);
	} else if (history->purpleHiddenFromView()) {
		return tr::lng_purple_recording_bin_hidden_chat(tr::now);
	}
	return history->peer->name();
}

[[nodiscard]] QString ItemText(const RecordingBinItem &item) {
	auto result = tr::lng_purple_recording_bin_item(
		tr::now,
		lt_chat,
		ChatText(item),
		lt_kind,
		(item.kind == Kind::Round)
			? tr::lng_purple_recording_bin_round(tr::now)
			: tr::lng_purple_recording_bin_voice(tr::now),
		lt_duration,
		Ui::FormatDurationText(item.duration / 1000),
		lt_date,
		langDateTime(base::unixtime::parse(item.started)));
	if (Core::App().domain().accounts().size() > 1) {
		const auto session = RecordingAccountSession(item.account);
		result += u" · "_q + (session
			? session->user()->name()
			: tr::lng_purple_recording_bin_unknown_chat(tr::now));
	}
	const auto mark = MarkText(item.mark);
	return mark.isEmpty() ? result : (result + '\n' + mark);
}

[[nodiscard]] Data::Thread *OriginalThread(
		const RecordingBinItem &item,
		std::set<std::int64_t> *requested = nullptr,
		Fn<void()> topicLoaded = nullptr) {
	const auto history = HistoryOf(item);
	if (!history || !item.topic) {
		return history;
	}
	const auto forum = history->peer->forum();
	if (!forum) {
		return nullptr;
	} else if (const auto topic = forum->topicFor(item.topic)) {
		return topic;
	} else if (requested && RecordingRegistry::FirstTopicRequest(
			*requested,
			item.topic.bare,
			forum->topicDeleted(item.topic))) {
		forum->requestTopic(item.topic, std::move(topicLoaded));
	}
	return nullptr;
}

void Send(
		not_null<Ui::GenericBox*> box,
		const QString &id,
		not_null<Data::Thread*> thread) {
	const auto show = box->uiShow();
	SendRecordingBinItem(id, thread, [=](bool ok) {
		if (!ok) {
			show->showToast(tr::lng_purple_recording_bin_send_failed(tr::now));
		}
	});
}

void Save(not_null<Ui::GenericBox*> box, const RecordingBinItem &item) {
	const auto id = item.id;
	const auto extension = (item.kind == Kind::Round) ? u".mp4"_q : u".ogg"_q;
	const auto name = u"recording-"_q
		+ base::unixtime::parse(item.started).toString(u"yyyy-MM-dd-HHmmss"_q)
		+ extension;
	const auto show = box->uiShow();
	const auto session = RecordingAccountSession(item.account);
	const auto folder = session
		? ::File::DefaultDownloadPath(session)
		: QString();
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		tr::lng_purple_recording_bin_save(tr::now),
		FileDialog::AllFilesFilter(),
		folder + name,
		[=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			const auto bytes = RecordingBinMedia(id);
			auto file = QFile(path);
			const auto ok = bytes
				&& file.open(QIODevice::WriteOnly)
				&& (file.write(*bytes) == bytes->size());
			show->showToast(ok
				? tr::lng_purple_recording_bin_saved(tr::now)
				: tr::lng_purple_recording_bin_save_failed(tr::now));
		});
}

void TogglePlay(
		not_null<Ui::GenericBox*> box,
		not_null<BinState*> state,
		const RecordingBinItem &item) {
	const auto was = state->playing.id;
	state->playing.lifetime.destroy();
	state->playing.player = nullptr;
	state->playing.id = QString();
	if (was == item.id) {
		state->rebuild();
		return;
	}
	const auto bytes = RecordingBinMedia(item.id);
	if (!bytes) {
		box->uiShow()->showToast(tr::lng_purple_recording_bin_play_failed(tr::now));
		return;
	}
	const auto round = (item.kind == Kind::Round);
	auto player = std::make_unique<Editor::SegmentPlayer>(
		QString(),
		*bytes,
		Editor::SegmentPlayerOptions{ .audio = !round });
	player->start();
	if (!player->valid()) {
		box->uiShow()->showToast(tr::lng_purple_recording_bin_play_failed(tr::now));
		return;
	}
	if (round) {
		player->setSound(true);
	}
	state->playing.id = item.id;
	state->playing.player = std::move(player);
	state->rebuild();
}

void AddItem(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<BinState*> state,
		not_null<Window::SessionController*> controller,
		const RecordingBinItem &item) {
	rows->add(
		object_ptr<Ui::FlatLabel>(rows, ItemText(item), st::boxLabel),
		st::boxRowPadding);
	const auto playing = (state->playing.id == item.id);
	const auto canvas = rows->add(
		object_ptr<Ui::RpWidget>(rows),
		st::boxRowPadding);
	if (playing && item.kind == Kind::Round && state->playing.player) {
		canvas->resize(kRoundPreview, kRoundPreview);
		state->playing.player->repaints() | rpl::on_next([=] {
			canvas->update();
		}, canvas->lifetime());
		canvas->paintRequest() | rpl::on_next([=] {
			const auto player = state->playing.player.get();
			if (!player || !player->ready()) {
				return;
			}
			const auto size = QSize(kRoundPreview, kRoundPreview);
			const auto frame = player->frame(size * canvas->devicePixelRatio());
			if (frame.isNull()) {
				return;
			}
			auto p = QPainter(canvas);
			PainterHighQualityEnabler hq(p);
			auto path = QPainterPath();
			path.addEllipse(QRectF(0, 0, kRoundPreview, kRoundPreview));
			p.setClipPath(path);
			p.drawImage(QRect(QPoint(), size), frame);
		}, canvas->lifetime());
	} else {
		canvas->resize(0, 0);
	}
	const auto actions = rows->add(
		object_ptr<Ui::RpWidget>(rows),
		st::boxRowPadding);
	auto buttons = std::vector<not_null<Ui::LinkButton*>>();
	const auto add = [&](const QString &text, Fn<void()> callback) {
		const auto button = Ui::CreateChild<Ui::LinkButton>(
			actions,
			text);
		button->setClickedCallback(std::move(callback));
		buttons.push_back(button);
	};
	const auto id = item.id;
	const auto copy = item;
	if (item.mark != Mark::Unrecoverable) {
		add(playing
			? tr::lng_purple_recording_bin_stop(tr::now)
			: tr::lng_purple_recording_bin_play(tr::now), [=] {
			TogglePlay(box, state, copy);
		});
		if (OriginalThread(item, &state->requestedTopics, crl::guard(box, [=] { state->rebuild(); }))) {
			add(tr::lng_purple_recording_bin_send(tr::now), [=] {
				if (const auto thread = OriginalThread(copy)) {
					Send(box, id, thread);
				}
			});
		}
		const auto own = RecordingAccountSession(item.account);
		const auto session = own ? own : &controller->session();
		if (ActiveSettings().recording.sendElsewhere) {
			add(tr::lng_purple_recording_send_elsewhere(tr::now), [=] {
				box->uiShow()->showBox(Window::PrepareChooseRecipientBox(
					session,
					crl::guard(box, [=](not_null<Data::Thread*> thread) {
						Send(box, id, thread);
						return true;
					}),
					tr::lng_purple_recording_send_elsewhere()));
			});
		}
	}
	add(tr::lng_purple_recording_bin_save(tr::now), [=] {
		Save(box, copy);
	});
	add(tr::lng_box_delete(tr::now), [=] {
		if (state->playing.id == id) {
			state->playing.lifetime.destroy();
			state->playing.player = nullptr;
			state->playing.id = QString();
		}
		DeleteRecordingBinItem(id);
	});
	actions->widthValue() | rpl::on_next([=](int width) {
		auto left = 0;
		auto top = 0;
		auto height = 0;
		for (const auto button : buttons) {
			if (left > 0 && left + button->width() > width) {
				left = 0;
				top += height + st::boxRowPadding.top();
			}
			button->moveToLeft(left, top, width);
			left += button->width() + st::boxRowPadding.left();
			height = std::max(height, button->height());
		}
		actions->resize(width, top + height);
	}, actions->lifetime());
	rows->add(
		object_ptr<Ui::RpWidget>(rows),
		st::boxRowPadding)->resize(0, st::boxRowPadding.bottom());
}

} // namespace

void RecordingBinBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::lng_purple_recording_bin());
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	const auto rows = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto state = box->lifetime().make_state<BinState>();
	state->rebuild = [=] {
		crl::on_main(box, [=] {
			rows->clear();
			const auto items = RecordingBinItems();
			if (items.empty()) {
				rows->add(
					object_ptr<Ui::FlatLabel>(
						rows,
						tr::lng_purple_recording_bin_empty(tr::now),
						st::boxLabel),
					st::boxRowPadding);
			}
			for (const auto &item : items) {
				AddItem(box, rows, state, controller, item);
			}
			rows->resizeToWidth(container->width());
		});
	};
	RecordingBinChanges() | rpl::on_next([=] {
		state->rebuild();
	}, box->lifetime());
	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) {
			session->changes().peerUpdates(
				Data::PeerUpdate::Flag::Name
			) | rpl::on_next([=] {
				state->rebuild();
			}, box->lifetime());
		}
	}
	state->rebuild();
	box->addLeftButton(tr::lng_purple_recording_bin_clear(), [=] {
		if (!RecordingBinCount()) {
			return;
		}
		box->uiShow()->showBox(Ui::MakeConfirmBox({
			.text = tr::lng_purple_recording_bin_clear_sure(),
			.confirmed = [=](Fn<void()> close) {
				state->playing.lifetime.destroy();
				state->playing.player = nullptr;
				state->playing.id = QString();
				ClearRecordingBin();
				close();
			},
			.confirmText = tr::lng_box_delete(),
		}));
	});
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

} // namespace Purple
