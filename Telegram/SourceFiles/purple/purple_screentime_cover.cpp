/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_screentime_cover.h"

#include "base/timer.h"
#include "data/data_peer.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_screentime.h"
#include "purple/purple_screentime_recorder.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>

namespace Purple {
namespace {

// How often the ledger is re-derived while a chat is open. A budget is spent
// by the minute, so a minute late is the worst this can be, and deriving the
// whole day's sessions on every paint would be the alternative.
constexpr auto kTick = crl::time(30 * 1000);

[[nodiscard]] const ScreenTime &Config() {
	return ActiveSettings().screenTime;
}

[[nodiscard]] int64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

// Whether this budget is counting the chat in front of you. The preset case
// asks what is running now rather than what was running when the time was
// spent: a preset budget is a rule about the mode you are in, and you are in
// it now.
[[nodiscard]] bool Covers(
		const ScreenTimeBudget &budget,
		not_null<PeerData*> peer) {
	switch (budget.kind) {
	case BudgetTarget::All:
		return true;
	case BudgetTarget::Chat:
		return (budget.chat == IdOf(peer));
	case BudgetTarget::Kind:
		return (budget.chatKind == ScreenTimeKindFor(KindOf(peer)));
	case BudgetTarget::Preset:
		return (budget.preset
			== (Filtering() ? CurrentState().activePreset : QString()));
	}
	return false;
}

[[nodiscard]] QString TargetText(const ScreenTimeBudget &budget) {
	// The target as the file wrote it. A budget is a line somebody typed, and
	// a cover that renamed it would be harder to find in the file than one
	// that quotes it.
	return budget.target.isEmpty() ? u"all"_q : budget.target;
}

[[nodiscard]] QString MinutesText(int seconds) {
	const auto minutes = std::max(seconds / 60, 1);
	return (minutes == 1)
		? u"1 more minute"_q
		: u"%1 more minutes"_q.arg(minutes);
}

} // namespace

class ScreenTimeCover::Widget final : public Ui::RpWidget {
public:
	explicit Widget(not_null<Ui::RpWidget*> parent);

	void setPeer(PeerData *peer);
	void place(QRect geometry);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void check();
	void layout();
	void noteSoft(const ScreenTimeBudget &budget);

	PeerData *_peer = nullptr;

	// The hard budget the cover is up for, by its index in
	// [[screen_time.budgets]], or -1 for none.
	int _index = -1;
	QString _target;
	QString _limit;

	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _detail;
	object_ptr<Ui::RoundButton> _snooze;

	base::Timer _timer;

	// The soft budgets already announced today, as "day|index|peer". In memory
	// only: a restart says it once more, which is the smaller of the two
	// mistakes a bulletin can make.
	base::flat_set<QString> _announced;

	rpl::lifetime _lifetime;

};

ScreenTimeCover::Widget::Widget(not_null<Ui::RpWidget*> parent)
: RpWidget(parent)
, _title(this, u"Time is up"_q, st::boxTitle)
, _detail(this, QString(), st::boxLabel)
, _snooze(this, rpl::single(QString()), st::defaultActiveButton)
, _timer([=] { check(); }) {
	hide();
	_detail->setTextColorOverride(st::windowSubTextFg->c);

	_snooze->setClickedCallback([=] {
		if (_index < 0) {
			return;
		}
		NoteScreenTimeSnooze(_index);
		check();
	});

	// A changed budget, a changed threshold, or the whole feature switched off
	// all change whether this should be up at all.
	SettingsChanges(
	) | rpl::on_next([=] {
		check();
	}, _lifetime);

	sizeValue(
	) | rpl::on_next([=] {
		layout();
	}, _lifetime);
}

void ScreenTimeCover::Widget::setPeer(PeerData *peer) {
	if (_peer == peer) {
		return;
	}
	_peer = peer;
	check();
}

void ScreenTimeCover::Widget::place(QRect geometry) {
	setGeometry(geometry);
	if (!isHidden()) {
		raise();
	}
}

void ScreenTimeCover::Widget::check() {
	const auto &config = Config();
	if (!config.enabled || !_peer || config.budgets.empty()) {
		_index = -1;
		_timer.cancel();
		hide();
		return;
	}

	// Kept running while a chat is open even when nothing is covered, because
	// the interesting moment is the one where a budget becomes spent, and
	// nothing else would notice it.
	if (!_timer.isActive()) {
		_timer.callEach(kTick);
	}

	const auto ledger = BudgetLedger(
		ScreenTimeEvents(),
		config,
		QDate::currentDate(),
		QTimeZone::systemTimeZone());
	const auto now = NowMs();
	auto found = -1;
	for (const auto &spent : ledger) {
		if (!spent.reached
			|| spent.index < 0
			|| spent.index >= int(config.budgets.size())) {
			continue;
		}
		const auto &budget = config.budgets[spent.index];
		if (!Covers(budget, _peer)) {
			continue;
		}
		if (budget.mode == BudgetMode::Soft) {
			noteSoft(budget);
			continue;
		}
		if (ScreenTimeSnoozeUntil(spent.index) > now) {
			// A snooze is running, so this one has been answered for the
			// moment. Every other reached budget still gets its turn below.
			continue;
		}
		found = spent.index;
		_target = TargetText(budget);
		_limit = u"%1 minutes a day"_q.arg(std::max(
			budget.perDaySeconds / 60,
			0));
		const auto allowed = CoverAllowed(
			ScreenTimeSnoozesUsed(spent.index),
			budget);
		_snooze->setText(rpl::single(MinutesText(budget.snoozeSeconds)));
		_snooze->setVisible(allowed);
		break;
	}
	_index = found;
	if (found < 0) {
		hide();
		return;
	}
	_detail->setText(u"The budget for %1 is spent - %2. Messages and "
		"notifications are untouched, and the time you spend here still "
		"counts."_q.arg(_target, _limit));
	layout();
	show();
	raise();
}

void ScreenTimeCover::Widget::noteSoft(const ScreenTimeBudget &budget) {
	if (!_peer) {
		return;
	}
	const auto key = u"%1|%2|%3"_q.arg(
		QDate::currentDate().toString(Qt::ISODate),
		TargetText(budget),
		QString::number(_peer->id.value));
	if (_announced.contains(key)) {
		return;
	}
	_announced.emplace(key);
	Ui::Toast::Show(
		parentWidget(),
		u"Screen time: the budget for %1 is spent."_q.arg(
			TargetText(budget)));
}

void ScreenTimeCover::Widget::layout() {
	const auto skip = st::boxRowPadding.left();
	const auto inner = std::max(width() - 2 * skip, 1);
	_title->resizeToWidth(inner);
	_detail->resizeToWidth(inner);
	const auto buttonHeight = _snooze->isHidden() ? 0 : _snooze->height();
	const auto total = _title->height()
		+ skip
		+ _detail->height()
		+ (buttonHeight ? (skip + buttonHeight) : 0);
	auto top = std::max((height() - total) / 2, skip);
	_title->moveToLeft(skip, top, width());
	top += _title->height() + skip;
	_detail->moveToLeft(skip, top, width());
	top += _detail->height() + skip;
	if (buttonHeight) {
		_snooze->moveToLeft(skip, top, width());
	}
}

void ScreenTimeCover::Widget::paintEvent(QPaintEvent *e) {
	// Opaque, not a veil: a cover you can read the chat through is one you
	// read the chat through.
	QPainter(this).fillRect(e->rect(), st::boxBg);
}

ScreenTimeCover::ScreenTimeCover(not_null<Ui::RpWidget*> parent)
: _widget(std::make_unique<Widget>(parent)) {
}

ScreenTimeCover::~ScreenTimeCover() = default;

void ScreenTimeCover::setPeer(PeerData *peer) {
	_widget->setPeer(peer);
}

void ScreenTimeCover::setGeometry(QRect geometry) {
	_widget->place(geometry);
}

} // namespace Purple
