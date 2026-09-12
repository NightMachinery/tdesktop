/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_screentime_box.h"

#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_schedule.h"
#include "purple/purple_screentime.h"
#include "purple/purple_screentime_recorder.h"
#include "settings/settings_common.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_peer_menu.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QTimeZone>

namespace Purple {
namespace {

// The five kinds, in the order the stack draws them and the legend lists them.
// Elsewhere last because it is the leftover rather than a kind of chat, and a
// stack reads better with the leftover at the top.
constexpr auto kKinds = std::array{
	ScreenTimeKind::Private,
	ScreenTimeKind::Group,
	ScreenTimeKind::Channel,
	ScreenTimeKind::Bot,
	ScreenTimeKind::Elsewhere,
};

constexpr auto kChartHeight = 120;
constexpr auto kChartTopSkip = 8;
constexpr auto kHeatCellMin = 6;
constexpr auto kRowHeight = 44;
constexpr auto kUserpicSize = 32;

// How many chats the ranked list shows before it stops. Long enough to hold a
// day's real conversations and short enough that the box is still a page.
constexpr auto kTopChats = 20;

enum class Period : uchar {
	Today,
	Week,
	Month,
	Custom,
};

struct Range {
	int64 fromMs = 0;
	int64 toMs = 0;
};

struct View {
	Period period = Period::Today;

	// Only read for Period::Custom, and kept across a switch away and back so
	// a range typed once does not have to be typed again.
	QDate customFrom;
	QDate customTo;

	// Which metric everything on screen is: the whole session, or only the
	// part of it the actions claimed. A filter rather than a second chart,
	// because "how much of this was me talking" is the same picture with a
	// different number in it.
	bool activeOnly = false;

	// One per kKinds entry. All on is the usual case and the only one that
	// adds up to foreground time.
	std::array<bool, 5> kinds = { true, true, true, true, true };

	// Empty is every preset. A name here is the file's spelling of it, and the
	// empty preset - Normal - is offered as its own choice.
	QString preset;
	bool presetFilter = false;

	std::vector<Event> events;
	std::vector<Session> sessions;
};

[[nodiscard]] const ScreenTime &Config() {
	return ActiveSettings().screenTime;
}

[[nodiscard]] QTimeZone Zone() {
	return QTimeZone::systemTimeZone();
}

[[nodiscard]] int KindIndex(ScreenTimeKind kind) {
	for (auto i = 0; i != int(kKinds.size()); ++i) {
		if (kKinds[i] == kind) {
			return i;
		}
	}
	return int(kKinds.size()) - 1;
}

[[nodiscard]] QString KindText(ScreenTimeKind kind) {
	switch (kind) {
	case ScreenTimeKind::Private: return u"People"_q;
	case ScreenTimeKind::Group: return u"Groups"_q;
	case ScreenTimeKind::Channel: return u"Channels"_q;
	case ScreenTimeKind::Bot: return u"Bots"_q;
	default: break;
	}
	return u"Elsewhere"_q;
}

// Palette entries rather than literals, so a theme that repaints the app
// repaints the chart with it. The userpic colours are the one set in the
// palette that is meant to be told apart at a glance.
[[nodiscard]] style::color KindColor(ScreenTimeKind kind) {
	switch (kind) {
	case ScreenTimeKind::Private: return st::historyPeer4UserpicBg;
	case ScreenTimeKind::Group: return st::historyPeer2UserpicBg;
	case ScreenTimeKind::Channel: return st::historyPeer8UserpicBg;
	case ScreenTimeKind::Bot: return st::historyPeer5UserpicBg;
	default: break;
	}
	return st::windowSubTextFg;
}

[[nodiscard]] QString ShareText(int64 part, int64 whole) {
	if (whole <= 0) {
		return QString();
	}
	return u"%1 %"_q.arg((part * 100 + whole / 2) / whole);
}

// How much peek there was, in a sentence. A different question from the line
// above it on the screen: that one is time spent IN the chats the preset hides,
// and a peek started to look at the chat list itself - which most are - leaves
// no mark on it at all. Peek is the way out of the preset, so how often it is
// taken is how much of the period the preset was not being kept to.
[[nodiscard]] QString PeekedText(const PeekUsage &peeked) {
	return !peeked.count
		? u"Peeked: not once in this period."_q
		: (peeked.count == 1)
		? u"Peeked: once, %1."_q.arg(FormatSpan(peeked.totalMs))
		: u"Peeked: %1 times, %2."_q
			.arg(peeked.count)
			.arg(FormatSpan(peeked.totalMs));
}

[[nodiscard]] int64 MsAt(const QDate &date, const QTimeZone &zone) {
	return QDateTime(date, QTime(0, 0), zone).toMSecsSinceEpoch();
}

[[nodiscard]] Range RangeFor(const View &state) {
	const auto zone = Zone();
	const auto today = QDate::currentDate();
	switch (state.period) {
	case Period::Today:
		return { MsAt(today, zone), MsAt(today.addDays(1), zone) };
	case Period::Week: {
		// Monday to Monday, the same week the heat map's rows are numbered
		// by and the same one the schedule's `days' counts in.
		const auto start = today.addDays(1 - today.dayOfWeek());
		return { MsAt(start, zone), MsAt(start.addDays(7), zone) };
	}
	case Period::Month: {
		const auto start = QDate(today.year(), today.month(), 1);
		return { MsAt(start, zone), MsAt(start.addMonths(1), zone) };
	}
	default: break;
	}
	const auto from = state.customFrom.isValid() ? state.customFrom : today;
	const auto till = state.customTo.isValid() ? state.customTo : today;
	// Inclusive of the last day, because a range picked as "the 1st to the
	// 7th" that stopped at midnight on the 7th would be missing a day nobody
	// asked it to leave out.
	return { MsAt(from, zone), MsAt(till.addDays(1), zone) };
}

[[nodiscard]] BucketUnit UnitFor(Period period) {
	// Hours for a day, days for anything longer. A month of days is thirty-one
	// bars, which is still a row you can read; a month of weeks would be four,
	// which says nothing about when.
	return (period == Period::Today) ? BucketUnit::HourOfDay : BucketUnit::Day;
}

[[nodiscard]] QString PeriodText(const View &state) {
	switch (state.period) {
	case Period::Today: return u"Today"_q;
	case Period::Week: return u"This week"_q;
	case Period::Month: return u"This month"_q;
	default: break;
	}
	const auto locale = QLocale();
	const auto from = state.customFrom.isValid()
		? state.customFrom
		: QDate::currentDate();
	const auto till = state.customTo.isValid()
		? state.customTo
		: QDate::currentDate();
	return (from == till)
		? locale.toString(from, QLocale::ShortFormat)
		: u"%1 - %2"_q.arg(
			locale.toString(from, QLocale::ShortFormat),
			locale.toString(till, QLocale::ShortFormat));
}

// The sessions the filters let through. The metric switch is not here: "active
// only" changes which of a session's two clocks is read, not which sessions
// exist, and dropping the reading ones would take their chats off the list
// along with their time.
[[nodiscard]] std::vector<Session> Filtered(const View &state) {
	auto result = std::vector<Session>();
	result.reserve(state.sessions.size());
	for (const auto &session : state.sessions) {
		if (!state.kinds[KindIndex(session.chatKind)]) {
			continue;
		} else if (state.presetFilter && session.preset != state.preset) {
			continue;
		}
		result.push_back(session);
	}
	return result;
}

[[nodiscard]] std::vector<Session> OfKind(
		const std::vector<Session> &sessions,
		ScreenTimeKind kind) {
	auto result = std::vector<Session>();
	for (const auto &session : sessions) {
		if (session.chatKind == kind) {
			result.push_back(session);
		}
	}
	return result;
}

[[nodiscard]] int64 Metric(const Slice &slice, bool activeOnly) {
	return activeOnly ? slice.activeMs : slice.totalMs;
}

[[nodiscard]] int64 Metric(const Bucket &bucket, bool activeOnly) {
	return activeOnly ? bucket.activeMs : bucket.totalMs;
}

[[nodiscard]] int64 Metric(const Totals &totals, bool activeOnly) {
	return activeOnly ? totals.activeMs : totals.totalMs;
}

[[nodiscard]] PeerData *PeerFor(
		not_null<Main::Session*> session,
		PeerIdValue id) {
	const auto owner = &session->data();
	const auto bare = BareId(id);
	// The log keeps the bare id, which is all IdOf() ever wrote, so the type
	// is guessed back the same way the settings comments guess it. A miss just
	// means a row named by its id, which is worse to read and true either way.
	if (const auto user = owner->peerLoaded(peerFromUser(bare))) {
		return user;
	} else if (const auto chat = owner->peerLoaded(peerFromChat(bare))) {
		return chat;
	}
	return owner->peerLoaded(peerFromChannel(bare));
}

[[nodiscard]] QString ChatText(
		not_null<Main::Session*> session,
		PeerIdValue id) {
	if (!id) {
		return u"Elsewhere"_q;
	} else if (const auto peer = PeerFor(session, id)) {
		return peer->name();
	}
	return u"Chat %1"_q.arg(id);
}

// One row of bars, stacked by kind. Painted rather than assembled out of
// widgets because it is one picture that has to line up with itself, and a
// column per bar would be a hundred widgets for a month.
class Chart final : public Ui::RpWidget {
public:
	struct Column {
		QString label;
		std::array<int64, 5> byKind = {};
		int64 total = 0;
	};

	Chart(QWidget *parent, std::vector<Column> columns);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	std::vector<Column> _columns;
	int64 _max = 0;

};

Chart::Chart(QWidget *parent, std::vector<Column> columns)
: RpWidget(parent)
, _columns(std::move(columns)) {
	for (const auto &column : _columns) {
		_max = std::max(_max, column.total);
	}
	resize(width(), kChartHeight + st::normalFont->height + kChartTopSkip);
}

void Chart::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_columns.empty() || !_max) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(rect(), Qt::AlignCenter, u"Nothing recorded yet."_q);
		return;
	}
	const auto count = int(_columns.size());
	const auto full = width();
	const auto step = full / float64(count);
	const auto barWidth = std::max(int(step) - 2, 1);
	const auto labelTop = kChartHeight + kChartTopSkip;

	// Every fourth label for a month, every third for a week of hours: a label
	// under every bar would overlap itself, and the bars are the picture.
	const auto labelEvery = std::max(
		1,
		(count * st::normalFont->width(u"00-00"_q) + full - 1) / std::max(full, 1));

	p.setFont(st::normalFont);
	for (auto i = 0; i != count; ++i) {
		const auto &column = _columns[i];
		const auto left = int(i * step);
		auto bottom = kChartHeight;
		for (auto k = 0; k != int(kKinds.size()); ++k) {
			const auto value = column.byKind[k];
			if (value <= 0) {
				continue;
			}
			// At least one pixel, so a kind that is present but tiny is not
			// silently missing from a stack that claims to be the whole.
			const auto height = std::max(
				int((value * kChartHeight) / _max),
				1);
			p.fillRect(
				QRect(left, bottom - height, barWidth, height),
				KindColor(kKinds[k]));
			bottom -= height;
		}
		if (!(i % labelEvery)) {
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(left, labelTop, barWidth * labelEvery, st::normalFont->height),
				Qt::AlignLeft | Qt::AlignVCenter,
				column.label);
		}
	}
}

// Hour by weekday, for the month view: seven rows of twenty-four cells, each
// shaded by how much of the period's busiest hour it holds.
class Heat final : public Ui::RpWidget {
public:
	Heat(QWidget *parent, HeatMap map, bool activeOnly);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	HeatMap _map;
	bool _activeOnly = false;
	int64 _max = 0;

};

Heat::Heat(QWidget *parent, HeatMap map, bool activeOnly)
: RpWidget(parent)
, _map(std::move(map))
, _activeOnly(activeOnly) {
	for (auto day = 1; day != 8; ++day) {
		for (auto hour = 0; hour != 24; ++hour) {
			_max = std::max(
				_max,
				_activeOnly ? _map.active(day, hour) : _map.total(day, hour));
		}
	}
	resize(width(), 7 * (kHeatCellMin + 2) + st::normalFont->height);
}

void Heat::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto locale = QLocale();
	const auto labelWidth = st::normalFont->width(u"Wed"_q)
		+ st::normalFont->spacew;
	const auto cell = std::max((width() - labelWidth) / 24, kHeatCellMin);
	const auto rowHeight = std::max(
		(height() - st::normalFont->height) / 7,
		kHeatCellMin);
	p.setFont(st::normalFont);
	for (auto day = 1; day != 8; ++day) {
		const auto top = (day - 1) * rowHeight;
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(0, top, labelWidth, rowHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			locale.dayName(day, QLocale::ShortFormat));
		for (auto hour = 0; hour != 24; ++hour) {
			const auto value = _activeOnly
				? _map.active(day, hour)
				: _map.total(day, hour);
			const auto rect = QRect(
				labelWidth + hour * cell,
				top + 1,
				cell - 1,
				rowHeight - 2);
			if (!_max || value <= 0) {
				p.fillRect(rect, st::windowBgOver);
				continue;
			}
			// Alpha rather than a second colour: one hue with a range of
			// weights reads as "more and less of the same thing", which is
			// what an hour of a heat map is.
			auto colour = st::windowBgActive->c;
			colour.setAlphaF(0.15 + 0.85 * (value / float64(_max)));
			p.fillRect(rect, colour);
		}
	}
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(
			labelWidth,
			7 * rowHeight,
			width() - labelWidth,
			st::normalFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		u"00:00 to 24:00"_q);
}

// One chat in the ranked list: userpic, name, the share it took as a bar, and
// its two numbers on the right.
class ChatRow final : public Ui::RippleButton {
public:
	ChatRow(
		QWidget *parent,
		PeerData *peer,
		QString name,
		ScreenTimeKind kind,
		int64 value,
		int64 top,
		QString right);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	PeerData *_peer = nullptr;
	Ui::PeerUserpicView _userpic;
	QString _name;
	ScreenTimeKind _kind = ScreenTimeKind::Elsewhere;
	int64 _value = 0;
	int64 _top = 0;
	QString _right;

};

ChatRow::ChatRow(
	QWidget *parent,
	PeerData *peer,
	QString name,
	ScreenTimeKind kind,
	int64 value,
	int64 top,
	QString right)
: RippleButton(parent, st::defaultRippleAnimation)
, _peer(peer)
, _name(std::move(name))
, _kind(kind)
, _value(value)
, _top(top)
, _right(std::move(right)) {
	resize(width(), kRowHeight);
}

void ChatRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	paintRipple(p, 0, 0);

	const auto left = st::boxRowPadding.left();
	const auto right = st::boxRowPadding.right();
	const auto userpicTop = (height() - kUserpicSize) / 2;
	if (_peer) {
		_peer->paintUserpicLeft(
			p,
			_userpic,
			left,
			userpicTop,
			width(),
			kUserpicSize);
	} else {
		// Elsewhere, and every chat this account has never loaded. A filled
		// circle in the kind's colour rather than a blank, so the row still
		// lines up with the ones around it.
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(KindColor(_kind));
		p.drawEllipse(QRect(left, userpicTop, kUserpicSize, kUserpicSize));
	}

	const auto textLeft = left + kUserpicSize + st::normalFont->spacew * 2;
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	const auto rightWidth = st::normalFont->width(_right);
	p.drawText(
		QRect(
			width() - right - rightWidth,
			0,
			rightWidth,
			height() / 2 + st::normalFont->height / 2),
		Qt::AlignRight | Qt::AlignBottom,
		_right);

	const auto textWidth = width()
		- right
		- rightWidth
		- textLeft
		- st::normalFont->spacew;
	p.setPen(st::windowFg);
	p.drawText(
		QRect(
			textLeft,
			height() / 2 - st::normalFont->height,
			textWidth,
			st::normalFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_name, std::max(textWidth, 1)));

	const auto barWidth = width() - right - textLeft;
	const auto filled = _top
		? std::max(int((_value * barWidth) / _top), 2)
		: 0;
	const auto barTop = height() / 2 + st::normalFont->height / 2;
	p.fillRect(
		QRect(textLeft, barTop, barWidth, st::lineWidth * 3),
		st::windowBgOver);
	if (filled > 0) {
		p.fillRect(
			QRect(textLeft, barTop, filled, st::lineWidth * 3),
			KindColor(_kind));
	}
}

[[nodiscard]] std::vector<Chart::Column> Columns(
		const std::vector<Session> &sessions,
		const Range &range,
		Period period,
		bool activeOnly) {
	const auto unit = UnitFor(period);
	const auto zone = Zone();
	const auto whole = Buckets(sessions, range.fromMs, range.toMs, unit, zone);
	auto result = std::vector<Chart::Column>();
	result.reserve(whole.size());
	for (const auto &bucket : whole) {
		auto column = Chart::Column();
		column.label = bucket.label;
		column.total = Metric(bucket, activeOnly);
		result.push_back(std::move(column));
	}

	// One pass per kind, so the stack is the same buckets split rather than a
	// second bucketing that could disagree with the first about a boundary.
	for (auto k = 0; k != int(kKinds.size()); ++k) {
		const auto part = Buckets(
			OfKind(sessions, kKinds[k]),
			range.fromMs,
			range.toMs,
			unit,
			zone);
		for (auto i = 0; i != int(part.size()) && i != int(result.size()); ++i) {
			result[i].byKind[k] = Metric(part[i], activeOnly);
		}
	}
	return result;
}

[[nodiscard]] QString ChangeText(
		const Comparison &comparison,
		bool activeOnly) {
	const auto now = Metric(comparison.current, activeOnly);
	const auto before = Metric(comparison.previous, activeOnly);
	if (!before) {
		// No percentage change from zero, and any number here would be one
		// this box made up.
		return QString();
	}
	const auto delta = now - before;
	if (!delta) {
		return u"the same as the period before"_q;
	}
	const auto percent = (std::abs(delta) * 100 + before / 2) / before;
	return (delta > 0)
		? u"up %1 % on the period before"_q.arg(percent)
		: u"down %1 % on the period before"_q.arg(percent);
}

void ExportCsv(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		const std::vector<Session> &sessions,
		const Range &range) {
	// Sessions rather than buckets: a bucket is one way of looking at the log
	// and a session is what the log actually says, so an export that anybody
	// can re-bucket is worth more than a picture of this box's choices.
	auto text = u"start,end,chat_id,chat,kind,preset,"
		"total_ms,active_ms,hidden\n"_q;
	for (const auto &one : sessions) {
		const auto slice = SliceOf(one, range.fromMs, range.toMs);
		if (!slice.totalMs) {
			continue;
		}
		const auto name = ChatText(session, one.dialogId);
		text += u"%1,%2,%3,\"%4\",%5,%6,%7,%8,%9\n"_q.arg(
			QDateTime::fromMSecsSinceEpoch(one.startMs).toString(Qt::ISODate),
			QDateTime::fromMSecsSinceEpoch(one.endMs).toString(Qt::ISODate),
			QString::number(one.dialogId),
			QString(name).replace('"', u"\"\""_q),
			ScreenTimeKindName(one.chatKind),
			one.preset,
			QString::number(slice.totalMs),
			QString::number(slice.activeMs),
			one.hidden ? u"1"_q : u"0"_q);
	}
	const auto show = box->uiShow();
	FileDialog::GetWritePath(
		box.get(),
		u"Export screen time"_q,
		u"Comma-separated values (*.csv)"_q,
		QDir::homePath() + u"/screentime.csv"_q,
		crl::guard(box, [=](QString &&path) {
			if (path.isEmpty()) {
				return;
			} else if (WriteConfigFile(path, text)) {
				show->showToast(u"Saved to %1."_q.arg(path));
			} else {
				show->showToast(u"Could not write %1."_q.arg(path));
			}
		}));
}

// Every write this box makes. A refusal is the core saying no in words - "the
// budget at that index is not the one you read" - and those words are the only
// useful thing to put in front of somebody whose edit did not land, so they go
// on screen rather than only in the log.
bool Write(
		not_null<Ui::GenericBox*> box,
		Fn<SpliceResult(const QString&)> op,
		const QString &what) {
	const auto result = WriteSettings(std::move(op), what);
	if (!result.ok) {
		box->uiShow()->showBox(Ui::MakeInformBox(result.error));
	}
	return result.ok;
}

// The `target' string as the file spells it, rebuilt from what the box was
// asked for. The parser's grammar and nothing else: a target this got wrong
// would look saved and then not be there, which is why the splice re-reads
// what it wrote before agreeing that it landed.
[[nodiscard]] QString TargetText(const ScreenTimeBudget &budget) {
	switch (budget.kind) {
	case BudgetTarget::All: return u"all"_q;
	case BudgetTarget::Chat: return u"chat:%1"_q.arg(budget.chat);
	case BudgetTarget::Kind:
		return u"kind:%1"_q.arg(ScreenTimeKindName(budget.chatKind));
	case BudgetTarget::Preset: return u"preset:%1"_q.arg(budget.preset);
	}
	return budget.target;
}

// The same target in words. A budget row used to print the file's spelling,
// which was the honest thing to do while the file was the only place budgets
// came from; now that this box writes them, `chat:7654321' is a row nobody can
// read and the name is right there to use.
[[nodiscard]] QString TargetName(
		not_null<Main::Session*> session,
		const ScreenTimeBudget &budget) {
	switch (budget.kind) {
	case BudgetTarget::All: return u"Everything"_q;
	case BudgetTarget::Chat: return ChatText(session, budget.chat);
	case BudgetTarget::Kind: return KindText(budget.chatKind);
	case BudgetTarget::Preset:
		return u"Preset: %1"_q.arg(
			PresetDisplayName(ActiveSettings(), budget.preset));
	}
	return budget.target;
}

// A whole non-negative number, or nothing. An empty field is zero rather than
// a mistake, so "2 hours and no minutes" can be typed as one number.
[[nodiscard]] std::optional<int> FieldNumber(not_null<Ui::InputField*> field) {
	const auto text = field->getLastText().trimmed();
	if (text.isEmpty()) {
		return 0;
	}
	auto ok = false;
	const auto value = text.toInt(&ok);
	return (ok && value >= 0) ? std::make_optional(value) : std::nullopt;
}

// One budget, and the only place the app writes one. `existing' is the budget
// as it was read: what the fields start on, and - through its target - the
// fingerprint the write is checked against, so a box left open while the file
// changed underneath refuses rather than rewriting a budget nobody looked at.
//
// `chat' is a chat to offer as the target outright, which is what this gets
// when it was opened from that chat's own page. Zero when it was opened from
// the list, where the picker is the only way to name a chat.
void BudgetBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		std::optional<ScreenTimeBudget> existing,
		PeerIdValue chat) {
	box->setTitle(rpl::single(existing ? u"Budget"_q : u"New budget"_q));
	box->setWidth(st::boxWideWidth);

	const auto padding = st::boxRowPadding;
	const auto container = box->verticalLayout();
	const auto start = existing.value_or(ScreenTimeBudget{
		.kind = chat ? BudgetTarget::Chat : BudgetTarget::All,
		.chat = chat,
		.perDaySeconds = 60 * 60,
	});

	// What each of the three narrow targets would be if it were picked. Kept
	// beside the radio rather than in it, because switching from a chat to a
	// kind and back should not lose the chat that was already chosen.
	struct Chosen {
		PeerIdValue chat = 0;
		ScreenTimeKind kind = ScreenTimeKind::Private;
		QString preset;
	};
	const auto state = box->lifetime().make_state<Chosen>(Chosen{
		.chat = (start.kind == BudgetTarget::Chat) ? start.chat : chat,
		.kind = (start.kind == BudgetTarget::Kind)
			? start.chatKind
			: ScreenTimeKind::Private,
		.preset = (start.kind == BudgetTarget::Preset)
			? start.preset
			: NormalPreset(),
	});

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"This budget counts"_q,
			st::boxLabel),
		padding);
	const auto target = std::make_shared<Ui::RadiobuttonGroup>(
		int(start.kind));
	const auto addTarget = [&](BudgetTarget value, const QString &text) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				target,
				int(value),
				text,
				st::defaultCheckbox),
			padding);
	};
	addTarget(BudgetTarget::All, u"Everything"_q);
	addTarget(BudgetTarget::Chat, u"One chat"_q);
	addTarget(BudgetTarget::Kind, u"One kind of chat"_q);
	addTarget(BudgetTarget::Preset, u"Time under one preset"_q);

	const auto chatLabel = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	const auto chatName = [=] {
		return state->chat ? ChatText(session, state->chat) : u"Choose"_q;
	};
	const auto chatRow = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Chat"_q),
		chatLabel->events_starting_with(chatName()),
		st::settingsButtonNoIcon);
	chatRow->setClickedCallback([=] {
		// The app's own chooser, PrepareChooseRecipientBox() out of
		// window_peer_menu.h: the same list every forward and every "send to"
		// opens, so a chat is picked here the way it is picked everywhere.
		// It only wants a session, which is all this box has.
		box->uiShow()->showBox(Window::PrepareChooseRecipientBox(
			session,
			crl::guard(box, [=](not_null<Data::Thread*> thread) {
				state->chat = IdOf(thread->peer());
				chatLabel->fire(chatName());

				// Picking a chat is saying the budget is about that chat.
				target->setValue(int(BudgetTarget::Chat));
				return true;
			}),
			rpl::single(u"Chat"_q)));
	});

	const auto kindLabel = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	const auto kindRow = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Kind"_q),
		kindLabel->events_starting_with(KindText(state->kind)),
		st::settingsButtonNoIcon);
	kindRow->setClickedCallback([=] {
		box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> inner) {
			inner->setTitle(rpl::single(u"Kind"_q));
			for (const auto &kind : kKinds) {
				const auto row = ::Settings::AddButtonWithLabel(
					inner->verticalLayout(),
					rpl::single(KindText(kind)),
					rpl::single(QString()),
					st::settingsButtonNoIcon);
				row->setClickedCallback([=] {
					state->kind = kind;
					kindLabel->fire(KindText(kind));
					target->setValue(int(BudgetTarget::Kind));
					inner->closeBox();
				});
			}
			inner->addButton(tr::lng_cancel(), [=] { inner->closeBox(); });
		}));
	});

	const auto presetLabel = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	const auto presetRow = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Preset"_q),
		presetLabel->events_starting_with(
			PresetDisplayName(ActiveSettings(), state->preset)),
		st::settingsButtonNoIcon);
	presetRow->setClickedCallback([=] {
		box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> inner) {
			inner->setTitle(rpl::single(u"Preset"_q));
			const auto add = [=](const QString &name) {
				const auto display = PresetDisplayName(ActiveSettings(), name);
				const auto row = ::Settings::AddButtonWithLabel(
					inner->verticalLayout(),
					rpl::single(display),
					rpl::single(QString()),
					st::settingsButtonNoIcon);
				row->setClickedCallback([=] {
					state->preset = name;
					presetLabel->fire_copy(display);
					target->setValue(int(BudgetTarget::Preset));
					inner->closeBox();
				});
			};

			// Normal first, and by name: it is a bypass with no table of its
			// own, and it is the preset most of a day is spent under.
			add(NormalPreset());
			for (const auto &preset : ActiveSettings().presets) {
				add(preset.name);
			}
			inner->addButton(tr::lng_cancel(), [=] { inner->closeBox(); });
		}));
	});

	// Two fields rather than one duration string. The file spells an allowance
	// the way every other duration in it is spelled and the splice writes that
	// spelling; what a person picking an allowance is choosing is a number of
	// hours and a number of minutes, and asking for those two directly is one
	// less grammar to get wrong.
	const auto hours = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Hours a day"_q),
		QString::number(start.perDaySeconds / 3600)));
	const auto minutes = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		rpl::single(u"Minutes a day"_q),
		QString::number((start.perDaySeconds % 3600) / 60)));

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"When the allowance is gone"_q,
			st::boxLabel),
		padding);
	const auto mode = std::make_shared<Ui::RadiobuttonGroup>(int(start.mode));
	const auto addMode = [&](BudgetMode value, const QString &text) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				mode,
				int(value),
				text,
				st::defaultCheckbox),
			padding);
	};
	addMode(BudgetMode::Soft, u"Soft - a bulletin, once a chat a day"_q);
	addMode(BudgetMode::Hard, u"Hard - a cover over the chat"_q);

	// Only a hard cap puts up anything to snooze, so the two fields follow the
	// mode rather than sitting there meaning nothing. Wrapped rather than
	// hidden so the space goes with them.
	const auto snoozeWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto snoozeRows = snoozeWrap->entity();
	const auto snooze = snoozeRows->add(
		object_ptr<Ui::InputField>(
			snoozeRows,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"Snooze minutes"_q),
			QString::number(start.snoozeSeconds / 60)),
		padding);
	const auto snoozes = snoozeRows->add(
		object_ptr<Ui::InputField>(
			snoozeRows,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"Snoozes a day"_q),
			QString::number(start.snoozesPerDay)),
		padding);
	snoozeRows->add(
		object_ptr<Ui::FlatLabel>(
			snoozeRows,
			u"Zero for either leaves the cover with no way past it."_q,
			st::boxDividerLabel),
		padding);
	snoozeWrap->toggle(start.mode == BudgetMode::Hard, anim::type::instant);
	mode->setChangedCallback([=](int value) {
		snoozeWrap->toggle(
			value == int(BudgetMode::Hard),
			anim::type::normal);
	});

	const auto save = [=] {
		auto budget = ScreenTimeBudget();
		budget.kind = BudgetTarget(target->current());
		switch (budget.kind) {
		case BudgetTarget::Chat: budget.chat = state->chat; break;
		case BudgetTarget::Kind: budget.chatKind = state->kind; break;
		case BudgetTarget::Preset: budget.preset = state->preset; break;
		default: break;
		}
		if (budget.kind == BudgetTarget::Chat && !budget.chat) {
			box->uiShow()->showBox(Ui::MakeInformBox(
				u"Pick the chat this budget is about."_q));
			return;
		}
		budget.target = TargetText(budget);

		const auto perDayHours = FieldNumber(hours);
		const auto perDayMinutes = FieldNumber(minutes);
		if (!perDayHours) {
			hours->showError();
			return;
		} else if (!perDayMinutes) {
			minutes->showError();
			return;
		} else if (hours->getLastText().trimmed().isEmpty()
			&& minutes->getLastText().trimmed().isEmpty()) {
			// An allowance of nothing is a coherent thing to ask for - the
			// core says so - but it is not a thing two empty fields meant, so
			// it has to be typed as a zero rather than left blank.
			hours->showError();
			box->uiShow()->showBox(Ui::MakeInformBox(
				u"Say how long a day this budget allows."_q));
			return;
		}
		budget.perDaySeconds = *perDayHours * 3600 + *perDayMinutes * 60;

		// Read whatever the mode, so that turning a hard budget soft and back
		// again does not quietly put its snooze settings back to the defaults.
		// The file keeps them either way; a soft budget simply never uses them.
		const auto snoozeMinutes = FieldNumber(snooze);
		const auto snoozeCount = FieldNumber(snoozes);
		if (!snoozeMinutes || !snoozeCount) {
			snoozeWrap->toggle(true, anim::type::normal);
			(!snoozeMinutes ? snooze : snoozes)->showError();
			return;
		}
		budget.mode = BudgetMode(mode->current());
		budget.snoozeSeconds = *snoozeMinutes * 60;
		budget.snoozesPerDay = *snoozeCount;

		const auto path = SettingsFilePath();
		const auto index = start.sourceIndex;
		const auto expected = start.target;
		if (existing) {
			Write(box, [=](const QString &text) {
				return SetBudget(text, path, index, expected, budget);
			}, u"a screen time budget"_q);
		} else {
			Write(box, [=](const QString &text) {
				return AppendBudget(text, path, budget);
			}, u"a new screen time budget"_q);
		}

		// Closed either way. On a refusal the file is not what this box was
		// opened on any more, and the list behind it has already rebuilt itself
		// from what the file now says - so the honest next step is to look at
		// that list rather than to keep editing a budget out of a stale copy.
		box->closeBox();
	};

	box->addButton(rpl::single(u"Save"_q), save);
	if (existing) {
		const auto path = SettingsFilePath();
		const auto index = start.sourceIndex;
		const auto expected = start.target;
		box->addLeftButton(rpl::single(u"Delete"_q), [=] {
			Write(box, [=](const QString &text) {
				return RemoveBudget(text, path, index, expected);
			}, u"deleting a screen time budget"_q);
			box->closeBox();
		});
	}
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ChatScreenTimeBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		PeerIdValue dialogId,
		std::vector<Session> sessions,
		Range range,
		bool activeOnly) {
	box->setTitle(rpl::single(ChatText(session, dialogId)));
	box->setWidth(st::boxWideWidth);

	auto own = std::vector<Session>();
	for (const auto &one : sessions) {
		if (one.dialogId == dialogId) {
			own.push_back(one);
		}
	}
	const auto totals = RangeTotals(own, range.fromMs, range.toMs);
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"%1 in this chat, %2 of it active."_q.arg(
				FormatSpan(totals.totalMs),
				ShareText(totals.activeMs, totals.totalMs)),
			st::boxLabel),
		st::boxRowPadding);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Day by day"_q));
	container->add(
		object_ptr<Chart>(
			container,
			Columns(own, range, Period::Week, activeOnly)),
		st::boxRowPadding);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"When in the day"_q));
	container->add(
		object_ptr<Chart>(
			container,
			Columns(own, range, Period::Today, activeOnly)),
		st::boxRowPadding);

	// The one place a budget is offered with a chat already in hand, which is
	// why the editor takes one: from the list behind this there is nothing to
	// mean by "this chat" and the picker is the only way to name one.
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	const auto budget = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"Set a daily budget"_q),
		rpl::single(QString()),
		st::settingsButtonNoIcon);
	budget->setClickedCallback([=] {
		box->uiShow()->showBox(Box(
			BudgetBox,
			session,
			std::optional<ScreenTimeBudget>(),
			dialogId));
	});

	Ui::AddSkip(container);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void ScreenTimeBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(rpl::single(u"Screen time"_q));
	box->setWidth(st::boxWideWidth);

	const auto padding = st::boxRowPadding;
	const auto container = box->verticalLayout();
	const auto state = box->lifetime().make_state<View>();
	const auto rows = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	const auto rebuild = box->lifetime().make_state<Fn<void()>>();
	*rebuild = [=] {
		rows->clear();

		const auto &config = Config();
		const auto wasEnabled = config.enabled;
		const auto again = [=] { crl::on_main(box, [=] { (*rebuild)(); }); };

		const auto enabled = rows->add(
			object_ptr<Ui::Checkbox>(
				rows,
				u"Record screen time"_q,
				wasEnabled,
				st::defaultCheckbox),
			padding);
		enabled->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (checked == wasEnabled) {
				return;
			}
			WriteSettings([=](const QString &text) {
				return SetTableBool(
					text,
					SettingsFilePath(),
					u"screen_time"_q,
					u"enabled_p"_q,
					checked);
			}, u"the screen time switch"_q);

			// Rebuilt whether or not the write landed, so the switch ends up
			// where the file is rather than where the click left it.
			again();
		}, enabled->lifetime());

		if (!wasEnabled) {
			rows->add(
				object_ptr<Ui::FlatLabel>(
					rows,
					u"Off. Nothing is being recorded and no log file exists. "
					"Switched on, this appends one line per event - a chat "
					"opening, an action in the composer, the app leaving the "
					"front - to %1, and reads it back here. The file never "
					"leaves this machine."_q.arg(ScreenTimeLogPath()),
					st::boxDividerLabel),
				padding);
			return;
		}

		state->events = ScreenTimeEvents();
		state->sessions = DeriveSessions(state->events, config);
		const auto range = RangeFor(*state);
		const auto sessions = Filtered(*state);
		const auto comparison = Compare(sessions, range.fromMs, range.toMs);
		const auto &totals = comparison.current;
		const auto activeOnly = state->activeOnly;

		const auto periodRow = ::Settings::AddButtonWithLabel(
			rows,
			rpl::single(u"Period"_q),
			rpl::single(PeriodText(*state)),
			st::settingsButtonNoIcon);
		periodRow->setClickedCallback([=] {
			// Four choices and a range, so a box of radio rows rather than a
			// menu: the custom one opens two more pickers and a menu that
			// spawns a box behind itself is a jump nobody asked for.
			box->uiShow()->showBox(Box([=](
					not_null<Ui::GenericBox*> inner) {
				inner->setTitle(rpl::single(u"Period"_q));
				const auto choose = [=](Period period) {
					state->period = period;
					inner->closeBox();
					again();
				};
				const auto add = [&](const QString &text, Period period) {
					const auto row = ::Settings::AddButtonWithLabel(
						inner->verticalLayout(),
						rpl::single(text),
						rpl::single(QString()),
						st::settingsButtonNoIcon);
					row->setClickedCallback([=] { choose(period); });
				};
				add(u"Today"_q, Period::Today);
				add(u"This week"_q, Period::Week);
				add(u"This month"_q, Period::Month);

				const auto custom = ::Settings::AddButtonWithLabel(
					inner->verticalLayout(),
					rpl::single(u"Custom range"_q),
					rpl::single(QString()),
					st::settingsButtonNoIcon);
				custom->setClickedCallback([=] {
					// Two pickers, first day then last, because there is no
					// range picker in the app and two dates asked in order is
					// the shape every other date range here already has.
					inner->uiShow()->showBox(Box([=](
							not_null<Ui::GenericBox*> first) {
						Ui::ChooseDateTimeBox(first, {
							.title = rpl::single(u"From"_q),
							.submit = rpl::single(u"Next"_q),
							.done = [=](TimeId chosen) {
								const auto from = base::unixtime::parse(
									chosen).date();
								first->closeBox();
								inner->uiShow()->showBox(Box([=](
										not_null<Ui::GenericBox*> second) {
									Ui::ChooseDateTimeBox(second, {
										.title = rpl::single(u"To"_q),
										.submit = rpl::single(u"Show"_q),
										.done = [=](TimeId till) {
											state->period = Period::Custom;
											state->customFrom = from;
											state->customTo
												= base::unixtime::parse(
													till).date();
											second->closeBox();
											inner->closeBox();
											again();
										},
										.time = chosen,
									});
								}));
							},
							.time = base::unixtime::now() - 7 * 86400,
						});
					}));
				});
				inner->addButton(tr::lng_cancel(), [=] { inner->closeBox(); });
			}));
		});

		const auto total = Metric(totals, activeOnly);
		const auto change = ChangeText(comparison, activeOnly);
		auto headline = activeOnly
			? u"%1 active"_q.arg(FormatSpan(totals.activeMs))
			: u"%1, %2 of it active"_q.arg(
				FormatSpan(totals.totalMs),
				ShareText(totals.activeMs, totals.totalMs));
		if (!change.isEmpty()) {
			headline += u" - "_q + change;
		}
		rows->add(
			object_ptr<Ui::FlatLabel>(rows, headline + '.', st::boxLabel),
			padding);

		Ui::AddSkip(rows);
		rows->add(
			object_ptr<Chart>(
				rows,
				Columns(sessions, range, state->period, activeOnly)),
			padding);

		// The legend is a sentence rather than a row of swatches: five colours
		// with five words beside them is the same information and twice the
		// widgets.
		auto legend = QStringList();
		for (const auto &kind : totals.kinds) {
			legend.push_back(u"%1 %2"_q.arg(
				KindText(kind.chatKind),
				FormatSpan(activeOnly ? kind.activeMs : kind.totalMs)));
		}
		if (!legend.isEmpty()) {
			rows->add(
				object_ptr<Ui::FlatLabel>(
					rows,
					legend.join(u" · "_q),
					st::boxDividerLabel),
				padding);
		}

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		Ui::AddSubsectionTitle(rows, rpl::single(u"Filters"_q));

		const auto active = rows->add(
			object_ptr<Ui::Checkbox>(
				rows,
				u"Active only"_q,
				activeOnly,
				st::defaultCheckbox),
			padding);
		active->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (checked != state->activeOnly) {
				state->activeOnly = checked;
				again();
			}
		}, active->lifetime());

		for (auto i = 0; i != int(kKinds.size()); ++i) {
			const auto kind = kKinds[i];
			const auto was = state->kinds[i];
			const auto check = rows->add(
				object_ptr<Ui::Checkbox>(
					rows,
					KindText(kind),
					was,
					st::defaultCheckbox),
				padding);
			check->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				if (checked != state->kinds[i]) {
					state->kinds[i] = checked;
					again();
				}
			}, check->lifetime());
		}

		const auto presetRow = ::Settings::AddButtonWithLabel(
			rows,
			rpl::single(u"Preset"_q),
			rpl::single(!state->presetFilter
				? u"Every preset"_q
				: state->preset.isEmpty()
				? u"Normal"_q
				: state->preset),
			st::settingsButtonNoIcon);
		presetRow->setClickedCallback([=] {
			box->uiShow()->showBox(Box([=](
					not_null<Ui::GenericBox*> inner) {
				inner->setTitle(rpl::single(u"Preset"_q));
				const auto choose = [=](bool filter, const QString &name) {
					state->presetFilter = filter;
					state->preset = name;
					inner->closeBox();
					again();
				};
				const auto add = [&](
						const QString &text,
						bool filter,
						const QString &name) {
					const auto row = ::Settings::AddButtonWithLabel(
						inner->verticalLayout(),
						rpl::single(text),
						rpl::single(QString()),
						st::settingsButtonNoIcon);
					row->setClickedCallback([=] { choose(filter, name); });
				};
				add(u"Every preset"_q, false, QString());

				// Normal has no table in the file and is spelled as the empty
				// preset in the log, so it is offered by name rather than
				// found among the presets below.
				add(u"Normal"_q, true, QString());
				for (const auto &preset : ActiveSettings().presets) {
					add(preset.name, true, preset.name);
				}
				inner->addButton(tr::lng_cancel(), [=] { inner->closeBox(); });
			}));
		});

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		Ui::AddSubsectionTitle(rows, rpl::single(u"Chats"_q));

		if (totals.chats.empty()) {
			rows->add(
				object_ptr<Ui::FlatLabel>(
					rows,
					u"Nothing in this period."_q,
					st::boxDividerLabel),
				padding);
		}
		// The longest chat sets the bar that fills the row, so the list is a
		// picture of proportions rather than of absolute minutes.
		const auto top = totals.chats.empty()
			? int64(0)
			: (activeOnly
				? totals.chats.front().activeMs
				: totals.chats.front().totalMs);
		auto shown = 0;
		for (const auto &chat : totals.chats) {
			if (shown++ >= kTopChats) {
				break;
			}
			const auto value = activeOnly ? chat.activeMs : chat.totalMs;
			const auto peer = chat.dialogId
				? PeerFor(session, chat.dialogId)
				: nullptr;
			const auto right = activeOnly
				? FormatSpan(chat.activeMs)
				: u"%1 · %2 active"_q.arg(
					FormatSpan(chat.totalMs),
					ShareText(chat.activeMs, chat.totalMs));
			const auto row = rows->add(
				object_ptr<ChatRow>(
					rows,
					peer,
					ChatText(session, chat.dialogId),
					chat.chatKind,
					value,
					top,
					right));
			const auto id = chat.dialogId;
			if (id) {
				row->setClickedCallback([=] {
					box->uiShow()->showBox(Box(
						ChatScreenTimeBox,
						session,
						id,
						sessions,
						range,
						activeOnly));
				});
			}
		}

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		Ui::AddSubsectionTitle(rows, rpl::single(u"Reading load"_q));
		rows->add(
			object_ptr<Ui::FlatLabel>(
				rows,
				u"Every day in the period folded onto one clock, which is "
				"what a schedule window is placed by."_q,
				st::boxDividerLabel),
			padding);
		rows->add(
			object_ptr<Chart>(
				rows,
				Columns(sessions, range, Period::Today, activeOnly)),
			padding);

		if (state->period == Period::Month) {
			Ui::AddSkip(rows);
			Ui::AddSubsectionTitle(rows, rpl::single(u"Hour by weekday"_q));
			rows->add(
				object_ptr<Heat>(
					rows,
					HeatMapFor(sessions, range.fromMs, range.toMs, Zone()),
					activeOnly),
				padding);
		}

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		rows->add(
			object_ptr<Ui::FlatLabel>(
				rows,
				u"Hidden while peeking: %1."_q.arg(
					FormatSpan(totals.hiddenMs)),
				st::boxLabel),
			padding);
		rows->add(
			object_ptr<Ui::FlatLabel>(
				rows,
				PeekedText(PeekUsageIn(
					DerivePeeks(state->events),
					range.fromMs,
					range.toMs)),
				st::boxLabel),
			padding);

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		Ui::AddSubsectionTitle(rows, rpl::single(u"Budgets"_q));

		const auto ledger = BudgetLedger(
			state->events,
			config,
			QDate::currentDate(),
			Zone());
		if (ledger.empty()) {
			rows->add(
				object_ptr<Ui::FlatLabel>(
					rows,
					u"None yet. A budget is a day's allowance for one thing - "
					"everything, one chat, one kind of chat, or the time under "
					"one preset - and it either shows a bulletin when the "
					"allowance is gone or puts a cover over the chat. Adding "
					"one writes an [[screen_time.budgets]] entry in %1, which "
					"you can also edit by hand."_q.arg(SettingsFilePath()),
					st::boxDividerLabel),
				padding);
		}
		for (const auto &spent : ledger) {
			if (spent.index < 0 || spent.index >= int(config.budgets.size())) {
				continue;
			}
			const auto budget = config.budgets[spent.index];
			const auto row = ::Settings::AddButtonWithLabel(
				rows,
				rpl::single(u"%1 · %2"_q.arg(
					TargetName(session, budget),
					BudgetModeName(budget.mode))),
				rpl::single(spent.reached
					? u"%1 of %2 · reached"_q.arg(
						FormatSpan(spent.spentMs),
						FormatSpan(spent.perDayMs))
					: u"%1 of %2"_q.arg(
						FormatSpan(spent.spentMs),
						FormatSpan(spent.perDayMs))),
				st::settingsButtonNoIcon);
			row->setClickedCallback([=] {
				box->uiShow()->showBox(Box(
					BudgetBox,
					session,
					std::make_optional(budget),
					PeerIdValue(0)));
			});
		}

		const auto addBudget = ::Settings::AddButtonWithLabel(
			rows,
			rpl::single(u"Add a budget"_q),
			rpl::single(QString()),
			st::settingsButtonNoIcon);
		addBudget->setClickedCallback([=] {
			box->uiShow()->showBox(Box(
				BudgetBox,
				session,
				std::optional<ScreenTimeBudget>(),
				PeerIdValue(0)));
		});

		Ui::AddSkip(rows);
		Ui::AddDivider(rows);
		const auto exportRow = ::Settings::AddButtonWithLabel(
			rows,
			rpl::single(u"Export as CSV"_q),
			rpl::single(QString()),
			st::settingsButtonNoIcon);
		exportRow->setClickedCallback([=] {
			ExportCsv(box, session, sessions, range);
		});
		rows->add(
			object_ptr<Ui::FlatLabel>(
				rows,
				u"The log is %1. It never leaves this machine."_q.arg(
					ScreenTimeLogPath()),
				st::boxDividerLabel),
			padding);
		Ui::AddSkip(rows);
	};

	(*rebuild)();

	// An outside edit to settings.toml can change a threshold, add a budget or
	// switch the whole thing off, and all three change every number here.
	SettingsChanges(
	) | rpl::on_next([=] {
		(*rebuild)();
	}, box->lifetime());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

rpl::producer<QString> ScreenTimeDigestValue(
		not_null<Main::Session*> session) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		SettingsChanges()
	) | rpl::map([=] {
		const auto &config = Config();
		if (!config.enabled) {
			return u"Off"_q;
		}
		const auto zone = Zone();
		const auto today = QDate::currentDate();
		const auto start = today.addDays(1 - today.dayOfWeek());
		const auto from = MsAt(start, zone);
		const auto till = MsAt(start.addDays(7), zone);
		const auto sessions = DeriveSessions(ScreenTimeEvents(), config);
		const auto totals = RangeTotals(sessions, from, till);
		if (!totals.totalMs) {
			return u"Nothing this week"_q;
		}
		auto result = u"This week: %1, %2 active"_q.arg(
			FormatSpan(totals.totalMs),
			ShareText(totals.activeMs, totals.totalMs));
		if (!totals.chats.empty()) {
			result += u", top: "_q
				+ ChatText(session, totals.chats.front().dialogId);
		}
		return result;
	});
}

} // namespace Purple
