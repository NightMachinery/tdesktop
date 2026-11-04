#include "purple/hooks/recording.h"

#include "purple/hooks/media_recording_capture.h"
#include "purple/purple_config.h"
#include "purple/purple_recording.h"
#include "purple/purple_recording_dock_model.h"
#include "purple/purple_recording_store.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "core/application.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "editor/video/video_segment_player.h"
#include "history/history_item_helpers.h"
#include "history/view/controls/history_view_voice_record_bar.h"
#include "history/view/history_view_schedule_box.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture.h"
#include "ui/boxes/confirm_box.h"
#include "menu/menu_send.h"
#include "data/data_chat_participant_status.h"
#include "data/data_peer_values.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <map>

namespace Purple {
namespace {

struct Dock {
	RecordingDockModel model;
	std::optional<RecordingContext> context;
	rpl::event_stream<> changes;
	rpl::lifetime capture;
	SendPaymentHelper payment;
	int samples = 0;
	std::array<ushort, 64> levels = {};
	int levelIndex = 0;
	bool once = false;
	bool wantsPreview = false;
    bool previewOpen = false;
	std::optional<Media::Capture::Result> preview;
};

[[nodiscard]] Dock &TheDock() {
	static auto result = Dock();
	return result;
}

struct Binding {
	Fn<std::optional<RecordingContext>()> context;
	Fn<void()> focus;
    std::optional<RecordingContext> active;
};
std::map<HistoryView::Controls::VoiceRecordBar*, Binding> Contexts;

[[nodiscard]] bool Current(std::uint64_t token) {
	return TheDock().model.accepts(token);
}

void Clear() {
	auto &dock = TheDock();
	dock.model.clear();
	dock.capture.destroy();
	dock.context.reset();
	dock.preview.reset();
	dock.wantsPreview = false;
    dock.previewOpen = false;
	dock.payment.clear();
	dock.changes.fire({});
}

void WithWindow(Fn<void(not_null<Window::SessionController*>)> done) {
	const auto &dock = TheDock();
	if (!dock.context) return;
	const auto session = &dock.context->action.history->session();
	const auto peer = dock.context->action.history->peer.get();
	const auto token = dock.model.generation();
	if (const auto window = Core::App().activePrimaryWindow()) {
		window->invokeForSessionController(&session->account(), peer, [=](not_null<Window::SessionController*> controller) {
			if (Current(token)) done(controller);
		});
	}
}

[[nodiscard]] Data::ForumTopic *TopicFor(const Api::SendAction &action) {
	const auto forum = action.history->peer->forum();
	return forum && action.replyTo.topicRootId ? forum->topicFor(action.replyTo.topicRootId) : nullptr;
}

void OpenTarget() {
	WithWindow([](not_null<Window::SessionController*> controller) {
		const auto &action = TheDock().context->action;
		if (action.replyTo.topicRootId) {
			if (const auto topic = TopicFor(action)) {
				controller->showThread(topic);
			}
		} else {
			controller->showPeerHistory(action.history);
		}
	});
}

void ShowPreview();

void Pause() {
	auto &dock = TheDock();
	const auto token = dock.model.generation();
	if (!Current(token)) return;
	const auto pause = !dock.model.paused();
	if (!dock.model.pause(token, pause)) return;
	const auto revision = dock.model.pauseRevision();
    if (!pause) dock.changes.fire({});
	dock.preview.reset();
	Media::Capture::instance()->pause(pause, pause ? Fn<void(Media::Capture::Result&&)>([=](Media::Capture::Result &&result) {
		if (Current(token) && TheDock().model.paused() && TheDock().model.pauseRevision() == revision) {
			TheDock().preview = std::move(result);
			TheDock().changes.fire({});
			if (TheDock().wantsPreview) ShowPreview();
		}
	}) : nullptr);
	dock.changes.fire({});
}

void ShowPreview() {
	auto &dock = TheDock();
	if (!dock.preview || !dock.model.paused() || dock.previewOpen) return;
	dock.wantsPreview = false;
    dock.previewOpen = true;
    const auto token = dock.model.generation();
	const auto snapshot = *dock.preview;
	WithWindow([=](not_null<Window::SessionController*> controller) {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(tr::lng_purple_recording_preview());
            box->lifetime().add([=] {
                if (TheDock().model.generation() == token) TheDock().previewOpen = false;
            });
            TheDock().changes.events() | rpl::filter([=] {
                return !Current(token) || !TheDock().model.paused();
            }) | rpl::on_next([=] { box->closeBox(); }, box->lifetime());
			const auto player = box->lifetime().make_state<Editor::SegmentPlayer>(
				QString(), snapshot.bytes, Editor::SegmentPlayerOptions{ .audio = true });
			player->start();
			const auto wave = box->addRow(object_ptr<Ui::RpWidget>(box));
			wave->resize(320, 60);
			wave->paintRequest() | rpl::on_next([=] {
				auto painter = QPainter(wave);
				painter.setPen(wave->palette().highlight().color());
				const auto count = snapshot.waveform.size();
				for (auto i = 0; i < count; ++i) {
					const auto x = i * wave->width() / count;
					const auto h = std::max(1, int(snapshot.waveform[i]));
					painter.drawLine(x, 30 - h, x, 30 + h);
				}
			}, wave->lifetime());
			box->addButton(tr::lng_purple_recording_play(), [=] { player->setPaused(!player->held()); });
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	});
}

void Preview() {
	TheDock().wantsPreview = true;
	if (TheDock().model.paused()) ShowPreview();
	else Pause();
}

void Discard() {
	const auto token = TheDock().model.generation();
	WithWindow([=](not_null<Window::SessionController*> controller) {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::lng_record_lock_cancel_sure(),
			.confirmed = [=](Fn<void()> close) {
				close();
				if (!Current(token)) return;
				NoteRecordingStop(&TheDock().context->action.history->session(), 0, TheDock().once);
				Media::Capture::instance()->stop();
				Clear();
			},
			.confirmText = tr::lng_record_lock_discard(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
}

void Send(Api::SendOptions options, bool scheduleChosen = false) {
	const auto token = TheDock().model.generation();
	WithWindow([=](not_null<Window::SessionController*> controller) mutable {
		auto &dock = TheDock();
		auto &context = *dock.context;
		const auto peer = context.action.history->peer;
		const auto topic = context.action.replyTo.topicRootId;
		const auto thread = topic ? static_cast<Data::Thread*>(TopicFor(context.action)) : static_cast<Data::Thread*>(context.action.history.get());
		if (!thread
			|| !Data::CanSend(thread, ChatRestriction::SendVoiceMessages)
			|| peer->slowmodeSecondsLeft() > 0) {
			OpenTarget();
			return;
		}
		if (context.scheduled && !scheduleChosen) {
			controller->show(Box(HistoryView::ScheduleBox,
				&controller->session(), nullptr, options, SendMenu::Details(),
				[=](Api::SendOptions chosen) { if (Current(token)) Send(chosen, true); },
				HistoryView::DefaultScheduleTime(), HistoryView::ScheduleBoxStyleArgs()));
			return;
		}
		options.sendAs = context.action.options.sendAs;
		options.suggest = context.action.options.suggest;
		options.ttlSeconds = dock.once ? std::numeric_limits<int>::max() : 0;
		if (!dock.payment.check(controller, peer, options, 1, [=](int approved) mutable {
			if (Current(token)) { options.starsApproved = approved; Send(options, scheduleChosen); }
		})) return;
		auto action = context.action;
		action.options = options;
		action.clearDraft = false;
		if (!dock.model.finish(token)) return;
		NoteRecordingStop(&action.history->session(), 1, dock.once);
		Media::Capture::instance()->stop([=](Media::Capture::Result &&result) {
			auto &dock = TheDock();
			if (dock.model.generation() != token || !dock.model.finishing()) return;
			if (!result.bytes.isEmpty()) {
				action.history->session().api().sendVoiceMessage(
					std::move(result.bytes), std::move(result.waveform), result.duration, false, action);
			}
			Clear();
		});
		dock.changes.fire({});
	});
}

class Strip final : public Ui::RpWidget {
public:
	Strip(not_null<Ui::RpWidget*> parent, Fn<void()> relayout)
	: RpWidget(parent), _title(this), _pause(this), _preview(this), _send(this), _discard(this) {
		setObjectName(u"purple-recording-strip"_q);
		_pause.setText(tr::lng_purple_recording_pause(tr::now));
		_preview.setText(tr::lng_purple_recording_preview(tr::now));
		_send.setText(tr::lng_send_button(tr::now));
		_discard.setText(tr::lng_record_lock_discard(tr::now));
		QObject::connect(&_pause, &QPushButton::clicked, this, [] { Pause(); });
		QObject::connect(&_send, &QPushButton::clicked, this, [] { Send({}); });
		QObject::connect(&_discard, &QPushButton::clicked, this, [] { Discard(); });
		QObject::connect(&_preview, &QPushButton::clicked, this, [] { Preview(); });
		_title.installEventFilter(this);
		TheDock().changes.events() | rpl::on_next([=] {
			const auto visible = TheDock().model.active();
			const auto changed = visible != !isHidden();
			setVisible(visible);
			refresh();
			if (changed) relayout();
		}, lifetime());
		SettingsChanges() | rpl::on_next([=] { refresh(); }, lifetime());
		paintRequest() | rpl::on_next([=] {
			auto painter = QPainter(this);
			painter.fillRect(rect(), palette().window());
			const auto &dock = TheDock();
			painter.setPen(Qt::red);
			for (auto i = 0; i != 64; ++i) {
				const auto level = dock.levels[(dock.levelIndex + i) % 64];
				const auto h = std::clamp(int(level / 2048), 1, 12);
				painter.drawLine(width() / 3 + i * 2, 20 - h, width() / 3 + i * 2, 20 + h);
			}
		}, lifetime());
		refresh();
		setVisible(TheDock().model.active());
	}

	void refresh() {
		const auto &dock = TheDock();
		if (!dock.context) return;
		const auto history = dock.context->action.history;
		const auto seconds = dock.samples / 48000;
		_title.setText((history->purpleHiddenFromView() ? tr::lng_purple_recording(tr::now) : history->peer->name())
			+ u"  %1:%2"_q.arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0')));
		_pause.setText(dock.model.paused() ? tr::lng_purple_recording_resume(tr::now) : tr::lng_purple_recording_pause(tr::now));
		for (const auto button : { &_pause, &_preview, &_send, &_discard }) button->setEnabled(!dock.model.finishing());
		update();
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (object == &_title && event->type() == QEvent::MouseButtonRelease) { OpenTarget(); return true; }
		return RpWidget::eventFilter(object, event);
	}
	void resizeEvent(QResizeEvent *event) override {
		RpWidget::resizeEvent(event);
		const auto w = std::max(44, width() / 8);
		_title.setGeometry(8, 0, std::max(0, width() - 4 * w - 8), height());
		auto x = width() - 4 * w;
		for (const auto button : { &_pause, &_preview, &_send, &_discard }) { button->setGeometry(x, 0, w, height()); x += w; }
	}

private:
	QLabel _title;
	QPushButton _pause, _preview, _send, _discard;

};

[[nodiscard]] Strip *FindStrip(not_null<const Ui::RpWidget*> parent) {
	return static_cast<Strip*>(parent->findChild<Ui::RpWidget*>(u"purple-recording-strip"_q, Qt::FindDirectChildrenOnly));
}

class BarKeyFilter final : public QObject {
public:
	explicit BarKeyFilter(not_null<HistoryView::Controls::VoiceRecordBar*> bar)
	: QObject(bar), _bar(bar) { qApp->installEventFilter(this); }

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		const auto widget = qobject_cast<QWidget*>(object);
		if (event->type() != QEvent::KeyPress || !_bar->isRecordingLocked()
			|| !widget || widget->window() != _bar->window()) return false;
		const auto key = static_cast<QKeyEvent*>(event);
		if (key->text().isEmpty() || !key->text().front().isPrint()
			|| (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
		if (!TryDockRecording(_bar)) return false;
		if (const auto field = QApplication::focusWidget()) {
			QCoreApplication::postEvent(field, new QKeyEvent(QEvent::KeyPress,
				key->key(), key->modifiers(), key->text(), key->isAutoRepeat(), key->count()));
		}
		return true;
	}

private:
	not_null<HistoryView::Controls::VoiceRecordBar*> _bar;

};

} // namespace

std::optional<RecordingContext> RecordingHistoryContext(
		History *history, Fn<Api::SendAction()> action) {
	return history ? std::make_optional(RecordingContext{ action() }) : std::nullopt;
}

std::optional<RecordingContext> RecordingComposeContext(
		History *history, FullReplyTo reply, PeerData *sendAs,
		BusinessShortcutId shortcut, bool scheduled) {
	if (!history || shortcut) return std::nullopt;
	auto action = Api::SendAction(history);
	action.replyTo = reply;
	action.options.sendAs = sendAs;
	return RecordingContext{ std::move(action), scheduled };
}

void BindRecordingBar(
		not_null<HistoryView::Controls::VoiceRecordBar*> bar,
		Fn<std::optional<RecordingContext>()> context, Fn<void()> focus) {
	Contexts.emplace(bar.get(), Binding{ std::move(context), std::move(focus) });
	new BarKeyFilter(bar);
	bar->lifetime().add([pointer = bar.get()] { Contexts.erase(pointer); });
	auto filter = base::install_event_filter(bar, [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::MouseButtonRelease && bar->isRecordingLocked()) {
			const auto mouse = static_cast<QMouseEvent*>(event.get());
			if (mouse->pos().x() > bar->width() / 3 && mouse->pos().x() < bar->width() - 100) {
				const auto adopted = TryDockRecording(bar);
				if (adopted) return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});
	bar->lifetime().make_state<base::unique_qptr<QObject>>(std::move(filter));
}

bool TryDockRecording(not_null<HistoryView::Controls::VoiceRecordBar*> bar) {
	auto &dock = TheDock();
	const auto i = Contexts.find(bar.get());
	if (!bar->purpleDockable() || i == Contexts.end()
		|| !CanDockVoice(ActiveSettings().recording, false, false, false)
		|| dock.model.active()) return false;
	auto context = i->second.active;
	if (!context || (context->action.replyTo.topicRootId
		&& !TopicFor(context->action))) return false;
	if (!dock.model.begin()) return false;
	dock.context = std::move(context);
	dock.samples = 0;
	dock.levels.fill(0);
	dock.once = bar->purpleReleaseToDock();
	i->second.focus();
	const auto token = dock.model.generation();
	const auto session = &dock.context->action.history->session();
	session->lifetime().add([=] {
		if (TheDock().model.generation() == token) {
			if (const auto capture = Media::Capture::instance()) capture->stop();
			Clear();
		}
	});
	Media::Capture::instance()->updated() | rpl::on_next_error([=](const Media::Capture::Update &update) {
		if (!Current(token)) return;
		auto &dock = TheDock();
		dock.samples = update.samples;
		dock.levels[dock.levelIndex++ % 64] = update.level;
		dock.changes.fire({});
	}, [=] {
		if (Current(token)) {
			NoteRecordingStop(session, 0, TheDock().once);
			Media::Capture::instance()->stop();
			Clear();
		}
	}, dock.capture);
	dock.changes.fire({});
	return true;
}

bool PreventRecordingStart(not_null<HistoryView::Controls::VoiceRecordBar*> bar) {
	if (TheDock().model.active()) { Discard(); return true; }
	SetRecordingCaptureTarget(std::nullopt);
	if (const auto i = Contexts.find(bar.get()); i != Contexts.end()) {
        if (!bar->isListenState() || !i->second.active) i->second.active = i->second.context();
		if (const auto &context = i->second.active) {
			const auto &action = context->action;
			const auto session = &action.history->session();
			SetRecordingCaptureTarget(RecordingCaptureTarget{
				.account = RecordingAccountIndex(session),
				.user = session->userId().bare,
				.peer = action.history->peer->id.value,
				.topic = action.replyTo.topicRootId.bare,
				.reply = action.replyTo.messageId.msg.bare,
			});
		}
	}
	return false;
}

void AttachRecordingStrip(not_null<Ui::RpWidget*> parent, Fn<void()> relayout) {
	new Strip(parent, std::move(relayout));
}

int RecordingStripHeight(not_null<const Ui::RpWidget*> parent) {
	const auto strip = FindStrip(parent);
	return strip && !strip->isHidden() ? style::ConvertScale(40) : 0;
}

void PlaceRecordingStrip(not_null<Ui::RpWidget*> parent, int left, int top, int width) {
	if (const auto strip = FindStrip(parent)) {
		strip->setGeometry(left, top, width, style::ConvertScale(40));
		strip->raise();
	}
}

} // namespace Purple
