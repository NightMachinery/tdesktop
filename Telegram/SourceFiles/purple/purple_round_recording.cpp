#include "purple/hooks/round_recording.h"
#include "purple/hooks/recording.h"

#include "purple/purple_config.h"
#include "purple/purple_round_recording_geometry.h"
#include "base/event_filter.h"
#include "calls/calls_instance.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_calls.h"
#include "ui/controls/round_video_recorder.h"
#include "ui/rp_widget.h"
#include "ui/layers/generic_box.h"

#include <QtCore/QSettings>
#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>
#include <map>

namespace Purple {
namespace {

struct Floating {
	Ui::RoundVideoRecorder *recorder = nullptr;
	HistoryView::Controls::VoiceRecordBar *bar = nullptr;
	Ui::RpWidget *preview = nullptr;
	QSize outer;
	int side = 0;
	bool small = false;
	bool dragging = false;
	QPoint down;
	QPoint position;
	RoundRecordingGeometry geometry;
	Fn<void()> pause;
	std::shared_ptr<ChatHelpers::Show> show;
	QPushButton *pauseButton = nullptr;
	QPushButton *cameraButton = nullptr;
};

std::map<Ui::RpWidget*, std::shared_ptr<Floating>> Floats;

[[nodiscard]] QSize Available(not_null<const Floating*> state) {
    return QSize(state->outer.width(), std::max(0, state->outer.height() - style::ConvertScale(76)));
}

void Place(not_null<Floating*> state) {
    const auto available = Available(state);
	const auto side = std::min({ style::ConvertScale(160), available.width(), available.height() });
	if (!state->small || side <= 0) return;
	state->geometry.place(available.width(), available.height(), side, style::ConvertScale(12));
	state->preview->setGeometry(state->geometry.x, state->geometry.y, side, side);
	if (state->pauseButton) {
		state->pauseButton->setGeometry(0, side - 28, side / 2, 28);
		state->cameraButton->setGeometry(side / 2, side - 28, side / 2, 28);
	}
}

void Toggle(not_null<Floating*> state) {
	const auto small = !state->small;
	if (state->bar && !SetRoundRecordingFloat(state->bar, small)) return;
	state->small = small;
	if (state->small) Place(state);
	else state->preview->setGeometry((state->outer.width() - state->side) / 2,
		(state->outer.height() - state->side) / 2, state->side, state->side);
	if (state->pauseButton) {
		state->pauseButton->setVisible(state->small);
		state->cameraButton->setVisible(state->small);
	}
	state->preview->update();
}

class FloatFilter final : public QObject {
public:
	explicit FloatFilter(std::shared_ptr<Floating> state)
	: QObject(state->preview), _state(std::move(state)) { qApp->installEventFilter(this); }

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (!_state->preview->isVisible() || !ActiveSettings().recording.roundFloat) return false;
		const auto widget = qobject_cast<QWidget*>(object);
		const auto mouse = (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)
			? static_cast<QMouseEvent*>(event) : nullptr;
		if (!widget || !mouse || widget->window() != _state->preview->window()) return false;
		const auto parent = _state->preview->parentWidget();
		if (!parent || (widget != parent && !parent->isAncestorOf(widget))) return false;
		const auto point = _state->preview->mapFromGlobal(mouse->globalPos());
		if (!_state->small && event->type() == QEvent::MouseButtonRelease
			&& mouse->button() == Qt::LeftButton && !_state->preview->rect().contains(point)) {
			Toggle(_state.get());
			return false;
		}
		if (!_state->small || widget != _state->preview) return false;
		if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
			_state->down = mouse->globalPos(); _state->position = _state->preview->pos(); _state->dragging = false;
			return true;
		} else if (event->type() == QEvent::MouseMove && (mouse->buttons() & Qt::LeftButton)) {
			const auto delta = mouse->globalPos() - _state->down;
			_state->dragging |= delta.manhattanLength() > QApplication::startDragDistance();
			_state->geometry.x = _state->position.x() + delta.x(); _state->geometry.y = _state->position.y() + delta.y();
			_state->geometry.clamp(Available(_state.get()).width(), Available(_state.get()).height(), _state->preview->width());
			_state->preview->move(_state->geometry.x, _state->geometry.y);
			return true;
		} else if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
			if (_state->dragging) {
				_state->geometry.snap(Available(_state.get()).width(), Available(_state.get()).height(), _state->preview->width(), style::ConvertScale(12));
				QSettings().setValue(u"purple/recording-corner"_q, _state->geometry.corner);
				Place(_state.get());
			} else Toggle(_state.get());
			return true;
		}
		return false;
	}

private:
	std::shared_ptr<Floating> _state;

};

} // namespace

void BindRoundRecorder(not_null<Ui::RoundVideoRecorder*> recorder,
		not_null<HistoryView::Controls::VoiceRecordBar*> bar,
		Fn<void()> pause, std::shared_ptr<ChatHelpers::Show> show) {
	for (const auto &[preview, state] : Floats) {
		if (state->recorder != recorder) continue;
		state->bar = bar;
		state->pause = std::move(pause);
		state->show = std::move(show);
		state->pauseButton = new QPushButton(tr::lng_purple_recording_toggle(tr::now), preview);
		state->cameraButton = new QPushButton(tr::lng_purple_recording_camera(tr::now), preview);
		state->pauseButton->hide();
		state->cameraButton->hide();
		QObject::connect(state->pauseButton, &QPushButton::clicked, preview, [=] { state->pause(); });
		QObject::connect(state->cameraButton, &QPushButton::clicked, preview, [=] {
			state->show->showBox(::Settings::ChooseCameraDeviceBox(
				rpl::single(Core::App().settings().cameraDeviceId()),
				[](QString id) {
					if (!Core::App().calls().getVideoCapture(id)) return;
				}));
		});
	}
}

bool RoundRecorderFloats(not_null<Ui::RoundVideoRecorder*> recorder,
		not_null<Ui::RpWidget*> preview, QSize outer, int side) {
	auto &owned = Floats[preview.get()];
	if (!owned) {
		owned = std::make_shared<Floating>();
		const auto state = owned;
		state->recorder = recorder;
		state->preview = preview;
		state->geometry.corner = QSettings().value(u"purple/recording-corner"_q, 3).toInt();
		preview->lifetime().add([pointer = preview.get()] { Floats.erase(pointer); });
		new FloatFilter(state);
	}
	owned->outer = outer;
	owned->side = side;
	if (owned->small) Place(owned.get());
	return owned->small;
}

void ExpandRoundRecording(not_null<HistoryView::Controls::VoiceRecordBar*> bar) {
	for (const auto &[preview, state] : Floats) {
		if (state->bar == bar && state->small) { Toggle(state.get()); return; }
	}
}

void PaintRoundRecorder(not_null<Ui::RpWidget*> preview, QPainter &painter) {
	const auto i = Floats.find(preview.get());
	if (i != Floats.end() && i->second->small) painter.setWindow(0, 0, i->second->side, i->second->side);
}

QRect RoundRecorderCanvas(not_null<Ui::RpWidget*> preview) {
	const auto i = Floats.find(preview.get());
	return (i != Floats.end() && i->second->small) ? QRect(0, 0, i->second->side, i->second->side) : preview->rect();
}

} // namespace Purple
