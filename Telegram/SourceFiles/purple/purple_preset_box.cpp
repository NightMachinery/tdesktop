/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_preset_box.h"

#include "base/timer.h"
#include "base/timer_rpl.h"
#include "core/file_utilities.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_engine.h"
#include "purple/purple_gate.h"
#include "purple/purple_peek.h"
#include "purple/purple_schedule.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/qt_object_factory.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

namespace Purple {
namespace {

// How often the schedule line below re-reads the clock. The same period the
// schedule's own tick uses: a line that were more precise than the thing it
// describes would show a boundary that has not happened yet.
constexpr auto kScheduleTick = crl::time(30 * 1000);

// settings.toml holds a hotkey as Qt portable text, because that is what
// QKeySequence parses and what the docs can describe once for every platform.
// What is printed on the keyboard is another matter: on macOS Qt reads "Ctrl"
// as Command, so a label repeating the file would send someone to a key that
// does nothing.
[[nodiscard]] QString HotkeyText(const QString &keys) {
	const auto sequence = QKeySequence(keys, QKeySequence::PortableText);
	return sequence.isEmpty()
		? keys
		: sequence.toString(QKeySequence::NativeText);
}

// What a preset will do, in the words a chat list would use, so the choice can
// be made from the box rather than from memory of what was typed in the file.
[[nodiscard]] QString Summary(const Settings &settings, const QString &name) {
	const auto resolved = Resolve(settings, name);
	if (!resolved) {
		return u"does not resolve"_q;
	}
	auto through = 0;
	auto silenced = 0;
	auto gated = 0;
	for (const auto &list : resolved->lists) {
		if (list.show == ShowMode::Never) {
			continue;
		}
		++through;
		if (!list.notify) {
			++silenced;
		}
		// An entry that said nothing counts as gated too, because the per-kind
		// defaults gate everything except channels - calling it ungated would
		// be the wrong way round for most of a file.
		if (!list.show || ShowModeWatchesUnread(*list.show)) {
			++gated;
		}
	}
	const auto lists = [](int count) {
		return (count == 1) ? u"1 list"_q : u"%1 lists"_q.arg(count);
	};

	// Said the way the model now works: everything is hidden until a list lets
	// it through, so counting what it hides would be counting the whole account.
	auto parts = QStringList{
		through
			? u"lets through "_q + lists(through)
			: u"lets nothing through"_q,
	};
	if (silenced) {
		parts.push_back(u"silences "_q + lists(silenced));
	}
	if (gated) {
		parts.push_back(lists(gated) + u" only when unread"_q);
	}
	const auto all = ranges::any_of(resolved->folders, IsAllFolders);
	parts.push_back(all
		? u"every folder"_q
		: resolved->folders.empty()
		? u"no folders"_q
		: u"%1 folders"_q.arg(resolved->folders.size()));
	if (const auto views = int(resolved->views.size())) {
		parts.push_back((views == 1)
			? u"1 extra view"_q
			: u"%1 extra views"_q.arg(views));
	}
	return parts.join(u", "_q);
}

[[nodiscard]] QString RowText(const Settings &settings, const QString &name) {
	if (name == NormalPreset()) {
		// Normal is a bypass rather than a permissive preset, and saying so
		// here is the difference between "the one that allows everything" and
		// "the one that is not running".
		return u"Normal  -  stock Telegram Desktop"_q;
	}
	// The name the preset's own tab will carry, not the TOML key underneath it.
	// A picker offering "work" above a tab reading "Work" is one thing with two
	// names, and a preset that renamed its tab had no way to say so here at all.
	const auto preset = settings.preset(name);
	// The key beside the preset it presses, in the spelling the keyboard has -
	// a binding nothing displays is a binding nobody remembers, which is the
	// same reason the peek checkbox prints its own.
	const auto key = (preset && !preset->hotkey.isEmpty())
		? u"  (%1)"_q.arg(HotkeyText(preset->hotkey))
		: QString();
	return u"%1%2  -  %3"_q.arg(
		preset ? PresetTitle(*preset) : DefaultViewName(name),
		key,
		Summary(settings, name));
}

[[nodiscard]] QString HotkeyText() {
	return HotkeyText(ActiveSettings().peek.hotkey);
}

// A length as a chip names it. Zero is the position one past the last detent,
// where the core puts "no clock on it at all".
[[nodiscard]] QString ChipText(int seconds) {
	if (!seconds) {
		return u"until I stop"_q;
	} else if (seconds >= 3600 && !(seconds % 3600)) {
		return u"%1 h"_q.arg(seconds / 3600);
	}
	return u"%1 min"_q.arg(seconds / 60);
}

[[nodiscard]] int64 NowSeconds() {
	return QDateTime::currentSecsSinceEpoch();
}

// The dial's track, in the degrees QPainter counts - zero at three o'clock,
// positive counter-clockwise. It starts at half past seven and runs clockwise
// three quarters of the way round, so the gap sits at the bottom. A full circle
// would put the first stop and the last one in the same place, and "1 min" and
// "until I stop" meeting at twelve o'clock is the one confusion a dial of
// lengths cannot afford.
constexpr auto kDialStart = 225.;
constexpr auto kDialSweep = 270.;

// One notch of a mouse wheel, in the eighths of a degree Qt reports for one.
// Trackpads send a stream of much smaller deltas, so they are added up rather
// than rounded away and a flick still moves the same number of stops.
constexpr auto kWheelNotch = 120;

// How long the dial waits, after the wheel stops, before starting the peek it
// is showing. Every peek rebuilds every chat list, and a wheel rolled three
// notches would otherwise do that three times on its way past.
constexpr auto kWheelCommit = crl::time(400);

// The same stops the chips offer, on a track a wheel or a drag can run along.
// The chips stay: a row of words is the discoverable path, and a tick you have
// to find is not an affordance. This is the one that is quick once you know it
// is there - a peek is started mid-sentence, and reaching the far end of an
// hour is one flick here against seven targets to read there.
//
// It never reports an angle, only a stop. The lengths are the core's row, the
// rounding is the core's rounding, and "until I stop" is the position one past
// the last detent - so the track is a single run of stops rather than a track
// plus a checkbox somewhere else.
class PeekDial final : public Ui::RpWidget {
public:
	explicit PeekDial(QWidget *parent);

	// The resting position and the words in the middle, both decided by the
	// engine: the length while nothing is running, the countdown while it is.
	void showSelected(int index, const QString &center);
	void setDisabled(bool disabled);

	[[nodiscard]] rpl::producer<int> commitRequests() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	[[nodiscard]] static int Stops();

	[[nodiscard]] QRect ring() const;
	[[nodiscard]] bool overRing(QPoint point) const;
	[[nodiscard]] int shown() const;
	[[nodiscard]] float64 degreesAt(int index) const;
	[[nodiscard]] std::optional<int> detentAt(QPoint point) const;

	void preview(int index);
	void commit();
	void cancel();
	void refreshTooltip();

	rpl::event_stream<int> _commitRequests;
	base::Timer _wheelCommit;
	QString _center;
	std::optional<int> _pending;
	int _selected = 0;
	int _wheelAccumulated = 0;
	bool _dragging = false;
	bool _disabled = false;

};

int PeekDial::Stops() {
	return int(PeekDetentsSeconds().size()) + 1;
}

PeekDial::PeekDial(QWidget *parent)
: RpWidget(parent) {
	const auto side = st::boxWidth / 4;
	resize(side, side);
	setFocusPolicy(Qt::StrongFocus);
	setCursor(style::cur_pointer);
	_wheelCommit.setCallback([=] { commit(); });
	refreshTooltip();
}

QRect PeekDial::ring() const {
	const auto side = height();
	const auto inset = st::radialLine
		+ st::radialLine / 2
		+ st::lineWidth * 2;
	return QRect((width() - side) / 2, 0, side, side).marginsRemoved(
		QMargins(inset, inset, inset, inset));
}

bool PeekDial::overRing(QPoint point) const {
	const auto inner = ring();
	const auto center = QRectF(inner).center();
	const auto radius = inner.width() / 2.;
	const auto distance = std::hypot(
		point.x() - center.x(),
		point.y() - center.y());
	const auto band = st::radialLine * 2;
	return (distance >= radius - band) && (distance <= radius + band);
}

int PeekDial::shown() const {
	return std::clamp(_pending.value_or(_selected), 0, Stops() - 1);
}

float64 PeekDial::degreesAt(int index) const {
	return kDialStart - (kDialSweep * index) / (Stops() - 1);
}

std::optional<int> PeekDial::detentAt(QPoint point) const {
	const auto inner = ring();
	const auto center = QRectF(inner).center();
	const auto x = point.x() - center.x();
	const auto y = center.y() - point.y();
	if (!x && !y) {
		return std::nullopt;
	}
	auto degrees = std::atan2(y, x) * 180. / M_PI;
	if (degrees < kDialStart - 360.) {
		degrees += 360.;
	}

	// Where along the track the angle falls: 0 at the first stop, 1 at the
	// last, and up to 4/3 in the quarter turn of gap below. That gap is split
	// down the middle rather than clamped to one end, so a point just past the
	// end of the track reads as the end and one just short of the beginning
	// reads as the beginning.
	const auto position = (kDialStart - degrees) / kDialSweep;
	const auto last = Stops() - 1;
	if (position > 1.) {
		const auto half = 1. + (360. - kDialSweep) / (2. * kDialSweep);
		return (position > half) ? 0 : last;
	}

	// Rounded to the nearer stop, with a tie going to the SHORTER one, which
	// is the core's rule: a control that silently rounds a peek up is a control
	// that reveals more than was asked for.
	return std::clamp(int(std::ceil(position * last - 0.5)), 0, last);
}

void PeekDial::preview(int index) {
	const auto clamped = std::clamp(index, 0, Stops() - 1);
	if (_pending && (*_pending == clamped)) {
		return;
	}
	_pending = clamped;
	refreshTooltip();
	update();
}

void PeekDial::commit() {
	_wheelCommit.cancel();
	_wheelAccumulated = 0;
	const auto index = shown();
	_pending = std::nullopt;
	update();
	_commitRequests.fire_copy(index);
}

void PeekDial::cancel() {
	_wheelCommit.cancel();
	_wheelAccumulated = 0;
	if (!_pending) {
		return;
	}
	_pending = std::nullopt;
	refreshTooltip();
	update();
}

void PeekDial::refreshTooltip() {
	const auto seconds = PeekDetentSecondsAt(shown());
	setToolTip(seconds
		? u"Peek for %1"_q.arg(ChipText(seconds))
		: u"Peek until I stop it"_q);
}

void PeekDial::showSelected(int index, const QString &center) {
	const auto clamped = std::clamp(index, 0, Stops() - 1);
	if ((_selected == clamped) && (_center == center)) {
		return;
	}
	_selected = clamped;
	_center = center;
	refreshTooltip();
	update();
}

void PeekDial::setDisabled(bool disabled) {
	if (_disabled == disabled) {
		return;
	}
	_disabled = disabled;
	_dragging = false;
	cancel();
	setCursor(disabled ? style::cur_default : style::cur_pointer);
	update();
}

rpl::producer<int> PeekDial::commitRequests() const {
	return _commitRequests.events();
}

void PeekDial::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto inner = ring();
	const auto center = QRectF(inner).center();
	const auto radius = inner.width() / 2.;
	const auto last = Stops() - 1;
	const auto index = shown();
	const auto line = st::radialLine;
	const auto active = _disabled ? st::windowSubTextFg : st::activeButtonBg;

	auto pen = QPen(st::windowBgOver->c);
	pen.setWidth(line);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawArc(inner, int(kDialStart * 16), int(-kDialSweep * 16));

	pen.setColor(active->c);
	p.setPen(pen);
	p.drawArc(
		inner,
		int(kDialStart * 16),
		int((-kDialSweep * 16 * index) / last));

	const auto point = [&](float64 degrees, float64 distance) {
		const auto radians = degrees * M_PI / 180.;
		return QPointF(
			center.x() + std::cos(radians) * distance,
			center.y() - std::sin(radians) * distance);
	};
	const auto tickFrom = radius + line / 2. + st::lineWidth * 2;
	const auto tickTo = tickFrom + line;
	pen.setColor((_disabled ? st::windowBgOver : st::windowSubTextFg)->c);
	pen.setWidth(st::lineWidth);
	p.setPen(pen);
	for (auto i = 0; i <= last; ++i) {
		const auto degrees = degreesAt(i);
		p.drawLine(point(degrees, tickFrom), point(degrees, tickTo));
	}

	// The stop itself, not only the arc that reaches it: the shortest length is
	// the start of the track, where an arc has no length to be seen by.
	p.setPen(Qt::NoPen);
	p.setBrush(active);
	p.drawEllipse(point(degreesAt(index), radius), line, line);

	const auto text = _pending
		? ChipText(PeekDetentSecondsAt(*_pending))
		: _center;
	const auto side = int((inner.width() - line * 2) / std::sqrt(2.));
	p.setFont(st::normalFont);
	p.setPen(_disabled ? st::windowSubTextFg : st::windowFg);
	p.drawText(
		QRect(
			int(center.x()) - side / 2,
			int(center.y()) - side / 2,
			side,
			side),
		Qt::AlignCenter | Qt::TextWordWrap,
		text);
}

void PeekDial::mousePressEvent(QMouseEvent *e) {
	if (_disabled
		|| (e->button() != Qt::LeftButton)
		|| !overRing(e->pos())) {
		e->ignore();
		return;
	}
	setFocus();
	_dragging = true;
	if (const auto index = detentAt(e->pos())) {
		preview(*index);
	}
}

void PeekDial::mouseMoveEvent(QMouseEvent *e) {
	if (!_dragging) {
		return;
	} else if (const auto index = detentAt(e->pos())) {
		preview(*index);
	}
}

void PeekDial::mouseReleaseEvent(QMouseEvent *e) {
	if (!_dragging) {
		e->ignore();
		return;
	}
	_dragging = false;

	// Released away from the track, the drag is abandoned rather than
	// committed: a dial is dragged by eye, and dragging off it is how a mouse
	// says "not that after all" when there is no other way to take it back.
	if (overRing(e->pos())) {
		commit();
	} else {
		cancel();
	}
}

void PeekDial::wheelEvent(QWheelEvent *e) {
	const auto delta = e->angleDelta().y()
		? e->angleDelta().y()
		: e->angleDelta().x();
	if (_disabled || !delta) {
		e->ignore();
		return;
	}
	_wheelAccumulated += delta;
	const auto steps = _wheelAccumulated / kWheelNotch;
	if (steps) {
		_wheelAccumulated -= steps * kWheelNotch;
		preview(shown() + steps);
		_wheelCommit.callOnce(kWheelCommit);
	}
	e->accept();
}

void PeekDial::keyPressEvent(QKeyEvent *e) {
	const auto key = e->key();
	if (_disabled) {
		e->ignore();
		return;
	} else if (key == Qt::Key_Escape) {
		if (!_pending) {
			e->ignore();
			return;
		}
		cancel();
	} else if ((key == Qt::Key_Up) || (key == Qt::Key_Right)) {
		preview(shown() + 1);
	} else if ((key == Qt::Key_Down) || (key == Qt::Key_Left)) {
		preview(shown() - 1);
	} else if ((key == Qt::Key_Enter)
		|| (key == Qt::Key_Return)
		|| (key == Qt::Key_Space)) {
		commit();
	} else {
		e->ignore();
		return;
	}
	e->accept();
}

// What the preset is doing at this moment, rather than what it says it will do.
// The rows above are read off the file and would look exactly the same if a
// list name were misspelled; this is read off the chat list, and it is the line
// that answers the question everyone actually has - is it working.
[[nodiscard]] QString EffectText(not_null<Main::Session*> session) {
	if (!Filtering()) {
		return QString();
	}
	const auto owner = &session->data();
	const auto total = owner->chatsList()->indexed()->size();
	const auto shown = owner->purpleViewList()->indexed()->size();
	auto result = u"Showing %1 of %2 loaded chats."_q.arg(shown).arg(total);

	// Named rather than numbered, because a count against a tab nobody can
	// identify is not worth the line it costs.
	const auto &views = ExtraViews();
	auto extras = QStringList();
	for (auto i = 0; i != int(views.size()); ++i) {
		extras.push_back(u"%1 %2"_q
			.arg(views[i].name)
			.arg(owner->purpleViewList(i + 1)->indexed()->size()));
	}
	if (!extras.isEmpty()) {
		result += '\n' + extras.join(u", "_q) + '.';
	}
	return result;
}

} // namespace

void PresetBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(rpl::single(u"Work Mode"_q));
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	auto padding = st::boxRowPadding;
	padding.setTop(st::boxOptionListSkip / 2);
	padding.setBottom(st::boxOptionListSkip / 2);

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"A preset decides, for every chat, whether it appears in the "
			"chat list and whether it may interrupt you."_q,
			st::boxLabel),
		st::boxRowPadding);

	const auto rows = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	// Directly under the choice it describes, and wrapped rather than hidden so
	// that switching to Normal takes the space with it instead of leaving a gap
	// where a number used to be.
	const auto effect = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				EffectText(session),
				st::boxDividerLabel),
			padding));
	effect->toggle(Filtering(), anim::type::instant);

	const auto problems = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	// Peek is otherwise a hotkey and nothing else, and a hotkey with no visible
	// affordance is a hotkey nobody remembers. This is where someone would
	// look for it, next to the preset it suspends. Under Normal it says why it
	// does nothing rather than sitting there greyed out with no explanation.
	//
	// A peek also ends on a clock, and nothing anywhere said when. The toast at
	// the start was the only warning, so chats reappearing and then going again
	// two minutes later had no visible cause. The countdown below runs only
	// while this box is open, which is the only time there is anyone to read it.
	const auto peekText = [] {
		const auto &resolved = ActiveResolved();
		if (resolved.normal) {
			return u"Peek - nothing is hidden under Normal"_q;
		} else if (!resolved.peeking) {
			return u"Peek - show what the preset hides (%1)"_q.arg(HotkeyText());
		}
		const auto deadline = CurrentState().peekDeadlineUnix;
		if (!deadline) {
			// auto_off = "off": it runs until it is turned off, and saying so
			// is the whole difference from a countdown that never moves.
			return u"Peeking - until you turn it off"_q;
		}
		const auto left = std::max(
			int(deadline - QDateTime::currentSecsSinceEpoch()),
			0);
		return u"Peeking - %1 left"_q.arg(PeekRemainingText(left));
	};
	const auto peek = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			peekText(),
			Peeking(),
			st::defaultCheckbox),
		padding);
	peek->setDisabled(ActiveResolved().normal);
	peek->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked == Peeking()) {
			return;
		} else if (checked) {
			StartPeekFor(PeekTapSeconds(ActiveSettings()));
		} else {
			EndPeek();
		}
	}, peek->lifetime());

	// Which stop the peek is at: the chip that lights and the place the dial
	// rests are the same number, read once here so the two pictures of one
	// peek cannot come to disagree about it.
	//
	// It follows what is LEFT rather than what was asked for, so a five minute
	// peek with ninety seconds on it sits at two minutes. Nothing anywhere
	// remembers the length a peek was started with - state.toml holds a
	// deadline and that is all.
	const auto litIndex = [] {
		return !Peeking()
			? -1
			: PeekUntilStopped(CurrentState())
			? int(PeekDetentsSeconds().size())
			: PeekDetentIndex(PeekLeftSeconds(CurrentState(), NowSeconds()));
	};

	// The same stops as the chips below, as a track. The chips are the
	// discoverable path and this is the quick one: a peek is started
	// mid-sentence, and the far end of the row is one flick away here.
	const auto dial = container->add(
		object_ptr<PeekDial>(container),
		padding);
	dial->commitRequests(
	) | rpl::on_next([=](int index) {
		StartPeekFor(PeekDetentSecondsAt(index));
	}, dial->lifetime());

	const auto refreshDial = [=] {
		const auto lit = litIndex();

		// With nothing running the dial rests on the length the checkbox would
		// start, so it shows the peek a commit would give rather than an empty
		// track pointing at a number nobody chose.
		const auto index = (lit >= 0)
			? lit
			: PeekDetentIndex(PeekTapSeconds(ActiveSettings()));
		const auto left = (lit >= 0)
			? PeekLeftSeconds(CurrentState(), NowSeconds())
			: 0;
		dial->showSelected(index, left
			? PeekRemainingText(left)
			: ChipText(PeekDetentSecondsAt(index)));
		dial->setDisabled(ActiveResolved().normal);
	};

	// The checkbox is still the on/off, and this row is the same switch with a
	// number on it. One length in the file was never going to be right for
	// both "check one thing" and "the rest of this call", and the lengths come
	// from the core so a phone's chips and this row cannot drift apart.
	//
	// A chip pressed while a peek is running RESTARTS it at that length rather
	// than adding to it. The chip names a length and the peek then has it,
	// which is the only thing a row of lengths can be read as; adding belongs
	// to the hotkey, where there is no number on screen to contradict.
	const auto chips = container->add(
		object_ptr<Ui::RpWidget>(container),
		padding);
	auto lengths = PeekDetentsSeconds();
	lengths.push_back(0);
	auto row = std::vector<Ui::RoundButton*>();
	for (const auto seconds : lengths) {
		const auto chip = Ui::CreateChild<Ui::RoundButton>(
			chips,
			rpl::single(ChipText(seconds)),
			st::defaultTableSmallButton);
		chip->setFullRadius(true);
		chip->setClickedCallback([=] { StartPeekFor(seconds); });
		row.push_back(chip);
	}
	chips->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::normalFont->spacew;
		auto left = 0;
		auto top = 0;
		auto height = 0;
		for (const auto chip : row) {
			if (left && (left + chip->width() > width)) {
				left = 0;
				top += chip->height() + skip;
			}
			chip->moveToLeft(left, top, width);
			left += chip->width() + skip;
			height = top + chip->height();
		}
		chips->resize(width, height);
	}, chips->lifetime());

	const auto refreshChips = [=] {
		const auto normal = ActiveResolved().normal;

		const auto lit = litIndex();
		for (auto i = 0; i != int(row.size()); ++i) {
			const auto on = (i == lit);
			row[i]->setDisabled(normal);
			row[i]->setBrushOverride(on
				? std::make_optional(QBrush(st::activeButtonBg->c))
				: std::nullopt);
			row[i]->setTextFgOverride(on
				? std::make_optional(st::activeButtonFg->c)
				: std::nullopt);
		}
	};

	// Ticks the countdown while one is running, and only then: a timer left
	// running behind a closed box would repaint a label nobody is looking at
	// once a second for as long as the app is up.
	const auto ticker = box->lifetime().make_state<base::Timer>();
	ticker->setCallback([=] {
		peek->setText(peekText());
		refreshChips();
		refreshDial();
	});

	// Follows the engine rather than the click, so a peek started from the
	// hotkey, or ended by its own timer, moves the tick here too. Disabled
	// under Normal, which hides nothing there is anything to peek at.
	const auto refreshPeek = [=] {
		peek->setChecked(
			Peeking(),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
		peek->setDisabled(ActiveResolved().normal);
		peek->setText(peekText());
		refreshChips();
		refreshDial();
		if (Peeking() && CurrentState().peekDeadlineUnix) {
			ticker->callEach(crl::time(1000));
		} else {
			ticker->cancel();
		}
	};
	refreshPeek();

	ActiveChanges(
	) | rpl::on_next([=] {
		refreshPeek();

		// Deferred, because this fires from the gate and the chat lists it is
		// about are rebuilt by another subscriber to the same signal. Counting
		// here would count whatever the previous preset left behind.
		crl::on_main(effect, [=] {
			const auto text = EffectText(session);
			effect->entity()->setText(text);
			effect->toggle(!text.isEmpty(), anim::type::normal);
		});
	}, peek->lifetime());

	// Only worth a row when the file describes a schedule, since a switch that
	// holds off nothing explains nothing. Wrapped rather than hidden so the
	// space goes with it when a reload takes the last rule away.
	const auto paused = container->add(
		object_ptr<Ui::SlideWrap<Ui::Checkbox>>(
			container,
			object_ptr<Ui::Checkbox>(
				container,
				u"Pause the schedule"_q,
				SchedulePaused(),
				st::defaultCheckbox),
			padding));
	paused->toggle(ScheduleConfigured(), anim::type::instant);

	const auto pause = paused->entity();
	pause->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked != SchedulePaused()) {
			SetSchedulePaused(checked);
		}
	}, pause->lifetime());

	// state.toml is still hand-editable and the schedule writes to it too, so
	// the tick follows the file rather than only the click.
	StateChanges(
	) | rpl::on_next([=] {
		pause->setChecked(
			SchedulePaused(),
			Ui::Checkbox::NotifyAboutChange::DontNotify);
	}, pause->lifetime());
	SettingsChanges(
	) | rpl::on_next([=] {
		paused->toggle(ScheduleConfigured(), anim::type::normal);
	}, paused->lifetime());

	// The switch above says the schedule can be paused and nothing said what it
	// would be pausing. The file is the only place the windows were written and
	// working out which one is running from a list of times is exactly the sort
	// of arithmetic a screen should do for you.
	//
	// Two lines rather than one: the first is what is happening, the second is
	// which machine it is happening on. That second question did not exist
	// until one settings.toml could describe several devices, and it has to be
	// answerable from here, because a rule that runs on the phone and not on
	// this laptop otherwise looks like a rule that does not work.
	const auto status = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		padding);
	const auto lines = status->entity();
	const auto schedule = lines->add(
		object_ptr<Ui::FlatLabel>(lines, QString(), st::boxLabel));
	const auto device = lines->add(
		object_ptr<Ui::FlatLabel>(lines, QString(), st::boxDividerLabel));

	const auto refreshSchedule = [=](anim::type animated) {
		const auto text = ScheduleStatusText();
		schedule->setText(text);
		device->setText(ScheduleDeviceText());
		status->toggle(!text.isEmpty(), animated);
	};
	refreshSchedule(anim::type::instant);

	// A clock line goes stale on its own, without anything happening that would
	// fire a signal: seventeen o'clock arrives whether or not the file or the
	// state moved. Thirty seconds is the schedule's own resolution, so this is
	// never more wrong than the schedule itself is, and the timer lives and
	// dies with the box - there is nobody reading the label once it is closed.
	const auto scheduleTicker = box->lifetime().make_state<base::Timer>();
	scheduleTicker->setCallback([=] { refreshSchedule(anim::type::normal); });
	scheduleTicker->callEach(kScheduleTick);

	// The two files rather than ActiveChanges(), which is the resolution moving
	// and is deliberately not fired when a settings change resolves to the same
	// thing. Editing a rule under Normal is exactly that change, and it is one
	// this line has to show.
	rpl::merge(
		SettingsChanges(),
		StateChanges()
	) | rpl::on_next([=] {
		refreshSchedule(anim::type::normal);
	}, status->lifetime());

	// The path was already here, and already the answer to "where do I write
	// one". Clicking it is the part that was missing: everything this box can
	// do beyond choosing is done in that file, and until now finding it meant
	// retyping a path out of a label.
	auto written = TextWithEntities{ u"Presets are written in "_q };
	written.append(Ui::Text::Link(SettingsFilePath()));
	written.append(u", which reloads as you save it."_q);
	const auto path = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(std::move(written)),
			st::boxDividerLabel),
		st::boxRowPadding);
	path->setClickHandlerFilter([=](const auto &...) {
		// Reveal rather than open: the file has no registered handler on most
		// installs, and a TOML file opening in whatever claimed .toml is a
		// worse surprise than a Finder window.
		File::ShowInFolder(SettingsFilePath());
		return false;
	});

	// Indices into the radio group, so the callback can name what was picked.
	const auto names = box->lifetime().make_state<std::vector<QString>>();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>();

	group->setChangedCallback([=](int value) {
		if (value < 0 || value >= int(names->size())) {
			return;
		}
		const auto &name = (*names)[value];
		if (name == CurrentState().activePreset) {
			// Also the guard against re-entry: keeping the selection in step
			// with the state below sets the value that is already active, and
			// this is where that stops.
			return;
		}
		UpdateState([&](State &state) {
			state.activePreset = name;
			state.activeSource = PresetSource::Manual;
		});
	});

	const auto rebuild = [=] {
		rows->clear();
		names->clear();

		const auto &settings = ActiveSettings();
		names->push_back(NormalPreset());
		for (const auto &preset : settings.presets) {
			names->push_back(preset.name);
		}

		const auto active = CurrentState().activePreset;
		auto selected = -1;
		for (auto i = 0; i != int(names->size()); ++i) {
			const auto &name = (*names)[i];
			if (name == active) {
				selected = i;
			}
			const auto row = rows->add(
				object_ptr<Ui::Radiobutton>(
					rows,
					group,
					i,
					RowText(settings, name),
					st::defaultCheckbox),
				padding);

			// A checkbox is one elided line by default, and a summary saying
			// what a preset lets through, silences and gates does not fit in
			// one. Elided, the box was describing every preset as "lets
			// through 3 lists, silences 1 li..." - which is worse than no
			// summary, because it looks like the whole answer.
			row->setAllowTextLines(0);
		}
		if (selected >= 0) {
			group->setValue(selected);
		}

		problems->clear();
		const auto &found = SettingsProblems();
		auto errors = QStringList();
		if (selected < 0) {
			// Nothing is checked, which is the honest picture. Selecting Normal
			// instead would look tidier and would be a disaster: the callback
			// would fire and switch the user to Normal, unhiding every chat the
			// missing preset was hiding, over a typo mid-edit.
			errors.push_back(u"Error: the active preset '%1' is not in this "
				"file. The last resolution that worked is still in "
				"effect."_q.arg(active));
		}
		if (!found.error.isEmpty()) {
			errors.push_back(u"Error: "_q + found.error);
		}
		if (!errors.isEmpty()) {
			const auto label = problems->add(
				object_ptr<Ui::FlatLabel>(
					problems,
					errors.join('\n'),
					st::boxLabel),
				padding);

			// An error and a warning read identically in body text, and the
			// difference is the whole point: one means the file did not load,
			// the other means it loaded with something ignored. The "Error:"
			// prefix stays, so this does not rest on colour alone.
			label->setTextColorOverride(st::attentionButtonFg->c);
		}
		if (!found.warnings.empty()) {
			auto lines = QStringList();
			for (const auto &warning : found.warnings) {
				lines.push_back(u"Warning: "_q + warning);
			}
			problems->add(
				object_ptr<Ui::FlatLabel>(
					problems,
					lines.join('\n'),
					st::boxDividerLabel),
				padding);
		}
	};

	// Only settings.toml can change what the choices are. Rebuilding on a state
	// change instead would destroy the radio button whose click is still on the
	// stack, since choosing a preset is itself a state write.
	SettingsChanges(
	) | rpl::on_next([=] { rebuild(); }, box->lifetime());

	// A schedule or a focus change can move the active preset out from under an
	// open box, so the selection follows the state even though the rows do not.
	StateChanges(
	) | rpl::on_next([=] {
		const auto &active = CurrentState().activePreset;
		for (auto i = 0; i != int(names->size()); ++i) {
			if ((*names)[i] == active) {
				group->setValue(i);
				return;
			}
		}
	}, box->lifetime());

	rebuild();

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

rpl::producer<QString> PresetMenuLabel() {
	// The second source is the peek's own second hand, and it emits only while
	// there is a countdown to move: the menu is open for as long as somebody
	// leaves it open, and a label rewritten once a second for a preset that is
	// not going anywhere would be a timer running for nothing.
	return rpl::merge(
		rpl::single(rpl::empty) | rpl::then(ActiveChanges()),
		base::timer_each(
			crl::time(1000)
		) | rpl::filter([] {
			return Peeking() && CurrentState().peekDeadlineUnix;
		}) | rpl::to_empty
	) | rpl::map([] {
		const auto &resolved = ActiveResolved();
		if (resolved.normal) {
			return u"Work Mode"_q;
		} else if (!resolved.peeking) {
			return u"Work Mode: %1"_q.arg(ViewName());
		}

		// A peek reveals the chats a preset hides, which is the one time the
		// chat list stops matching the preset the label names - and how long
		// that goes on is the question the word "peeking" raises without
		// answering.
		const auto left = PeekLeftSeconds(CurrentState(), NowSeconds());
		return left
			? u"Work Mode: %1 (peeking, %2 left)"_q.arg(
				ViewName(),
				PeekRemainingText(left))
			: u"Work Mode: %1 (peeking)"_q.arg(ViewName());
	});
}

} // namespace Purple
