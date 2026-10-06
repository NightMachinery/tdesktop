#include "purple/purple_ui_witness.h"

#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/view/media/history_view_document.h"
#include "history/view/history_view_message.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "lang/lang_instance.h"
#include "ui/style/style_core.h"
#include "purple/purple_ui_witness_geometry.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QAction>
#include <QCoreApplication>
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSocketNotifier>
#include <QTimer>
#include <limits>

#ifdef Q_OS_MAC
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Purple {
namespace {

using namespace UiWitness;

class Observer;
QPointer<Observer> Current;

struct Target {
	FullMsgId id;
	HistoryItem *identity = nullptr;
	DocumentData *document = nullptr;
	qint64 documentSize = 0;
	QString documentName;
	HistoryView::Element *view = nullptr;
	const Ui::Text::String *leaf = nullptr;
	QByteArray itemToken;
	QByteArray documentToken;
	Cells cells;
	NativeWindow window;
	UiWitnessView state;
	quint64 epoch = 0;
	int seen = 0;
	TimeId date = 0;
	bool painted = false;
	bool keyboard = false;
	QFont font;
	int scale = 0;
	bool rtl = false;
};

struct Receiver {
	QPointer<QWidget> widget;
	base::weak_ptr<Window::SessionController> controller;
	base::weak_ptr<Main::Session> session;
	ReceiverKind kind = ReceiverKind::Main;
	UiWitnessLookup lookup;
	QByteArray token;
	QByteArray controllerToken;
	std::array<Target, 2> targets;
	rpl::lifetime lifetime;
	QRect paintClip;
	quint64 paintEpoch = 0;
	bool completePaint = false;
};

struct Menu {
	QPointer<Ui::PopupMenu> widget;
	QPointer<QAction> action;
	int receiver = -1;
	int slot = -1;
	int reason = -1;
	FullMsgId id;
	HistoryItem *item = nullptr;
	DocumentData *document = nullptr;
	bool registered = false;
	bool originKeyboard = false;
	Ui::PopupMenu::AnimatePhase phase = Ui::PopupMenu::AnimatePhase::Hidden;
	rpl::lifetime lifetime;
	base::weak_ptr<Window::SessionController> controller;
};

class Observer final : public QObject {
public:
	Observer(int fd, QByteArray run, QObject *parent);
	~Observer();
	void stop();
	void dirty(unsigned mask = 3, bool retire = false);
	[[nodiscard]] int registerReceiver(
		QWidget *widget,
		Window::SessionController *controller,
		ReceiverKind kind,
		UiWitnessLookup lookup);
	void beginPaint(int index, const QRegion &region);
	void endPaint(int index);
	[[nodiscard]] std::optional<int> admit(
		int receiver,
		HistoryItem *container,
		HistoryItem *owner,
		const Ui::Text::String &text,
		bool caption);
	[[nodiscard]] UiWitnessView resolve(int receiver, Target &target, int slot);
	[[nodiscard]] QJsonObject snapshot(int receiver, int slot, Facet facet);
	void importAction(
		Ui::PopupMenu *menu,
		Window::SessionController *controller,
		HistoryItem *item,
		DocumentData *document,
		QAction *action);
	void registerMenu(QWidget*, Ui::PopupMenu*, int reason, bool blocked);
	[[nodiscard]] bool eventFilter(QObject*, QEvent*) override;
	void read();
	void write();
	void dispatch(const Request &request);
	[[nodiscard]] QByteArray token();
	[[nodiscard]] qint64 now() const;
	[[nodiscard]] bool active() const { return !_stopped; }

	std::array<Receiver, 2> receivers;
	int receiverCount = 0;
	int painting = -1;
	quint64 epoch = 1;

private:
	void subscribe(Receiver &receiver);
	void reply(const Request&, QJsonObject authority, bool valid, int code, qint64 deadline);

	int _fd = -1;
	QByteArray _run;
	QElapsedTimer _clock;
	QSocketNotifier *_read = nullptr;
	QSocketNotifier *_write = nullptr;
	QTimer _timer;
	Reader _reader;
	Outbox _outbox;
	Lease _lease;
	Menu _menu;
	rpl::lifetime _lifetime;
	std::array<QByteArray, 32> _nonces;
	int _nonceCount = 0;
	quint64 _counter = 0;
	qint64 _partialStart = -1;
	bool _stopped = false;

};

QJsonArray Rect(QRectF r) {
	return {int(r.x() * 256), int(r.y() * 256), int(r.width() * 256), int(r.height() * 256)};
}

QString Token(QByteArrayView value) {
	return QString::fromLatin1(value);
}

bool LeafAgrees(const Ui::Text::String &leaf, QStringView literal) {
	if (leaf.length() < literal.size() || leaf.length() > literal.size() + 1
		|| leaf.hasPersistentAnimation()) {
		return false;
	}
	const auto content = leaf.toTextWithEntities();
	return content.text == literal && content.entities.empty();
}

bool AllowedContext(HistoryView::Element *view, ReceiverKind kind) {
	if (!view) { return false; }
	const auto context = view->delegate()->elementContext();
	return kind == ReceiverKind::Main
		? context == HistoryView::Context::History
		: context == HistoryView::Context::Pinned;
}

const Ui::Text::String *LiveLeaf(HistoryView::Element *view, int slot) {
	if (!view) {
		return nullptr;
	} else if (!slot) {
		const auto message = dynamic_cast<HistoryView::Message*>(view);
		return message ? &message->bodyText() : nullptr;
	}
	const auto document = dynamic_cast<HistoryView::Document*>(view->media());
	return document ? document->captionText() : nullptr;
}

Observer::Observer(int fd, QByteArray run, QObject *parent)
: QObject(parent)
, _fd(fd)
, _run(std::move(run)) {
	_clock.start();
	Lang::GetInstance().updated() | rpl::on_next([=] { dirty(); }, _lifetime);
	style::PaletteChanged() | rpl::on_next([=] { dirty(); }, _lifetime);
	if (const auto app = qobject_cast<QApplication*>(parent)) {
		connect(app, &QApplication::focusChanged, this, [=] { dirty(); });
		connect(app, &QGuiApplication::applicationStateChanged, this, [=] { dirty(); });
	}
#ifdef Q_OS_MAC
	_read = new QSocketNotifier(_fd, QSocketNotifier::Read, this);
	_write = new QSocketNotifier(_fd, QSocketNotifier::Write, this);
	_write->setEnabled(false);
	connect(_read, &QSocketNotifier::activated, this, [=] { read(); });
	connect(_write, &QSocketNotifier::activated, this, [=] { write(); });
	connect(&_timer, &QTimer::timeout, this, [=] {
		if (now() >= kSessionMs || (_partialStart >= 0 && now() - _partialStart >= 1000)) {
			stop();
		} else if (!_outbox.empty()) {
			write();
		}
	});
	_timer.start(25);
	const auto hello = QJsonObject{{u"schema"_q, 1}, {u"type"_q, 1},
		{u"run"_q, Token(_run)}, {u"catalog"_q, 1}};
	if (!_outbox.queue(hello, now() + 1000)) {
		stop();
	} else {
		write();
	}
#endif
}

Observer::~Observer() { stop(); }
qint64 Observer::now() const { return _clock.elapsed(); }

void Observer::stop() {
	if (_stopped) {
		return;
	}
	_stopped = true;
	_lifetime.destroy();
	_timer.stop();
	if (_read) { _read->setEnabled(false); }
	if (_write) { _write->setEnabled(false); }
	_lease.close();
	_outbox.clear();
	if (_menu.widget) { _menu.widget->removeEventFilter(this); }
	_menu = {};
	for (auto &r : receivers) {
		r.lifetime.destroy();
		if (r.widget) {
			r.widget->removeEventFilter(this);
			r.widget->window()->removeEventFilter(this);
		}
		r.targets = {};
		r.lookup = nullptr;
	}
#ifdef Q_OS_MAC
	if (_fd >= 0) { ::close(_fd); }
#endif
	_fd = -1;
	painting = -1;
	if (Current == this) { Current = nullptr; }
}

QByteArray Observer::token() {
	if (_stopped || ++_counter > 256) {
		stop();
		return {};
	}
	return EqualityToken(_run, _counter);
}

void Observer::dirty(unsigned mask, bool retire) {
	if (_stopped) { return; }
	if (epoch == std::numeric_limits<quint64>::max()) { stop(); return; }
	++epoch;
	_lease.invalidate();
	for (auto i = 0; i != receiverCount; ++i) {
		if (!(mask & (1U << i))) { continue; }
		for (auto &target : receivers[i].targets) {
			target.painted = false;
			if (retire) { target = {}; }
		}
	}
	_outbox.invalidate(mask, epoch, now() + 1000);
	if (_write) { _write->setEnabled(true); }
}

void Observer::subscribe(Receiver &r) {
	const auto session = r.session.get();
	if (!session) { stop(); return; }
	auto &d = session->data();
	rpl::merge(d.itemLayoutChanged(), d.itemRepaintRequest(), d.itemResizeRequest(),
		d.itemViewRefreshRequest(), d.itemRemoved())
	| rpl::on_next([=] { dirty(); }, r.lifetime);
	d.itemRemoved() | rpl::on_next([=] { dirty(3, true); }, r.lifetime);
	rpl::merge(d.viewLayoutChanged(), d.viewRemoved())
	| rpl::on_next([=] { dirty(); }, r.lifetime);
	d.viewRepaintRequest() | rpl::on_next([=] { dirty(); }, r.lifetime);
	d.viewResizeRequest() | rpl::on_next([=] { dirty(); }, r.lifetime);
	d.viewHeightAdjusted() | rpl::on_next([=] { dirty(); }, r.lifetime);
	d.itemDataChanges() | rpl::on_next([=] { dirty(); }, r.lifetime);
	d.newItemAdded() | rpl::on_next([=] { dirty(); }, r.lifetime);
	d.itemIdChanged() | rpl::on_next([=] { dirty(3, true); }, r.lifetime);
	d.historyCleared() | rpl::on_next([=] { dirty(3, true); }, r.lifetime);
	d.historyUnloaded() | rpl::on_next([=] { dirty(3, true); }, r.lifetime);
	d.sessionDataAboutToBeCleared() | rpl::on_next([=] { stop(); }, r.lifetime);
	connect(r.widget, &QObject::destroyed, this, [=] { dirty(3, true); });
}

int Observer::registerReceiver(QWidget *w, Window::SessionController *c,
		ReceiverKind kind, UiWitnessLookup lookup) {
	if (_stopped || !w || !c || !lookup) { return -1; }
	for (auto i = 0; i != receiverCount; ++i) {
		auto &r = receivers[i];
		if (r.widget != w) { continue; }
		if (r.controller.get() != c || r.session.get() != &c->session() || r.kind != kind) {
			stop();
			return -1;
		}
		r.lookup = std::move(lookup);
		return i;
	}
	if (receiverCount == receivers.size()) { stop(); return -1; }
	const auto index = receiverCount++;
	auto &r = receivers[index];
	r.widget = w;
	r.controller = base::make_weak(c);
	r.session = base::make_weak(&c->session());
	r.kind = kind;
	r.lookup = std::move(lookup);
	r.token = token();
	r.controllerToken = token();
	if (_stopped) { return -1; }
	w->installEventFilter(this);
	w->window()->installEventFilter(this);
	subscribe(r);
	return _stopped ? -1 : index;
}

void Observer::beginPaint(int index, const QRegion &region) {
	dirty(1U << index);
	if (_stopped) { return; }
	auto &r = receivers[index];
	for (auto &target : r.targets) { target.seen = 0; target.painted = false; }
	r.paintEpoch = epoch;
	r.completePaint = region.rectCount() == 1;
	r.paintClip = region.boundingRect();
	painting = index;
}

void Observer::endPaint(int index) {
	auto &r = receivers[index];
	for (auto &target : r.targets) {
		if (target.seen != 1 || r.paintEpoch != epoch || !r.completePaint) {
			target.painted = false;
		}
	}
	painting = -1;
}

UiWitnessView Observer::resolve(int index, Target &t, int slot) {
	auto &r = receivers[index];
	const auto c = r.controller.get();
	const auto session = r.session.get();
	if (!r.widget || !c || !session || !t.identity || !t.document
		|| &c->session() != session || !r.lookup) {
		return {};
	}
	const auto item = session->data().message(t.id);
	if (!item || item != t.identity || item->date() != t.date
		|| item->groupId() || item->richPage() || item->translatedRichPage()
		|| UiWitnessQualifyingDocument(session, item) != t.document
		|| t.document->size != t.documentSize || t.document->filename() != t.documentName) {
		return {};
	}
	const auto &raw = item->originalText();
	if (!raw.entities.empty() || LiteralSlot(raw.text, slot == 1) != slot) {
		return {};
	}
	const auto result = r.lookup(item);
	const auto leaf = LiveLeaf(result.view, slot);
	if (!AllowedContext(result.view, r.kind)
		|| result.view->data() != item || !result.view->media()
		|| result.view->media()->getDocument() != t.document || !leaf || leaf != t.leaf
		|| !LeafAgrees(*leaf, raw.text) || !leaf->style()
		|| leaf->style()->font->f != t.font || style::Scale() != t.scale
		|| style::RightToLeft() != t.rtl || result.native.receiver != r.widget
		|| !result.native.ready || result.native.viewport.isEmpty()
		|| result.view->isHidden() || result.view->pendingResize()) {
		return {};
	}
	return result;
}

std::optional<int> Observer::admit(int index, HistoryItem *container,
		HistoryItem *owner, const Ui::Text::String &text, bool caption) {
	if (_stopped || !owner || owner != container || owner->groupId()
		|| owner->richPage() || owner->translatedRichPage()) {
		return std::nullopt;
	}
	auto &r = receivers[index];
	const auto session = r.session.get();
	const auto controller = r.controller.get();
	if (!session || !controller || &controller->session() != session) { return std::nullopt; }
	const auto document = UiWitnessQualifyingDocument(session, owner);
	if (!document) { return std::nullopt; }
	const auto &raw = owner->originalText();
	const auto slot = LiteralSlot(raw.text, caption);
	if (!slot || !raw.entities.empty() || !LeafAgrees(text, raw.text)) { return std::nullopt; }
	auto &t = r.targets[*slot];
	if (++t.seen != 1) { t.painted = false; return std::nullopt; }
	if (t.identity != owner || t.document != document) {
		t.itemToken = token();
		t.documentToken = token();
		if (_stopped) { return std::nullopt; }
	}
	t.date = owner->date();
	t.id = owner->fullId();
	t.identity = owner;
	t.document = document;
	t.documentSize = document->size;
	t.documentName = document->filename();
	t.leaf = &text;
	if (!text.style()) { return std::nullopt; }
	t.font = text.style()->font->f;
	t.scale = style::Scale();
	t.rtl = style::RightToLeft();
	const auto state = r.lookup(owner);
	t.view = state.view;
	if (!resolve(index, t, *slot).native.ready) { return std::nullopt; }
	return slot;
}

QJsonObject Observer::snapshot(int index, int slot, Facet facet) {
	if (_stopped || painting >= 0 || index < 0
		|| index >= receiverCount || slot < 0 || slot > 1) {
		return {};
	}
	auto &r = receivers[index];
	auto &t = r.targets[slot];
	const auto live = resolve(index, t, slot);
	if (!live.native.ready) { return {}; }
	const auto root = ResolveNativeWindow(r.widget);
	if (!r.widget->window()->isActiveWindow()
		&& !(facet == Facet::Menu && _menu.widget && _menu.widget->isVisible())) { return {}; }
	if (!root) { return {}; }
	const auto focus = live.focused == t.identity ? 1 : (live.focused ? 2 : 0);
	const auto origin = r.widget->mapToGlobal(QPoint());
	auto a = QJsonObject{{u"catalog"_q, 1}, {u"receiver"_q, Token(r.token)},
		{u"controller"_q, Token(r.controllerToken)}, {u"item"_q, Token(t.itemToken)},
		{u"document"_q, Token(t.documentToken)}, {u"slot"_q, slot},
		{u"facet"_q, int(facet)}, {u"kind"_q, int(r.kind)},
		{u"epoch"_q, QString::number(epoch, 16)}, {u"focus"_q, focus},
		{u"selected"_q, 0}, {u"keyboard"_q, int(r.widget->hasFocus())},
		{u"window"_q, int(root->number)}, {u"frame"_q, Rect(root->frame)},
		{u"viewport"_q, Rect(live.native.viewport.translated(origin))},
		{u"geometry"_q, Rect(live.native.geometry.translated(origin))},
		{u"scale"_q, root->scale}};
	auto cells = QJsonArray();
	if (facet == Facet::Text) {
		if (!t.painted || t.epoch != epoch || t.window != *root
			|| t.state.native.viewport != live.native.viewport
			|| t.state.native.geometry != live.native.geometry || t.state.view != live.view
			|| t.state.focused != live.focused || t.keyboard != r.widget->hasFocus()) { return {}; }
		for (auto i = 0; i != t.cells.count; ++i) { cells.append(Rect(t.cells.rectangles[i])); }
		a.insert(u"popup"_q, QJsonArray());
		a.insert(u"input"_q, -1);
		a.insert(u"enabled"_q, 0);
	} else {
		if (!_menu.registered || _menu.phase != Ui::PopupMenu::AnimatePhase::Shown
			|| _menu.receiver != index || _menu.slot != slot
			|| !_menu.widget || !_menu.action || _menu.widget->parentWidget() != r.widget
			|| _menu.item != t.identity
			|| _menu.document != t.document || _menu.id != t.id
			|| _menu.reason < 0 || _menu.reason > 1
			|| (_menu.reason == 1 && (focus != 1 || !_menu.originKeyboard))) { return {}; }
		const auto popup = ResolveNativeWindow(_menu.widget);
		const auto actions = _menu.widget->menu()->actions();
		if (!popup || popup->number == root->number || actions.size() > 32
			|| std::find(actions.begin(), actions.end(), _menu.action.data()) == actions.end()) { return {}; }
		const auto actionView = _menu.widget->menu()->itemForAction(_menu.action.data());
		if (!actionView || !actionView->isVisible() || !actionView->isEnabled()) { return {}; }
		const auto frame = QRect(actionView->mapToGlobal(QPoint()), actionView->size());
		if (!popup->frame.contains(frame)) { return {}; }
		cells.append(Rect(frame));
		a.insert(u"popup"_q, QJsonArray{int(popup->number), Rect(popup->frame), int(root->number)});
		a.insert(u"input"_q, _menu.reason);
		a.insert(u"enabled"_q, int(_menu.action->isEnabled()));
	}
	if (cells.empty()) { return {}; }
	a.insert(u"cells"_q, cells);
	return a;
}

void Observer::importAction(Ui::PopupMenu *menu, Window::SessionController *c,
		HistoryItem *item, DocumentData *document, QAction *action) {
	_menu = {};
	if (!menu || !c || !item || !document || !action) { return; }
	_menu.widget = menu;
	_menu.action = action;
	_menu.controller = base::make_weak(c);
	_menu.id = item->fullId();
	_menu.item = item;
	_menu.document = document;
}

void Observer::registerMenu(QWidget *receiver, Ui::PopupMenu *menu, int reason, bool blocked) {
	if (blocked || _menu.widget != menu || !_menu.controller
		|| reason < 0 || reason > 1) { _menu = {}; return; }
	for (auto i = 0; i != receiverCount; ++i) {
		auto &r = receivers[i];
		if (r.widget != receiver || r.controller != _menu.controller) { continue; }
		for (auto j = 0; j != 2; ++j) {
			auto &t = r.targets[j];
			if (t.identity != _menu.item || t.document != _menu.document
				|| t.id != _menu.id || !resolve(i, t, j).native.ready) { continue; }
			if (_menu.registered) { _menu = {}; return; }
			_menu.receiver = i;
			_menu.slot = j;
			_menu.reason = reason;
			_menu.originKeyboard = receiver->hasFocus()
				&& r.lookup(t.identity).focused == t.identity;
			_menu.registered = true;
		}
	}
	if (!_menu.registered) { _menu = {}; return; }
	menu->installEventFilter(this);
	menu->animatePhaseValue() | rpl::on_next([=](Ui::PopupMenu::AnimatePhase phase) {
		_menu.phase = phase;
		dirty();
	}, _menu.lifetime);
	connect(menu, &QObject::destroyed, this, [=] { _menu = {}; dirty(); });
	dirty();
}

bool Observer::eventFilter(QObject *object, QEvent *event) {
	switch (event->type()) {
	case QEvent::FocusIn: case QEvent::FocusOut: case QEvent::KeyPress:
	case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseMove:
	case QEvent::Wheel: case QEvent::Resize: case QEvent::Move: case QEvent::Hide:
	case QEvent::ParentChange: case QEvent::WinIdChange: case QEvent::LanguageChange:
	case QEvent::ScreenChangeInternal: case QEvent::StyleChange: case QEvent::PaletteChange:
	case QEvent::WindowActivate: case QEvent::WindowDeactivate:
		dirty(); break;
	default: break;
	}
	return QObject::eventFilter(object, event);
}

void Observer::reply(const Request &r, QJsonObject authority,
		bool valid, int code, qint64 deadline) {
	auto value = QJsonObject{{u"schema"_q, 1}, {u"type"_q, 2},
		{u"header"_q, RequestHeader(r)}, {u"status"_q, valid ? 1 : 0},
		{u"code"_q, code}, {u"remaining"_q, int(std::max(qint64(0), deadline - now()))}};
	value.insert(u"authority"_q, authority);
	value.insert(u"receipt"_q, valid ? Token(AuthoritySeal(authority)) : QString(64, QChar('0')));
	if (!_outbox.queue(value, deadline)) { stop(); return; }
	write();
}

void Observer::dispatch(const Request &r) {
	if (_stopped || r.run != _run || now() >= kSessionMs || !_outbox.empty()
		|| _nonceCount == _nonces.size()
		|| std::find(_nonces.begin(), _nonces.begin() + _nonceCount, r.nonce)
			!= _nonces.begin() + _nonceCount) { stop(); return; }
	_nonces[_nonceCount++] = r.nonce;
	if (r.operation == Operation::Close) { stop(); return; }
	auto index = -1;
	for (auto i = 0; i != receiverCount; ++i) {
		const auto &v = receivers[i];
		if (!v.widget || v.kind != r.kind) { continue; }
		if (r.receiver != QByteArray(64, '0') && r.receiver != v.token) { continue; }
		if (index != -1) { reply(r, {}, false, 2, now() + 150); return; }
		index = i;
	}
	if (index < 0) { reply(r, {}, false, 2, now() + 150); return; }
	const auto authority = snapshot(index, r.slot, r.facet);
	if (r.operation == Operation::Observe) {
		if (!_lease.begin(r, now())) { stop(); return; }
		if (authority.empty()) { reply(r, {}, false, 3, _lease.deadline()); return; }
		_lease.record(authority);
		reply(r, authority, true, 0, _lease.deadline());
	} else {
		const auto valid = _lease.validate(r, authority, now());
		const auto deadline = std::min(_lease.deadline(), now() + kValidationMs);
		if (deadline <= now()) { stop(); return; }
		reply(r, valid ? authority : QJsonObject(), valid, valid ? 0 : 4, deadline);
	}
}

void Observer::read() {
#ifdef Q_OS_MAC
	auto buffer = std::array<char, kRequestLimit + 4>();
	const auto count = ::read(_fd, buffer.data(), buffer.size());
	if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
	if (count <= 0 || !_reader.feed(QByteArrayView(buffer.data(), count))) { stop(); return; }
	if (_partialStart < 0) { _partialStart = now(); }
	if (const auto request = _reader.take()) {
		_partialStart = -1;
		dispatch(*request);
	} else if (_reader.failed()) { stop(); }
#endif
}

void Observer::write() {
#ifdef Q_OS_MAC
	if (_stopped) { return; }
	const auto bytes = _outbox.pending(now());
	if (_outbox.failed()) { stop(); return; }
	if (bytes.empty()) { _write->setEnabled(false); return; }
	if (now() >= kSessionMs) { stop(); return; }
	const auto count = ::send(_fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
	if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { _write->setEnabled(true); return; }
	if (count <= 0 || !_outbox.consumed(count, now())) { stop(); return; }
	_write->setEnabled(!_outbox.empty());
#endif
}

}

struct UiWitnessPaintState {
	QPointer<Observer> observer;
	int receiver = -1;
};

struct UiWitnessTextState {
	QPointer<Observer> observer;
	QPointer<QWidget> receiver;
	int index = -1;
	int slot = -1;
	quint64 epoch = 0;
	QPainterPath path;
	Ui::Text::HighlightInfoRequest request;
	QTransform transform;
	QRect clip;
};

UiWitnessPaint::UiWitnessPaint() = default;
UiWitnessPaint::UiWitnessPaint(std::unique_ptr<UiWitnessPaintState> s) : _state(std::move(s)) {}
UiWitnessPaint::UiWitnessPaint(UiWitnessPaint&&) noexcept = default;
UiWitnessPaint::~UiWitnessPaint() {
	if (_state && _state->observer) { _state->observer->endPaint(_state->receiver); }
}
UiWitnessText::UiWitnessText() = default;
UiWitnessText::UiWitnessText(std::unique_ptr<UiWitnessTextState> s) : _state(std::move(s)) {}
UiWitnessText::UiWitnessText(UiWitnessText&&) noexcept = default;
Ui::Text::HighlightInfoRequest *UiWitnessText::highlight() {
	return _state ? &_state->request : nullptr;
}
UiWitnessText::~UiWitnessText() {
	if (!_state || !_state->observer || !_state->receiver) { return; }
	auto &s = *_state;
	auto &o = *s.observer;
	auto &r = o.receivers[s.index];
	auto &t = r.targets[s.slot];
	const auto cells = UiWitness::RectangularCells(s.path, s.transform, s.clip);
	const auto native = UiWitness::ResolveNativeWindow(s.receiver);
	const auto live = o.resolve(s.index, t, s.slot);
	if (!cells || !native || !live.native.ready || o.epoch != s.epoch
		|| t.seen != 1 || !r.completePaint || !r.paintClip.contains(live.native.viewport)) { return; }
	t.cells = *cells;
	for (auto i = 0; i != t.cells.count; ++i) { t.cells.rectangles[i].translate(native->origin); }
	t.state = live;
	t.keyboard = r.widget->hasFocus();
	t.window = *native;
	t.epoch = s.epoch;
	t.painted = true;
}

UiWitnessPaint ObserveUiWitnessPaint(QWidget *w, Window::SessionController *c,
		UiWitness::ReceiverKind kind, const QRegion &region, UiWitnessLookup lookup) {
	if (!Current || Current->painting >= 0) { return {}; }
	const auto index = Current->registerReceiver(w, c, kind, std::move(lookup));
	if (index < 0) { return {}; }
	Current->beginPaint(index, region);
	auto state = std::make_unique<UiWitnessPaintState>();
	state->observer = Current;
	state->receiver = index;
	return UiWitnessPaint(std::move(state));
}

UiWitnessText ObserveUiWitnessText(QPainter &p, HistoryItem *container,
		HistoryItem *owner, const Ui::Text::String &text, bool caption,
		QRect bounds, bool blocked) {
	if (!Current || Current->painting < 0 || blocked) { return {}; }
	const auto index = Current->painting;
	auto &r = Current->receivers[index];
	const auto slot = Current->admit(index, container, owner, text, caption);
	if (!slot || !r.widget || !r.completePaint || p.device() != r.widget.data()
		|| p.hasClipping() || p.viewTransformEnabled()
		|| !UiWitness::IsAxisTranslation(p.worldTransform())) { return {}; }
	const auto live = Current->resolve(index, r.targets[*slot], *slot);
	if (!live.native.ready || !r.paintClip.contains(live.native.viewport)) { return {}; }
	const auto clip = r.paintClip.intersected(live.native.viewport).intersected(live.native.geometry);
	const auto mapped = p.worldTransform().mapRect(bounds);
	if (!clip.contains(mapped)) { return {}; }
	auto state = std::make_unique<UiWitnessTextState>();
	state->observer = Current;
	state->receiver = r.widget;
	state->index = index;
	state->slot = *slot;
	state->epoch = Current->epoch;
	state->transform = p.worldTransform();
	state->clip = clip;
	state->request = Ui::Text::HighlightInfoRequest{.range = {0, 1}, .outPath = &state->path};
	return UiWitnessText(std::move(state));
}

bool UiWitnessEnabled() { return Current && Current->active(); }

void InvalidateUiWitness(QWidget *receiver) {
	if (!Current) { return; }
	for (auto i = 0; i != Current->receiverCount; ++i) {
		if (Current->receivers[i].widget == receiver) { Current->dirty(1U << i); }
	}
}

void RecordUiWitnessImportAction(Ui::PopupMenu *menu, Window::SessionController *c,
		HistoryItem *item, DocumentData *document, QAction *action) {
	if (Current) { Current->importAction(menu, c, item, document, action); }
}
void RegisterUiWitnessMenu(QWidget *receiver, Ui::PopupMenu *menu, int reason, bool blocked) {
	if (Current) { Current->registerMenu(receiver, menu, reason, blocked); }
}

void StartUiWitness() {
#ifdef Q_OS_MAC
	if (Current) { return; }
	const auto fdText = qgetenv("PURPLE_UI_WITNESS_FD");
	const auto run = qgetenv("PURPLE_UI_WITNESS_RUN");
	if (fdText.isEmpty() && run.isEmpty()) { return; }
	auto ok = false;
	const auto inherited = fdText.toInt(&ok);
	if (!ok || inherited < 3 || QByteArray::number(inherited) != fdText
		|| !UiWitness::IsToken(run)) {
		return;
	}
	struct stat info = {};
	auto own = sockaddr_un();
	auto peer = sockaddr_un();
	auto ownSize = socklen_t(sizeof(own));
	auto peerSize = socklen_t(sizeof(peer));
	auto type = 0;
	auto typeSize = socklen_t(sizeof(type));
	auto uid = uid_t(0);
	auto gid = gid_t(0);
	if (::fstat(inherited, &info) || !S_ISSOCK(info.st_mode) || info.st_uid != ::geteuid()
		|| ::getsockname(inherited, reinterpret_cast<sockaddr*>(&own), &ownSize)
		|| ::getpeername(inherited, reinterpret_cast<sockaddr*>(&peer), &peerSize)
		|| own.sun_family != AF_UNIX || peer.sun_family != AF_UNIX
		|| ::getsockopt(inherited, SOL_SOCKET, SO_TYPE, &type, &typeSize) || type != SOCK_STREAM
		|| ::getpeereid(inherited, &uid, &gid) || uid != ::geteuid()) { return; }
	const auto fd = ::fcntl(inherited, F_DUPFD_CLOEXEC, 3);
	if (fd < 0) { return; }
	const auto noSignal = 1;
	const auto flags = ::fcntl(fd, F_GETFL);
	if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK)
		|| ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal))) { ::close(fd); return; }
	::close(inherited);
	const auto observer = new Observer(fd, run, QCoreApplication::instance());
	if (!observer->active()) { delete observer; return; }
	Current = observer;
	if (Current) {
		QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
			Current, [=] { if (Current) { Current->stop(); } });
	}
#endif
}

#ifndef Q_OS_MAC
namespace UiWitness {
std::optional<NativeWindow> ResolveNativeWindow(QWidget*) { return std::nullopt; }
}
#endif

}
