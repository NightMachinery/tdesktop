#include "purple/purple_settings_customize.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/data_channel.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_keys.h"
#include "purple/purple_list_filter_keys.h"
#include "purple/purple_picker.h"
#include "purple/purple_privacy_screen.h"
#include "purple/purple_settings_collection_ui.h"
#include "purple/purple_settings_confirm_gesture.h"
#include "purple/purple_settings_customize_look.h"
#include "purple/purple_settings_editor.h"
#include "purple/purple_settings_focus.h"
#include "purple/purple_settings_review_display.h"
#include "purple/purple_settings_draft.h"
#include "purple/purple_settings_structures.h"
#include "purple/purple_settings_tree.h"
#include "styles/style_keys_legend.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "ui/layers/layer_widget.h"
#include "ui/style/style_core.h"
#include "ui/userpic_view.h"
#include "window/window_controller.h"
#include "window/window_lock_widgets.h"
#include "window/window_separate_id.h"

#include <QtCore/QPointer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QVBoxLayout>

#include <tuple>
#include <utility>

namespace Purple {
namespace {

using SettingsTree::Row;
using SettingsTree::EditorKey;
using Status = SettingsEditorStatus;
using PeerField = SettingsStructures::PeerField;

#ifdef Q_OS_MAC
constexpr auto kCtrlMod = Qt::MetaModifier;
#else
constexpr auto kCtrlMod = Qt::ControlModifier;
#endif // Q_OS_MAC

QString Isolate(const QString &text) {
	return u'\u2066' + text + u'\u2069';
}

QString AccountName(const AccountKey &key) {
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto live = entry.account->maybeSession();
		if (live && !entry.account->loggingOut() && SessionAccountKey(live) == key) {
			return live->user()->name();
		}
	}
	return u"Signed-out account"_q;
}

bool Protected(Window::Controller *window) {
	return !window || window->locked() || Core::App().screenIsLocked()
		|| PrivacyScreenActiveAny() || PrivacyScreenActive(window->widget());
}

Main::Session *LiveSessionFor(const AccountKey &key) {
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (session && !entry.account->loggingOut() && !session->termsLocked()
			&& SessionAccountKey(session) == key) return session;
	}
	return nullptr;
}

std::optional<PeerField> PeerFieldFor(SettingId id) {
	switch (id) {
	case SettingId::Members: return PeerField::Members;
	case SettingId::PresetPins:
	case SettingId::ViewPins: return PeerField::Pins;
	case SettingId::PreviewAlways: return PeerField::Notifications;
	case SettingId::MediaPeers: return PeerField::Media;
	case SettingId::BudgetTarget: return PeerField::Budget;
	default: return std::nullopt;
	}
}

QString PeerLiteral(not_null<PeerData*> peer, PeerField field) {
	const auto id = IdOf(peer);
	if (field == PeerField::Budget) return u"chat:%1"_q.arg(id);
	if (field == PeerField::Members || field == PeerField::Pins) return QString::number(id);
	const auto user = peer->asUser();
	const auto prefix = field == PeerField::Media
		? (user ? u"user:"_q : peer->isChat() ? u"chat:"_q : u"channel:"_q)
		: (user ? (user->isBot() ? u"bot:"_q : u"private:"_q)
			: peer->isBroadcast() ? u"channel:"_q : u"group:"_q);
	return prefix + QString::number(id);
}

QIcon PeerAvatar(not_null<PeerData*> peer) {
	const auto size = st::normalFont->height * 2;
	auto view = Ui::PeerUserpicView();
	auto pixmap = QPixmap(size, size);
	pixmap.fill(Qt::transparent);
	auto painter = QPainter(&pixmap);
	peer->paintUserpic(painter, view, 0, 0, size, true);
	painter.end();
	return QIcon(pixmap);
}

std::vector<SettingsPeerChoice> PeerCatalogue(Main::Session *session, PeerField field) {
	auto result = std::vector<SettingsPeerChoice>();
	if (field == PeerField::Notifications) {
		result.push_back({ u"MAGIC_BOTS"_q, u"All bots"_q });
		result.push_back({ u"MAGIC_CHANNELS"_q, u"All channels"_q });
	}
	if (!session) return result;
	auto counts = QHash<QString, int>();
	auto seen = QSet<uint64>();
	const auto append = [&](not_null<PeerData*> peer) {
		if (seen.contains(peer->id.value)) return;
		seen.insert(peer->id.value);
		const auto literal = PeerLiteral(peer, field);
		++counts[literal];
		if (peer->migrateTo() || !PickerVisible(peer)) return;
		const auto kind = peer->isBroadcast() ? u"Channel"_q : peer->isUser()
			? (peer->asUser()->isBot() ? u"Bot"_q : u"Person"_q) : u"Group"_q;
		result.push_back({ literal, peer->name() + u" · "_q + kind, PeerAvatar(peer),
			[weak = base::make_weak(session), id = peer->id] {
				const auto live = weak.get();
				const auto current = live && !live->account().loggingOut() ? live->data().peerLoaded(id) : nullptr;
				return current && !current->migrateTo() && PickerVisible(current);
			} });
	};
	session->data().enumerateUsers([&](not_null<UserData*> peer) { append(peer.get()); });
	session->data().enumerateGroups(append);
	session->data().enumerateBroadcasts([&](not_null<ChannelData*> peer) { append(peer.get()); });
	std::erase_if(result, [&](const auto &peer) { return counts.value(peer.literal) > 1; });
	return result;
}

void AnnounceError(QLabel *label, const QString &message) {
	if (label->text() == message) return;
	label->setText(message);
	auto event = QAccessibleEvent(label, QAccessible::Alert);
	QAccessible::updateAccessibility(&event);
}

QString PreviewPeerLiteral(const PreviewPeer &peer) {
	const auto prefix = peer.kind == ChatKind::Bot ? u"bot:"_q : peer.kind == ChatKind::Group
		? u"group:"_q : peer.kind == ChatKind::Channel ? u"channel:"_q : u"private:"_q;
	return prefix + QString::number(peer.id);
}

QLabel *Label(QWidget *parent, const QString &name = {}) {
	const auto label = new QLabel(name, parent);
	label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	return label;
}

QLabel *HelpLabel(QWidget *parent, const QString &text) {
	const auto label = Label(parent, text);
	label->setProperty("secondary", true);
	return label;
}

QPushButton *Button(QWidget *parent, const QString &text, QString key = {}) {
	return new CustomizeLook::Action(text, parent, std::move(key));
}

bool IsHotkey(const SettingDescriptor &descriptor) {
	return descriptor.id == SettingId::PeekHotkey || descriptor.id == SettingId::PresetHotkey;
}

bool Consequential(const SettingsEditorPreview &preview) {
	if (preview.impact.dangerous || preview.incompleteTrace || !preview.introducedWarnings.empty()
		|| preview.route == SettingsEditorRoute::MediaReply || preview.route == SettingsEditorRoute::Members) {
		return true;
	}
	return std::any_of(preview.changedAddresses.begin(), preview.changedAddresses.end(), [](const auto &address) {
		const auto descriptor = FindSettingDescriptor(address);
		if (!descriptor) return false;
		switch (descriptor->id) {
		case SettingId::Version:
		case SettingId::PreviewAlways:
		case SettingId::SendAfterSave:
		case SettingId::PrivateListening:
		case SettingId::Trade:
		case SettingId::TradeHold:
		case SettingId::HideEverywhere:
		case SettingId::KeepDiscarded:
		case SettingId::BinKeep:
		case SettingId::ScreenTimeEnabled:
		case SettingId::RetentionDays:
		case SettingId::Budgets:
		case SettingId::BudgetMode:
		case SettingId::BudgetTarget:
		case SettingId::PerDay: return true;
		default: return false;
		}
	});
}

QString FieldText(const Row &row) {
	if (!row.effective.isValid()) return {};
	switch (row.descriptor->codec) {
	case SettingCodec::Time: return SettingsTree::ValueText(row);
	case SettingCodec::Duration: return u"%1s"_q.arg(row.effective.toInt());
	default: return row.effective.toString();
	}
}

QString StatusText(const SettingsEditorPreparation &result) {
	if (!result.error.isEmpty()) return result.error;
	switch (result.status) {
	case Status::StaleRevision: return u"The account or settings file changed. The draft is kept for comparison."_q;
	case Status::IncompleteTrace: return u"Staged. Some effective reads could not be verified."_q;
	case Status::Unsupported: return u"This setting or combination has no guarded edit route."_q;
	case Status::InvalidSource: return u"The settings file needs repair."_q;
	case Status::InvalidValue: return u"The setting does not accept this value."_q;
	case Status::Unavailable: return u"The selected destination is unavailable."_q;
	default: return {};
	}
}

class CustomizeLayer;
QHash<::MainWindow*, QPointer<CustomizeLayer>> OpenEditors;
QHash<::MainWindow*, QHash<QString, QString>> LastSelection;

class OpenCustomizeOnActivation final : public QObject {
public:
	OpenCustomizeOnActivation(not_null<Window::Controller*> window, AccountKey account)
	: QObject(window->widget().get())
	, _window(base::make_weak(window.get()))
	, _account(account) {
		window->widget()->installEventFilter(this);
		QTimer::singleShot(0, this, [this] { check(); });
		QTimer::singleShot(2000, this, &QObject::deleteLater);
	}

private:
	bool eventFilter(QObject *, QEvent *event) override {
		if (event->type() == QEvent::WindowActivate) QTimer::singleShot(0, this, [this] { check(); });
		return false;
	}

	void check() {
		const auto window = _window.get();
		if (!window || _done || Core::App().activeWindow() != window) return;
		_done = true;
		const auto session = window->maybeSession();
		if (session && SessionAccountKey(session) == _account) ShowCustomizeSettings(window);
		deleteLater();
	}

	base::weak_ptr<Window::Controller> _window;
	AccountKey _account;
	bool _done = false;
};

class CustomizeLayer final : public Ui::RpWidget {
public:
	explicit CustomizeLayer(not_null<Window::Controller*> host)
	: RpWidget(host->widget()->bodyWidget())
	, _host(base::make_weak(host.get()))
	, _session(base::make_weak(host->maybeSession()))
	, _window(host->widget().get())
	, _pages(new QStackedWidget(this))
	, _browser(new CustomizeLook::Canvas(_pages))
	, _sheetPage(new QScrollArea(_pages))
	, _sheet(new CustomizeLook::Canvas(_sheetPage))
	, _covered(new CustomizeLook::Canvas(_pages))
	, _coverMessage(Label(_covered, u"Settings hidden while locked"_q))
	, _coverClose(Button(_covered, u"Close"_q))
	, _title(Label(_browser, u"Customize Purple settings"_q))
	, _subject(new CustomizeLook::Chip(_browser))
	, _destination(new CustomizeLook::Chip(_browser))
	, _accountsButton(Button(_browser, u"Account"_q, ModalKeysEnabled() ? u"Shift+A"_q : QString()))
	, _filter(new QLineEdit(_browser))
	, _clear(Button(_browser, u"Clear filter"_q))
	, _tree(new CustomizeLook::Tree(_browser))
	, _inspector(Label(_browser))
	, _feedback(Label(_browser))
	, _edit(Button(_browser, u"Edit"_q, u"RET"_q))
	, _reset(Button(_browser, u"Reset"_q, ModalKeysEnabled() ? u"r"_q : QString()))
	, _undo(Button(_browser, u"Undo"_q, ModalKeysEnabled() ? u"u"_q : QString()))
	, _reviewButton(Button(_browser, u"Review"_q, ModalKeysEnabled() ? u"d"_q : QString()))
	, _help(Button(_browser, u"Help"_q, ModalKeysEnabled() ? u"?"_q : QString()))
	, _sheetLayout(new QVBoxLayout(_sheet)) {
		setAccessibleName(u"Customize Purple settings"_q);
		setFont(st::normalFont);
		const auto gap = fontMetrics().height();
		_sheetLayout->setSpacing(gap / 3);
		_sheetLayout->setContentsMargins(gap, gap, gap, gap);
		setFocusPolicy(Qt::StrongFocus);
		auto outer = new QVBoxLayout(this);
		outer->setContentsMargins({});
		outer->addWidget(_pages);
		_sheetPage->setViewport(new CustomizeLook::Canvas(_sheetPage));
		_sheetPage->setWidgetResizable(true);
		_sheetPage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		_sheetPage->setFrameShape(QFrame::NoFrame);
		_sheetPage->setWidget(_sheet);
		for (const auto page : { _browser, static_cast<QWidget*>(_sheetPage), _covered }) {
			_pages->addWidget(page);
		}
		const auto cover = new QVBoxLayout(_covered);
		cover->addStretch();
		_coverMessage->setAlignment(Qt::AlignCenter);
		cover->addWidget(_coverMessage);
		cover->addWidget(_coverClose, 0, Qt::AlignCenter);
		cover->addStretch();
		_coverClose->hide();
		_coverClose->installEventFilter(this);
		connect(_coverClose, &QPushButton::clicked, this, [this] { back(); });
		setupBrowser();
		connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
			if (!now) return;
			if (_browser->isAncestorOf(now)) {
				_browserFocus = now;
			} else if (!_formExit && (now == _line || now == _multi || now == _choice
				|| (_bool && qobject_cast<QRadioButton*>(now) && _sheet->isAncestorOf(now)))) {
				_lastField = now;
			}
		});

		connect(_filter, &QLineEdit::textChanged, this, [this] {
			_anchor.clear();
			renderTree();
		});
		connect(_clear, &QPushButton::clicked, this, [this] {
			_filter->clear();
			_tree->setFocus();
		});
		connect(_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
			const auto key = item ? item->data(0, Qt::UserRole).toString() : QString();
			if (!key.isEmpty()) {
				_selection = key;
				LastSelection[_window.data()][_accountId] = key;
				if (!_anchor.isEmpty() && _anchor != key) {
					_anchor.clear();
					QTimer::singleShot(0, this, [this] { renderTree(); });
				}
			}
			refreshInspector();
		});
		connect(_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
			if (!filtering()) _expanded.insert(item->data(0, Qt::UserRole).toString());
		});
		connect(_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
			if (!filtering()) _expanded.remove(item->data(0, Qt::UserRole).toString());
		});
		connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) {
			activateRow();
		});
		connect(_edit, &QPushButton::clicked, this, [this] { activateRow(); });
		connect(_reset, &QPushButton::clicked, this, [this] { resetSelected(); });
		connect(_undo, &QPushButton::clicked, this, [this] { undo(); });
		connect(_accountsButton, &QPushButton::clicked, this, [this] { showAccounts(); });
		connect(_reviewButton, &QPushButton::clicked, this, [this] { showReview(); });
		connect(_help, &QPushButton::clicked, this, [this] { showHelp(); });

		host->widget()->installEventFilter(this);
		host->sessionControllerValue() | rpl::skip(1) | rpl::on_next([this] {
			if (!sameSession()) retire();
		}, lifetime());
		if (const auto session = _session.get()) {
			session->termsLockChanges() | rpl::on_next([this] { protectionChanged(); }, lifetime());
			session->data().sessionDataAboutToBeCleared() | rpl::on_next([this] {
				retire();
			}, lifetime());
		}
		rpl::merge(Core::App().passcodeLockChanges(),
			Core::App().screenIsLockedValue() | rpl::skip(1),
			PrivacyScreenChanges()) | rpl::on_next([this] { protectionChanged(); }, lifetime());
		SettingsChanges() | rpl::on_next([this] {
			QTimer::singleShot(0, this, [this] { refreshFromDisk(); });
		}, lifetime());
		style::PaletteChanged() | rpl::on_next([this] { applyPalette(); }, lifetime());
		applyPalette();
	}

	void open() {
		if (!capture()) return;
		_selection = LastSelection.value(_window.data()).value(_accountId);
		rebuild(true);
		showRecoveryIfNeeded();
	}

	void present(anim::type animated);

	void focusPage() {
		if (isVisible()) {
			if (_pages->currentWidget() == _browser) _tree->setFocus();
			else if (_resumeRefused && visibleContent()) _coverClose->setFocus();
		} else if (!_presentation && visibleContent()) {
			protectionChanged();
		}
	}

	bool privacyKey(QWidget *focus, const QKeyEvent &key) {
		const auto text = qobject_cast<QTextEdit*>(focus);
		if (!visibleContent() || _capture || QApplication::activePopupWidget()
			|| qobject_cast<QLineEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus)
			|| (text && !text->isReadOnly())) {
			_privacyPrefix.invalidate();
			return false;
		}
		const auto plain = !(key.modifiers() & ~Qt::KeypadModifier);
		const auto prefix = _privacyPrefix.isValid() && _privacyPrefix.elapsed() < 1000;
		_privacyPrefix.invalidate();
		if (plain && (key.key() == Qt::Key_F8 || (prefix && key.key() == Qt::Key_Z))) {
			if (!key.isAutoRepeat()) TogglePrivacyScreen(_window.data());
			return true;
		}
		if (plain && key.key() == Qt::Key_Space && focus == _tree && ModalKeysEnabled()) {
			if (!key.isAutoRepeat()) _privacyPrefix.start();
			return true;
		}
		return false;
	}

protected:
	void keyPressEvent(QKeyEvent *event) override {
		if (handleKey(this, event)) return;
		event->accept();
	}

	bool eventFilter(QObject *object, QEvent *event) override {
		if (object == _window.data()) {
			if (event->type() == QEvent::Close) {
				retire();
			} else if (_needsRefresh && event->type() == QEvent::WindowActivate) {
				QTimer::singleShot(0, this, [this] { protectionChanged(); });
			}
			return false;
		}
		if (event->type() == QEvent::InputMethod) {
			const auto input = static_cast<QInputMethodEvent*>(event);
			if (input->preeditString().isEmpty()) _preedit.remove(object);
			else _preedit.insert(object);
		} else if (event->type() == QEvent::FocusOut) {
			_preedit.remove(object);
		} else if (event->type() == QEvent::MouseButtonRelease && _confirm && object == _confirm) {
			const auto mouse = static_cast<QMouseEvent*>(event);
			if (visibleContent() && mouse->button() == Qt::LeftButton && _confirm->isDown()
				&& _confirm->rect().contains(mouse->position().toPoint())) {
				SettingsConfirmGesture::Guard(_window, *mouse);
			}
		} else if (event->type() == QEvent::ShortcutOverride && visibleContent()) {
			event->accept();
		} else if (event->type() == QEvent::KeyPress) {
			return handleKey(object, static_cast<QKeyEvent*>(event));
		}
		return false;
	}

private:
	friend class CustomizePresentation;

	void closeLayer() {
		if (const auto layer = _presentation.data()) layer->closeLayer();
		else deleteLater();
	}

	void setupBrowser() {
		_title->setFont(st::keysLegendHeaderStyle.font);
		auto layout = new QVBoxLayout(_browser);
		const auto gap = fontMetrics().height();
		layout->setContentsMargins(gap, gap, gap, gap);
		layout->setSpacing(gap / 2);
		auto header = new QHBoxLayout();
		header->addWidget(_title, 1);
		header->addWidget(_accountsButton);
		_accountsButton->installEventFilter(this);
		const auto close = Button(_browser, u"Close"_q);
		header->addWidget(close);
		connect(close, &QPushButton::clicked, this, [this] { back(); });
		layout->addLayout(header);
		auto scope = new QHBoxLayout();
		scope->addWidget(_subject, 1);
		scope->addWidget(_destination, 2);
		layout->addLayout(scope);

		auto filterRow = new QHBoxLayout();
		const auto updateFilterHint = [this](bool modal) {
			_filter->setPlaceholderText(modal ? u"i or / to filter settings"_q : u"Filter settings…"_q);
		};
		updateFilterHint(ModalKeysEnabled());
		ModalKeysEnabledChanges() | rpl::on_next(updateFilterHint, lifetime());
		ListFilter::Mark(_filter);
		_filter->setAccessibleName(u"Filter settings"_q);
		filterRow->addWidget(_filter, 1);
		filterRow->addWidget(_clear);
		layout->addLayout(filterRow);

		_tree->setHeaderHidden(true);
		_tree->setAccessibleName(u"Settings tree"_q);
		_tree->setWordWrap(true);
		_tree->setExpandsOnDoubleClick(false);
		_tree->setFont(st::normalFont);
		_tree->setIndentation(gap);
		layout->addWidget(_tree, 1);

		_inspector->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
		_inspector->setAccessibleName(u"Selected setting details"_q);
		_feedback->setAccessibleName(u"Draft status"_q);
		_feedback->setProperty("secondary", true);
		layout->addWidget(_inspector);
		layout->addWidget(_feedback);
		const auto stockMx = new QCheckBox(_browser);
		MxShortcutInStockModeLabel() | rpl::on_next([=](const QString &label) {
			stockMx->setText(label);
			stockMx->setAccessibleName(label);
		}, lifetime());
		MxShortcutInStockModeValue() | rpl::on_next([=](bool enabled) {
			const auto block = QSignalBlocker(stockMx);
			stockMx->setChecked(enabled);
		}, lifetime());
		connect(stockMx, &QCheckBox::toggled, this, [this, stockMx](bool enabled) {
			if (visibleContent()) {
				SetMxShortcutInStockMode(enabled);
			} else {
				const auto block = QSignalBlocker(stockMx);
				stockMx->setChecked(MxShortcutInStockMode());
			}
		});
		layout->addWidget(stockMx);
		const auto weather = Button(_browser, u"Privacy screen weather · this device"_q);
		weather->installEventFilter(this);
		connect(weather, &QPushButton::clicked, this, [this] {
			if (visibleContent() && _window) {
				if (const auto owner = _window->controller().sessionController()) {
					ShowPrivacyWeatherSettings(owner);
				}
			}
		});
		CustomizeLook::AppendAction(layout, weather);
		const auto footer = new CustomizeLook::Actions(_browser);
		for (const auto button : { _edit, _reset, _undo, _reviewButton, _help }) {
			footer->append(button);
			button->installEventFilter(this);
		}
		layout->addWidget(footer);
		for (const auto child : { static_cast<QWidget*>(_filter), static_cast<QWidget*>(_tree),
			static_cast<QWidget*>(_inspector), static_cast<QWidget*>(_clear), static_cast<QWidget*>(close) }) {
			child->installEventFilter(this);
		}
	}

	void applyPalette() {
		CustomizeLook::Apply(this, st::windowBg->c, st::windowBgOver->c, st::windowFg->c,
			st::windowSubTextFg->c, st::windowActiveTextFg->c, st::boxDividerBg->c);
		if (_reviewBody && _reviewEffectsShown && visibleContent()) {
			if (const auto frame = _frames.current()) _reviewBody->setHtml(reviewEffects(frame->review->preview()));
		}
	}

	bool sameSession() const {
		const auto host = _host.get();
		const auto live = _session.get();
		return !_retired && host && live && host->maybeSession() == live
			&& live->account().maybeSession() == live && !live->account().loggingOut();
	}

	bool visibleContent() const {
		return sameSession() && !Protected(_host.get());
	}

	void retire() {
		if (_retired) return;
		_retired = true;
		hide();
		_pages->setCurrentWidget(_covered);
		clearSheet();
		_frames.clear();
		_rows.clear();
		_tree->clear();
		_inspector->clear();
		_opening = {};
		QTimer::singleShot(0, this, [this] { closeLayer(); });
	}

	void protectionChanged() {
		if (!sameSession()) {
			retire();
		} else if (!visibleContent()) {
			QToolTip::hideText();
			_privacyPrefix.invalidate();
			_coverMessage->setText(u"Settings hidden while locked"_q);
			_coverClose->hide();
			for (const auto choice : _sheet->findChildren<QComboBox*>()) {
				choice->hidePopup();
			}
			_protectedForm.hide(_pages, _sheetPage, _covered, !_editingKey.isEmpty());
			if (!_protectedForm.held()) clearSheet();
			if (Core::App().passcodeLocked() && _presentation && _window) {
				// The stock lock destroys layers, so the hidden editor belongs to its window.
				hide();
				setParent(_window->bodyWidget());
				_presentation.clear();
			}
		} else {
			const auto wasCovered = _pages->currentWidget() == _covered;
			QTimer::singleShot(0, this, [this, wasCovered] {
				if (!visibleContent()) return;
				const auto weak = QPointer(this);
				refreshFromDisk();
				if (!weak || !visibleContent()) return;
				if (wasCovered && !_needsRefresh) {
					_resumeRefused = _draftStale || !_opening.current;
					if (!_presentation) present(anim::type::instant);
					if (!weak || !visibleContent()) return;
					if (_resumeRefused) {
						_coverMessage->setText(u"The source changed while settings were hidden. Close and reopen Customize to see the current settings."_q);
						_coverClose->setText(u"Discard draft and close"_q);
						_coverClose->show();
						_coverClose->setFocus();
					} else if (_applyInFlight) {
						displayApplying(u"Waiting for the reviewed settings operation."_q);
					} else if (!_protectedForm.restore(_pages, _sheetPage,
							SettingsDraft::CanRestoreForm(sameSession(), !_draftStale))) {
						_pages->setCurrentWidget(_browser);
						_tree->setFocus();
					}
				}
			});
		}
	}

	bool capture() {
		const auto weak = QPointer(this);
		if (const auto host = _host.get()) {
			auto opening = CaptureWindowSettingsEditor(host, [weak] {
				return weak && weak->visibleContent();
			});
			if (!weak) return false;
			_opening = std::move(opening);
		}
		if (_opening.current) {
			const auto key = _opening.requester;
			_accountId = AccountKeyText(key.environment, key.userId).value_or(QString());
		}
		_draftStale = false;
		return true;
	}

	void refreshFromDisk() {
		if (!visibleContent() || _applyInFlight) return;
		if (Core::App().activeWindow() != _host.get()) {
			_needsRefresh = true;
			return;
		}
		_needsRefresh = false;
		const auto weak = QPointer(this);
		const auto opening = _opening;
		const auto matches = WindowSettingsEditorOpeningMatches(opening);
		if (!weak || opening.origin != _opening.origin || !visibleContent()) return;
		if (_editingKey.isEmpty() && !_frames.current()) {
			if (!matches) {
				_frames.clear();
				if (capture()) rebuild();
			} else {
				_draftStale = false;
			}
			return;
		}
		_draftStale = !matches;
		if (_draftStale) {
			const auto formOpen = (_pages->currentWidget() == _sheetPage) && !_editingKey.isEmpty();
			AnnounceError(formOpen && _formStatus ? _formStatus : _feedback, formOpen
				? u"The settings source changed. Your input is kept until you leave this form."_q
				: u"The settings source changed. Review the staged draft against the current file."_q);
		}
	}

	QString sourceName(const std::optional<AccountSettingsOpenTarget> &source) const {
		return source ? AccountName(source->file.account) : u"Unavailable source"_q;
	}

	void rebuild(bool revealSelection = false) {
		if (!visibleContent() || !_opening.current) return;
		const auto frame = _frames.current();
		const auto layers = frame ? frame->review->preview().afterLayers : InspectSettingsEditor(_opening);
		const auto source = [&](const std::optional<AccountSettingsOpenTarget> &target,
				const SettingsFileInput &good, const SettingsLayerFileReport &report, bool writable) {
			auto result = SettingsTree::Source{
				.name = sourceName(target),
				.path = target ? target->file.path : QString(),
				.bytes = target ? target->file.input.bytes : QByteArray(),
				.writable = writable,
				.recovery = report.usedGood,
			};
			if (frame && frame->review->preview().path == result.path) {
				result.bytes = frame->review->preview().after;
			}
			if (report.usedGood && report.good) {
				result.path = report.good->path;
				result.bytes = good.bytes;
			}
			return result;
		};
		const auto same = _opening.ownSource && _opening.globalSource
			&& _opening.ownSource->file.path == _opening.globalSource->file.path;
		const auto own = source(_opening.ownSource, _opening.ownGood,
			same ? layers.global : layers.account, bool(_opening.own));
		const auto global = source(_opening.globalSource, _opening.globalGood,
			layers.global, bool(_opening.appWide));
		const auto files = std::vector<SettingsTree::Source>{ global, own };
		_rows = SettingsTree::Build(layers, files, [session = _session.get()](const QString &key) {
			using Kind = ArchiveV2::ChatClass;
			const auto token = ParsePeerToken(key);
			if (!session || !token) return Kind::Unknown;
			if (token->kind == PeerTokenKind::Chat) return Kind::Group;
			if (token->kind == PeerTokenKind::User) {
				const auto peer = session->data().peerLoaded(peerFromUser(UserId(token->id)));
				const auto user = peer ? peer->asUser() : nullptr;
				return !user ? Kind::Unknown : user->isBot() ? Kind::Bot : Kind::Direct;
			}
			if (token->kind == PeerTokenKind::Channel) {
				const auto peer = session->data().peerLoaded(peerFromChannel(ChannelId(token->id)));
				return !peer ? Kind::Unknown : peer->isMegagroup() ? Kind::Group : Kind::Channel;
			}
			return Kind::Unknown;
		});
		if (revealSelection) {
			auto key = _selection;
			while (!key.isEmpty()) {
				const auto row = std::find_if(_rows.begin(), _rows.end(), [&](const Row &item) {
					return item.key == key;
				});
				if (row == _rows.end()) break;
				key = row->parent;
				if (!key.isEmpty()) _expanded.insert(key);
			}
		}
		_subject->setDetails(u"%1 · App-wide from %2"_q.arg(
			AccountName(_opening.requester), sourceName(_opening.globalSource)));
		renderTree();
	}

	bool filtering() const {
		return !_filter->text().trimmed().isEmpty();
	}

	void renderTree() {
		if (!visibleContent()) return;
		const auto wasBlocked = QSignalBlocker(_tree);
		const auto oldScroll = _tree->verticalScrollBar()->value();
		_tree->clear();
		auto full = _expanded;
		if (!filtering()) {
			for (const auto &row : _rows) if (row.branch) full.insert(row.key);
		}
		const auto view = SettingsTree::View(_rows, full, _filter->text(), _anchor);
		auto items = QHash<QString, QTreeWidgetItem*>();
		for (const auto index : view.rows) {
			const auto &row = _rows[index];
			const auto parent = items.value(row.parent);
			const auto item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_tree);
			item->setData(0, Qt::UserRole, row.key);
			const auto value = SettingsTree::ValueText(row);
			const auto marked = changed(row);
			const auto state = (row.invalid ? u" · Invalid"_q : QString())
				+ (marked ? u" · Modified"_q : QString());
			item->setText(0, row.label + state + (!row.branch && !value.isEmpty()
				? u"\n"_q + Isolate(value) + (row.source.isEmpty() ? QString()
					: u"  ·  "_q + row.source) : QString()));
			item->setFont(0, row.branch ? QFont(st::semiboldFont) : QFont(st::normalFont));
			if (row.branch && (filtering() || _expanded.contains(row.key))) item->setExpanded(true);
			items[row.key] = item;
		}
		if (filtering() && !view.matches && view.rows.empty()) {
			const auto empty = new QTreeWidgetItem(_tree);
			empty->setText(0, u"No matching settings. Clear the filter to see all settings."_q);
			empty->setFlags(Qt::ItemIsEnabled);
		}
		auto selected = items.value(_selection);
		while (selected && selected->parent() && !selected->parent()->isExpanded()) {
			selected = selected->parent();
		}
		_tree->setCurrentItem(selected ? selected : _tree->topLevelItem(0));
		_tree->verticalScrollBar()->setValue(oldScroll);
		if (filtering() && _tree->currentItem()) _tree->scrollToItem(_tree->currentItem());
		refreshInspector();
	}

	const Row *selected() const {
		const auto item = _tree->currentItem();
		const auto key = item ? item->data(0, Qt::UserRole).toString() : QString();
		const auto found = std::find_if(_rows.begin(), _rows.end(), [&](const Row &row) {
			return row.key == key;
		});
		return found == _rows.end() ? nullptr : &*found;
	}

	bool changed(const Row &row) const {
		const auto frame = _frames.current();
		if (!frame) return false;
		const auto edits = std::get_if<std::vector<SettingsEdit>>(&frame->intent);
		if (edits) {
			return std::any_of(edits->begin(), edits->end(),
				[&](const SettingsEdit &edit) { return edit.address == row.address; });
		}
		const auto &members = std::get<SettingsEditorMembers>(frame->intent);
		return row.address == SettingAddress{ u"lists"_q, members.list, u"members"_q };
	}

	void refreshInspector() {
		const auto row = selected();
		const auto editLabel = (row && row->listCaseFix) ? u"Fix list spelling"_q : u"Edit"_q;
		_edit->setText(editLabel);
		_edit->setAccessibleName(editLabel);
		_edit->setToolTip(editLabel);
		_edit->setEnabled(row && (row->editable || isCollection(*row)));
		_reset->setEnabled(row && row->editable && SettingsTree::CanReset(*row->descriptor));
		_undo->setEnabled(_frames.canUndo());
		_reviewButton->setEnabled(_frames.current() && !_applyInFlight);
		_clear->setEnabled(filtering());
		const auto &source = row && row->scope == SettingScope::AppWide
			? _opening.globalSource : _opening.ownSource;
		_destination->setDetails(
			u"Destination"_q,
			source ? source->file.path : u"Unavailable"_q);
		if (!row) {
			_inspector->clear();
			return;
		}
		auto lines = QStringList();
		if (!row->address.empty()) lines.push_back(Isolate(SettingsTree::Address(row->address)));
		if (!row->listCaseFix && row->descriptor && SettingsTree::IsScalar(row->descriptor->codec)) {
			lines.push_back(u"%1 · %2 · Destination %3"_q.arg(row->source,
				SettingsTree::ScopeText(row->scope), row->destination));
			lines.push_back(u"Effective %1 · Default %2"_q.arg(Isolate(SettingsTree::ValueText(*row)),
				Isolate(row->defaultValue)));
		}
		for (const auto &physical : row->physical) {
			auto raw = physical.raw;
			if (raw.size() > 110) raw = raw.left(109) + u'…';
			lines.push_back(u"%1 · %2, line %3  %4"_q.arg(physical.file,
				Isolate(QFileInfo(physical.path).fileName())).arg(physical.line).arg(Isolate(raw)));
		}
		if (!row->help.isEmpty()) lines.push_back(row->help);
		if (row->descriptor && !SettingsTree::CanReset(*row->descriptor)) lines.push_back(u"This field is required."_q);
		if (!row->editable && row->descriptor && SettingsTree::IsScalar(row->descriptor->codec)) {
			const auto &reason = row->scope == SettingScope::AppWide ? _opening.appWideError : _opening.ownError;
			lines.push_back(reason.isEmpty() ? u"Read-only in this account context."_q : reason);
		}
		_inspector->setText(lines.join(u'\n'));
	}

	void selectSibling(int delta) {
		if (!_anchor.isEmpty()) {
			const auto previous = selected();
			if (previous) _selection = previous->key;
			_anchor.clear();
			renderTree();
		}
		auto item = _tree->currentItem();
		for (auto i = 0; item && i != std::abs(delta); ++i) {
			const auto next = delta > 0 ? _tree->itemBelow(item) : _tree->itemAbove(item);
			if (next) item = next;
		}
		if (item) {
			_tree->setCurrentItem(item);
			_tree->scrollToItem(item);
		}
	}

	void traverse(bool inward) {
		const auto item = _tree->currentItem();
		if (!item) return;
		if (inward) {
			if (item->childCount() && !item->isExpanded()) item->setExpanded(true);
			else if (item->childCount()) _tree->setCurrentItem(item->child(0));
			else _inspector->setFocus();
		} else if (item->isExpanded()) {
			item->setExpanded(false);
		} else if (item->parent()) {
			_tree->setCurrentItem(item->parent());
		}
	}

	void changePanel(bool reverse) {
		const auto focus = QApplication::focusWidget();
		const auto index = focus == _tree || focus == _filter ? 0 : (focus == _inspector ? 1 : 2);
		const auto next = (index + (reverse ? 2 : 1)) % 3;
		if (next == 0) _tree->setFocus();
		else if (next == 1) _inspector->setFocus();
		else (_edit->isEnabled() ? _edit : _help)->setFocus();
	}

	void editShortcut(int key) {
		const auto row = selected();
		if (key == Qt::Key_Y && row && !row->address.empty() && visibleContent()) {
			QApplication::clipboard()->setText(SettingsTree::TomlPath(row->address));
			return;
		}
		if (!row || !row->editable) return;
		if (key == Qt::Key_T && row->descriptor->codec == SettingCodec::Boolean && row->effective.isValid()) {
			stage(*row, SettingScalar(!row->effective.toBool()));
		} else if (key == Qt::Key_Plus || key == Qt::Key_Minus) {
			if (const auto value = SettingsTree::Step(*row->descriptor, row->effective,
					key == Qt::Key_Plus ? 1 : -1)) {
				stage(*row, *value);
			}
		}
	}

	bool handleKey(QObject *object, QKeyEvent *event) {
		if (!visibleContent()) {
			if (object == _coverClose && !event->isAutoRepeat()
				&& (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
				_coverClose->click();
			}
			return true;
		}
		const auto mods = (_reviewBody && object == _reviewBody)
			? ModalScrollModifiers(*event, this)
			: (event->modifiers() & ~Qt::KeypadModifier);
		const auto key = ModalPhysicalKey(*event);
		const auto enter = key == Qt::Key_Return || key == Qt::Key_Enter;
		const auto cancel = key == Qt::Key_Escape || (key == Qt::Key_G && mods == kCtrlMod);
		const auto modal = ModalKeysEnabled();
		const auto once = [&] { return AllowModalLayerKey(*event, this); };
		const auto moving = [&] { return AllowModalLayerKey(*event, this, true); };
		if (_capture && _pages->currentWidget() == _sheetPage && object == _record) {
			if (key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Meta
				&& key != Qt::Key_Alt && key != Qt::Key_AltGr && key != Qt::Key_unknown
				&& once()) {
				const auto chord = QKeySequence(event->keyCombination());
				_line->setText(chord.toString(QKeySequence::PortableText));
				endCapture();
			}
			return true;
		}
		if (_preedit.contains(object) || _preedit.contains(QApplication::focusWidget())
			|| (_collection && SettingsCollectionHasPreedit(_collection))) return false;
		if (const auto half = ListFilter::HalfPageDirection(key, mods)) {
			if (const auto list = qobject_cast<QListWidget*>(object)) {
				if (moving()) {
					const auto rows = ListFilter::HalfPageRows(
						list->viewport()->height(), list->sizeHintForRow(list->currentRow()));
					list->setCurrentRow(std::clamp(list->currentRow() + half * rows,
						0, std::max(0, list->count() - 1)));
				}
				return true;
			} else if (object == _tree || object == _filter) {
				if (moving()) {
					const auto row = _tree->visualItemRect(_tree->currentItem()).height();
					selectSibling(half * ListFilter::HalfPageRows(
						_tree->viewport()->height(), row));
				}
				return true;
			} else if (_reviewBody && object == _reviewBody) {
				if (moving()) {
					const auto bar = _reviewBody->verticalScrollBar();
					bar->setValue(bar->value() + half * ListFilter::HalfPagePixels(
						_reviewBody->viewport()->height()));
				}
				return true;
			}
		}
		if (const auto button = qobject_cast<QPushButton*>(object); button && enter && !mods) {
			if (once()) button->click();
			return true;
		}
		if (object == _filter) {
			if (cancel || (modal && ListFilter::Exit(key, mods))) {
				if (!once()) return true;
				_tree->setFocus();
				return true;
			}
			if (enter && !mods) {
				if (!once()) return true;
				_tree->setFocus();
				if (modal) activateRow();
				return true;
			}
			if ((!mods && (key == Qt::Key_Down || key == Qt::Key_Up))
				|| (mods == kCtrlMod && (key == Qt::Key_N || key == Qt::Key_P))) {
				if (moving()) selectSibling((key == Qt::Key_Down || key == Qt::Key_N) ? 1 : -1);
				return true;
			}
		}
		if (_pages->currentWidget() == _sheetPage) {
			if (const auto list = qobject_cast<QListWidget*>(object); list && !mods
				&& (key == Qt::Key_Down || key == Qt::Key_Up
					|| (modal && (key == Qt::Key_J || key == Qt::Key_K)))) {
				const auto down = (key == Qt::Key_Down || key == Qt::Key_J);
				if (moving()) {
					list->setCurrentRow(std::clamp(list->currentRow() + (down ? 1 : -1),
						0, std::max(0, list->count() - 1)));
				}
				return true;
			}
			if (_reviewBody && object == _reviewBody && !mods) {
				if (key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) {
					if (once()) showReviewPage(key == Qt::Key_BracketLeft ? -1 : 1);
					return true;
				}
				if (key == Qt::Key_Down || key == Qt::Key_Up
					|| (modal && (key == Qt::Key_J || key == Qt::Key_K))) {
					if (moving()) {
						const auto direction = (key == Qt::Key_Down || key == Qt::Key_J)
							? 1 : -1;
						ScrollModalLayer(*event, this, _reviewBody,
							direction * _reviewBody->verticalScrollBar()->singleStep());
					}
					return true;
				}
			}
			if (_choice && _choice->view()->isVisible()) return false;
			const auto submit = enter && (mods == kCtrlMod || (!mods && object == _line));
			switch (SettingsTree::ShouldHandleEditorKey(
					_preedit.contains(object), submit, !_multi || mods == kCtrlMod, cancel)) {
			case EditorKey::Stage: if (once() && !_editingKey.isEmpty()) stageForm(); return true;
			case EditorKey::Back: if (once()) back(); return true;
			case EditorKey::Native: return false;
			}
		}
		if (cancel || (modal && ListFilter::Exit(key, mods)
			&& (object == _tree || object == this))) {
			if (once()) {
				if (modal && !_filter->text().isEmpty() && (object == _tree || object == this)) {
					_filter->clear();
				} else {
					back();
				}
			}
			return true;
		}
		if (key == Qt::Key_F6) { if (once()) changePanel(mods.testFlag(Qt::ShiftModifier)); return true; }
		if (key == Qt::Key_Tab && !mods && object == _inspector) {
			if (once()) _tree->setFocus();
			return true;
		}
		if (object != _tree && object != this && !qobject_cast<QPushButton*>(object)) {
			return false;
		}
		if (((key == Qt::Key_Down || (modal && key == Qt::Key_J)) && !mods)
			|| (key == Qt::Key_N && mods == kCtrlMod)) { if (moving()) selectSibling(1); }
		else if (((key == Qt::Key_Up || (modal && key == Qt::Key_K)) && !mods)
			|| (key == Qt::Key_P && mods == kCtrlMod)) { if (moving()) selectSibling(-1); }
		else if (modal && key == Qt::Key_H && !mods) { if (moving()) traverse(false); }
		else if (modal && key == Qt::Key_L && !mods) { if (moving()) traverse(true); }
		else if ((key == Qt::Key_Left || key == Qt::Key_Right) && !mods) {
			if (moving()) traverse((key == Qt::Key_Right) == (layoutDirection() != Qt::RightToLeft));
		} else if (key == Qt::Key_Tab && !mods) {
			if (!once()) return true;
			if (const auto item = _tree->currentItem(); item && item->childCount()) {
				item->setExpanded(!item->isExpanded());
			} else {
				_inspector->setFocus();
			}
		} else if (enter && !mods) { if (once()) activateRow(); }
		else if (!modal) return false;
		else if (ListFilter::Enter(*event, key)) { if (once()) _filter->setFocus(); }
		else if (key == Qt::Key_Question) { if (once()) showHelp(); }
		else if (key == Qt::Key_R && !mods) { if (once()) resetSelected(); }
		else if (key == Qt::Key_U && !mods) { if (once()) undo(); }
		else if (key == Qt::Key_D && !mods) { if (once()) showReview(); }
		else if (key == Qt::Key_A && mods == Qt::ShiftModifier) { if (once()) showAccounts(); }
		else if (key == Qt::Key_Q && !mods) { if (once()) back(); }
		else if (((key == Qt::Key_Y || key == Qt::Key_T || key == Qt::Key_Minus) && !mods)
			|| (key == Qt::Key_Plus && (!mods || mods == Qt::ShiftModifier))) {
			if (once()) editShortcut(key);
		} else return false;
		return true;
	}

	void activateRow() {
		const auto row = selected();
		if (!row || !visibleContent()) return;
		if (row->listCaseFix) {
			auto fixed = *row;
			fixed.key = SettingsTree::Address(row->address);
			stage(std::move(fixed), SettingScalar(*row->listCaseFix));
		} else if (isCollection(*row)) {
			showCollection(*row);
		} else if (row->editable) {
			showForm(*row);
		} else if (row->invalid && row->address.empty()) {
			showRecoveryIfNeeded();
		} else if (const auto current = _tree->currentItem(); current && current->childCount()) {
			current->setExpanded(!current->isExpanded());
		} else {
			_inspector->setFocus();
		}
	}

	void addSheetWidget(QWidget *widget) {
		widget->installEventFilter(this);
		if (const auto button = qobject_cast<QPushButton*>(widget)) CustomizeLook::AppendAction(_sheetLayout, button);
		else _sheetLayout->addWidget(widget);
	}

	void clearSheet() {
		_collection.clear();
		_catalogueLifetime.destroy();
		_sheetBack = {};
		_reviewBody = nullptr;
		_reviewEffectsShown = false;
		_applyMessage.clear();
		_reviewPages.clear();
		_protectedForm = SettingsProtectedForm();
		_exitFocus = SettingsFocus();
		_lastField.clear();
		_formExit.clear();
		_formBaseline.reset();
		_editingKey.clear();
		_preedit.clear();
		_capture = false;
		delete std::exchange(_bool, nullptr);
		_line = nullptr;
		if (const auto choice = std::exchange(_choice, nullptr)) {
			choice->hidePopup();
		}
		_multi = nullptr;
		_formStatus = nullptr;
		_record = nullptr;
		_cancelCapture = nullptr;
		_confirm.clear();
		while (const auto item = _sheetLayout->takeAt(0)) {
			if (const auto widget = item->widget()) {
				widget->hide();
				widget->deleteLater();
			}
			delete item;
		}
	}

	void returnToTree() {
		_afterApply = {};
		clearSheet();
		if (_resumeRefused) {
			_pages->setCurrentWidget(_covered);
			_coverClose->setFocus();
			return;
		}
		const auto previous = SettingsFocus(_browserFocus);
		_pages->setCurrentWidget(_browser);
		if (!previous.restore(_browser)) _tree->setFocus();
		if (_needsRefresh || _draftStale) refreshFromDisk();
	}

	QLabel *sheetTitle(const QString &text) {
		const auto label = Label(_sheet, text);
		label->setFont(st::keysLegendTitleStyle.font);
		addSheetWidget(label);
		return label;
	}

	QPushButton *sheetButton(const QString &text, Fn<void()> action) {
		const auto button = Button(_sheet, text);
		addSheetWidget(button);
		connect(button, &QPushButton::clicked, this, std::move(action));
		return button;
	}

	void sheetPath(const QString &path) {
		const auto chip = new CustomizeLook::Chip(_sheet);
		chip->setDetails(u"Destination"_q, path);
		addSheetWidget(chip);
	}

	void showSheet(QWidget *focus) {
		if (!visibleContent()) return;
		_pages->setCurrentWidget(_sheetPage);
		if (focus) focus->setFocus();
	}

	QTextEdit *sheetText(const QString &content, const QString &name) {
		const auto text = new QTextEdit(_sheet);
		text->setPlainText(content);
		text->setReadOnly(true);
		text->setAccessibleName(name);
		text->setLayoutDirection(Qt::LeftToRight);
		text->setLineWrapMode(QTextEdit::WidgetWidth);
		text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
		text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		addSheetWidget(text);
		return text;
	}

	QString reviewEffects(const SettingsEditorPreview &preview) const {
		const auto frame = _frames.current();
		const auto edits = frame ? std::get_if<std::vector<SettingsEdit>>(&frame->intent) : nullptr;
		auto renames = std::vector<SettingsReviewDisplay::Rename>();
		if (edits) {
			for (const auto &edit : *edits) {
				const auto op = std::get_if<SettingsObjectEdit>(&edit.operation);
				if (!op || op->operation != SettingsCollectionOp::Rename || edit.address.empty()) continue;
				const auto name = std::get_if<QString>(&edit.address.back());
				if (!name) continue;
				auto destination = edit.address;
				destination.back() = op->name;
				auto references = QStringList();
				for (const auto &address : SettingsStructures::RewriteReferences(
						preview.before, edit.address, op->name).uses) {
					if (!SettingsStructures::SourceMatches(preview.before, preview.after, address)) {
						references.push_back(SettingsTree::Address(address));
					}
				}
				renames.push_back({ SettingsTree::Address(edit.address), SettingsTree::Address(destination),
					*name, op->name, std::move(references) });
			}
		}
		auto effective = std::map<QString, QString>();
		for (const auto &address : preview.changedAddresses) {
			const auto descriptor = FindSettingDescriptor(address);
			if (!descriptor || !SettingsTree::IsScalar(descriptor->codec)) continue;
			auto value = Row{ .descriptor = descriptor };
			value.effective = EffectiveSettingValue(preview.beforeLayers.effective.settings, address);
			const auto before = SettingsTree::ValueText(value);
			value.effective = EffectiveSettingValue(preview.afterLayers.effective.settings, address);
			const auto after = SettingsTree::ValueText(value);
			if (before != after) {
				effective.emplace(SettingsTree::Address(address), u"Effective · %1 → %2"_q.arg(before, after));
			}
		}
		const auto color = CustomizeLook::Tone(this, "customizeSurface", QPalette::Base);
		const auto ink = SettingsReviewDisplay::InkFor(color);
		const auto changes = SettingsReviewDisplay::Flatten(preview.summary);
		auto html = SettingsReviewDisplay::Effects(changes, renames, ink.removed, ink.added, effective);
		auto lines = QStringList();
		for (const auto &finding : preview.impact.findings) {
			if ((finding.dangerous || finding.kind != ConfigImpactKind::ValueChanged)
				&& !SettingsReviewDisplay::IsRenameOnlyLoss(finding, changes, renames)) {
				lines.push_back(finding.label);
			}
		}
		for (const auto &warning : preview.physicalBefore.warnings) {
			lines.push_back(u"Existing source warning · %1"_q.arg(warning));
		}
		for (const auto &entry : preview.afterLayers.inactive) {
			if (entry.reason == SettingsInactiveReason::UndefinedGlobalList) {
				lines.push_back(u"This account has an inactive list value at %1, line %2."_q
					.arg(entry.source.path).arg(entry.source.line));
			}
		}
		for (const auto &warning : preview.introducedWarnings) lines.push_back(warning);
		if (preview.incompleteTrace) lines.push_back(u"Some effective reads could not be verified."_q);
		if (preview.offlineGlobalSource) lines.push_back(u"The app-wide source account is signed out."_q);
		if (preview.usesGlobalRecovery) lines.push_back(u"Effective settings use an app-wide recovery copy."_q);
		if (html.isEmpty() && lines.isEmpty()) lines.push_back(u"The change affects the stored representation. See Lines."_q);
		return html + (lines.isEmpty() ? QString() : SettingsReviewDisplay::Block(u"Notes"_q,
			{ SettingsReviewDisplay::Escaped(lines.join(u'\n')) }));
	}

	void showReviewPage(int delta) {
		if (!_reviewBody || _reviewPages.empty() || !visibleContent()) return;
		_reviewEffectsShown = false;
		_reviewPage = std::clamp(_reviewPage + delta, 0, int(_reviewPages.size()) - 1);
		_reviewBody->setPlainText(u"Page %1 of %2\n\n"_q.arg(_reviewPage + 1).arg(_reviewPages.size())
			+ _reviewPages[_reviewPage]);
	}

	void showReview() {
		const auto frame = _frames.current();
		if (!frame || _applyInFlight || !visibleContent()) return;
		const auto review = frame->review;
		const auto weak = QPointer(this);
		const auto opening = _opening;
		const auto current = WindowSettingsEditorOpeningMatches(opening);
		if (!weak || !visibleContent() || opening.origin != _opening.origin) return;
		if (!current || _draftStale) {
			showConflict(review);
			return;
		}
		clearSheet();
		const auto &preview = review->preview();
		sheetTitle(u"Review settings changes"_q);
		sheetPath(preview.path);
		if (preview.diff.truncated) {
			addSheetWidget(Label(_sheet, u"The compact diff reached its limit. Lines includes both complete files in pages."_q));
		}
		_reviewPages = SettingsStructures::ReviewLines(preview.diff, preview.before, preview.after);
		_reviewPage = 0;
		_reviewBody = sheetText({}, u"Reviewed settings effects and lines"_q);
		_reviewEffectsShown = true;
		_reviewBody->setHtml(reviewEffects(preview));
		const auto actions = new CustomizeLook::Actions(_sheet);
		addSheetWidget(actions);
		const auto action = [&](const QString &text, Fn<void()> run) {
			const auto button = Button(actions, text);
			actions->append(button);
			button->installEventFilter(this);
			connect(button, &QPushButton::clicked, this, std::move(run));
			return button;
		};
		const auto back = action(u"Back"_q, [this] { returnToTree(); });
		action(u"Effects"_q, [this, review, body = QPointer(_reviewBody)] {
			if (!body || body != _reviewBody || !visibleContent()) return;
			_reviewEffectsShown = true;
			body->setHtml(reviewEffects(review->preview()));
		});
		action(u"Lines"_q, [this] { showReviewPage(0); });
		action(u"Previous page"_q, [this] { showReviewPage(-1); });
		action(u"Next page"_q, [this] { showReviewPage(1); });
		const auto apply = action(u"Apply changes"_q, [this, review] { approve(review); });
		apply->setEnabled(!preview.diff.truncated || !_reviewPages.empty());
		showSheet(back);
	}

	void approve(const std::shared_ptr<SettingsEditorReview> &review) {
		if (!visibleContent() || _applyInFlight) return;
		const auto &preview = review->preview();
		if (preview.route == SettingsEditorRoute::Peek) {
			submitReview(review);
			return;
		}
		if (!Consequential(preview)) {
			submitReview(review);
			return;
		}
		clearSheet();
		sheetTitle(u"Confirm settings change"_q);
		sheetPath(preview.path);
		sheetText({}, u"Consequences"_q)->setHtml(reviewEffects(preview));
		_sheetBack = [this] { showReview(); };
		const auto cancel = sheetButton(u"Cancel"_q, _sheetBack);
		const auto apply = sheetButton(u"Confirm and apply"_q, [this, review] { submitReview(review); });
		_confirm = apply;
		apply->setEnabled(false);
		QTimer::singleShot(0, apply, [apply] { apply->setEnabled(true); });
		showSheet(cancel);
	}

	void submitReview(std::shared_ptr<SettingsEditorReview> review) {
		const auto host = _host.get();
		if (!host || !visibleContent() || _applyInFlight) return;
		const auto active = _frames.current();
		if (!active || active->review != review) return;
		const auto key = _selection;
		const auto serial = ++_applySerial;
		_applyInFlight = true;
		const auto weak = QPointer(this);
		displayApplying(u"Checking the reviewed settings source…"_q);
		if (!weak || _applySerial != serial) return;
		if (!visibleContent()) {
			_applyInFlight = false;
			return;
		}
		RequestSettingsEditorApply(host, std::move(review), [weak, serial, key](SettingsEditorApplyResult result) {
			if (weak && weak->_applySerial == serial && weak->sameSession()) {
				weak->applyCompleted(std::move(result), key);
			}
		});
	}

	void displayApplying(const QString &message) {
		clearSheet();
		sheetTitle(u"Applying reviewed settings"_q);
		_applyMessage = Label(_sheet, message);
		addSheetWidget(_applyMessage);
		showSheet(_applyMessage);
	}

	void applyCompleted(SettingsEditorApplyResult result, const QString &key) {
		if (result.status == Status::Waiting) {
			if (_applyMessage && visibleContent()) _applyMessage->setText(u"Waiting for the Peek gate. Confirm or abort in the countdown."_q);
			return;
		}
		_applyInFlight = false;
		const auto after = std::exchange(_afterApply, {});
		const auto saved = result.status == Status::Saved || result.status == Status::SavedWithFollowUpError;
		if (saved) {
			_frames.clear();
			_selection = key;
			_needsRefresh = true;
			_draftStale = false;
		} else if (result.status == Status::StaleRevision) {
			_draftStale = true;
		}
		const auto message = saved ? (result.status == Status::Saved
			? u"Settings saved."_q : u"Settings saved. Follow-up failed. %1"_q.arg(result.error))
			: result.error.isEmpty() ? u"Settings were not applied."_q : result.error;
		const auto weak = QPointer(this);
		AnnounceError(_feedback, message);
		if (!weak || !visibleContent()) return;
		returnToTree();
		if (!weak || !visibleContent()) return;
		if (result.status == Status::Saved && after) after();
	}

	void showConflict(std::shared_ptr<SettingsEditorReview> review) {
		if (!visibleContent()) return;
		const auto host = _host.get();
		if (!host) return;
		const auto weak = QPointer(this);
		const auto fresh = CaptureWindowSettingsEditor(host, [weak] { return weak && weak->visibleContent(); });
		if (!weak || !visibleContent()) return;
		const auto &preview = review->preview();
		const auto matches = [&](const std::optional<AccountSettingsOpenTarget> &source) {
			return source && source->file.account == preview.fileAccount && source->file.path == preview.path;
		};
		const auto disk = matches(fresh.ownSource) ? &fresh.ownSource->file.input
			: matches(fresh.globalSource) ? &fresh.globalSource->file.input : nullptr;
		clearSheet();
		sheetTitle(u"Settings changed since this draft"_q);
		sheetPath(preview.path);
		if (disk) {
			const auto onDisk = disk->state == SettingsFileState::ReadError
				? ConfigChangeSummary() : SummarizeConfigChange(preview.before, disk->bytes);
			const auto differences = SettingsReviewDisplay::Conflict(preview.summary, onDisk);
			if (!differences.isEmpty()) {
				sheetText({}, u"Differing settings"_q)->setHtml(differences);
			} else {
				addSheetWidget(HelpLabel(_sheet, u"The source changed without a comparable setting value."_q));
			}
		} else {
			addSheetWidget(HelpLabel(_sheet, u"The reviewed destination is no longer available."_q));
		}
		const auto keep = sheetButton(u"Keep draft for comparison"_q, [this] { returnToTree(); });
		sheetButton(u"Reload and review"_q, [this, review, fresh] {
			const auto guard = QPointer(this);
			const auto frame = _frames.current();
			if (!frame || frame->review != review || !visibleContent()) return;
			const auto intent = frame->intent;
			const auto result = ReprepareWindowSettingsEditorEdit(fresh, *review);
			if (!guard || !visibleContent()) return;
			if (!result.review) {
				addSheetWidget(Label(_sheet, StatusText(result)));
				return;
			}
			_opening = fresh;
			_frames.clear();
			_frames.push(Draft{ intent, result.review });
			_draftStale = false;
			rebuild();
			showReview();
		});
		sheetButton(u"Discard draft"_q, [this] { discardDraft(); });
		if (disk) {
			const auto raw = sheetText(u"Yours\n%1\n\nOn disk\n%2\n\nOriginal\n%3"_q.arg(
				QString::fromUtf8(preview.after), QString::fromUtf8(disk->bytes), QString::fromUtf8(preview.before)),
				u"Raw settings files"_q);
			raw->hide();
			const auto toggle = Button(_sheet, u"Show raw files"_q);
			addSheetWidget(toggle);
			connect(toggle, &QPushButton::clicked, this, [this, toggle, raw] {
				const auto show = !raw->isVisible();
				raw->setVisible(show);
				toggle->setText(show ? u"Hide raw files"_q : u"Show raw files"_q);
				if (show) _sheetPage->ensureWidgetVisible(raw);
			});
			_sheetLayout->removeWidget(raw);
			_sheetLayout->addWidget(raw);
		}
		showSheet(keep);
	}

	void discardDraft() {
		_frames.clear();
		_afterApply = {};
		if (!capture()) return;
		const auto weak = QPointer(this);
		returnToTree();
		if (!weak) return;
		rebuild();
		describeDraft();
	}

	void showRecoveryIfNeeded() {
		if (!visibleContent() || !_opening.current || _applyInFlight) return;
		const auto inspection = InspectSettingsEditor(_opening);
		const auto invalid = [](const SettingsLayerFileReport &report) {
			return report.current.present && !report.current.path.isEmpty() && !report.current.parsed.ok();
		};
		const auto global = invalid(inspection.global);
		if (!global && !invalid(inspection.account)) return;
		const auto report = global ? inspection.global : inspection.account;
		const auto &openingSource = global ? _opening.globalSource : _opening.ownSource;
		const auto source = openingSource ? openingSource->file.input.bytes : QByteArray();
		const auto mapped = MapSettingsSource(source);
		const auto location = report.current.path + (mapped.line > 0
			? u" · line %1, column %2"_q.arg(mapped.line).arg(mapped.column) : QString());
		clearSheet();
		sheetTitle(u"Settings file needs repair"_q);
		addSheetWidget(Label(_sheet, Isolate(location)));
		addSheetWidget(Label(_sheet, report.current.parsed.error));
		const auto excerpt = SettingsStructures::SourceExcerpt(source, mapped.line, mapped.column);
		if (!excerpt.isEmpty()) sheetText(excerpt, u"Source excerpt"_q);
		addSheetWidget(Label(_sheet, report.usedGood ? u"Effective values use the last-good copy."_q
			: u"This file has no usable settings. Other eligible sources can still be inspected."_q));
		const auto retry = sheetButton(u"Retry after repair"_q, [this] {
			refreshFromDisk();
			returnToTree();
			showRecoveryIfNeeded();
		});
		sheetButton(u"Copy location"_q, [this, location] {
			if (visibleContent()) QApplication::clipboard()->setText(location);
		});
		sheetButton(u"Browse effective values"_q, [this] { returnToTree(); });
		sheetButton(u"Close"_q, [this] { closeLayer(); });
		showSheet(retry);
	}

	void showAccounts() {
		if (!visibleContent() || _applyInFlight) return;
		clearSheet();
		sheetTitle(u"Account to inspect"_q);
		const auto list = new QListWidget(_sheet);
		list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		list->setTextElideMode(Qt::ElideRight);
		list->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		list->setAccessibleName(u"Signed-in accounts"_q);
		auto accounts = std::vector<base::weak_ptr<Main::Account>>();
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto session = entry.account->maybeSession();
			if (!session || entry.account->loggingOut() || session->termsLocked()) continue;
			const auto key = SessionAccountKey(session);
			const auto source = _opening.globalSource && _opening.globalSource->file.account == key;
			list->addItem(session->user()->name() + (source ? u" · Supplies app-wide settings"_q
				: u" · "_q + AccountWorkModeTitle(session)));
			accounts.push_back(base::make_weak(entry.account.get()));
			if (key == _opening.requester) list->setCurrentRow(list->count() - 1);
		}
		addSheetWidget(list);
		const auto choose = [this, list, accounts] {
			const auto index = list->currentRow();
			if (index >= 0 && index < int(accounts.size())) chooseAccount(accounts[index]);
		};
		connect(list, &QListWidget::itemActivated, this, choose);
		sheetButton(u"Switch buffer"_q, choose);
		sheetButton(u"Back"_q, [this] { returnToTree(); });
		showSheet(list);
	}

	bool isCollection(const Row &row) const {
		const auto descriptor = row.descriptor;
		return descriptor && (descriptor->codec == SettingCodec::Array
			|| SettingsStructures::IsNamedContainer(descriptor->id)
			|| SettingsStructures::IsNamedObject(descriptor->id));
	}

	void showCollection(Row row) {
		if (!visibleContent() || !row.descriptor) return;
		const auto id = row.descriptor->id;
		const auto field = PeerFieldFor(id);
		const auto isMember = field == PeerField::Members;
		const auto scope = SettingsScope(row.address);
		const auto appWide = isMember ? _opening.workMode == AccountWorkMode::Follow
			: scope == SettingScope::AppWide;
		const auto &openingTarget = appWide ? _opening.appWide : _opening.own;
		if (!openingTarget) {
			const auto error = appWide ? _opening.appWideError : _opening.ownError;
			AnnounceError(_feedback, error.isEmpty() ? u"This collection is read-only in the current context."_q : error);
			return;
		}
		const auto target = openingTarget->destination.file;
		const auto frame = _frames.current();
		const auto previous = frame ? frame->review : nullptr;
		if (previous && (previous->preview().path != target.path
			|| (previous->preview().route == SettingsEditorRoute::Members) != isMember)) {
			showIncompatibleDraft();
			return;
		}
		auto bytes = previous ? previous->preview().after : target.input.bytes;
		if (!MapSettingsSource(bytes).ok()) {
			showRecoveryIfNeeded();
			return;
		}
		auto address = row.address;
		const auto named = SettingsStructures::IsNamedContainer(id) || SettingsStructures::IsNamedObject(id);
		if (SettingsStructures::IsNamedObject(id)) address.pop_back();
		auto members = std::optional<SettingsEditorMembers>();
		if (isMember) {
			const auto &listName = std::get<QString>(address[1]);
			const auto layers = previous ? previous->preview().afterLayers : InspectSettingsEditor(_opening);
			const auto list = layers.effective.settings.list(listName);
			members = SettingsEditorMembers{ listName, list ? list->members : std::vector<PeerIdValue>() };
			if (frame) {
				const auto existing = std::get_if<SettingsEditorMembers>(&frame->intent);
				if (!existing || existing->list != listName) {
					showIncompatibleDraft();
					return;
				}
			}
		}
		const auto session = field ? LiveSessionFor(target.account) : nullptr;
		auto peers = field ? PeerCatalogue(session, *field) : std::vector<SettingsPeerChoice>();
		auto seed = std::vector<SettingsEdit>();
		if (field == PeerField::Notifications) {
			const auto map = MapSettingsSource(bytes);
			const auto explicitValues = std::any_of(map.nodes.begin(), map.nodes.end(), [&](const auto &node) {
				return node.address == address;
			});
			if (!explicitValues) {
				const auto layers = previous ? previous->preview().afterLayers : InspectSettingsEditor(_opening);
				const auto &notifications = layers.effective.settings.notifications;
				const auto add = [&](QString text) {
					seed.push_back(SettingsStructures::ArrayEdit(address, SettingsCollectionOp::Insert,
						seed.size(), 0, std::move(text)));
				};
				if (notifications.magicBots) add(u"MAGIC_BOTS"_q);
				if (notifications.magicChannels) add(u"MAGIC_CHANNELS"_q);
				for (const auto &peer : notifications.previewAlways) add(PreviewPeerLiteral(peer));
			}
		}
		const auto opening = _opening.origin;
		const auto referenceOpening = named ? _opening : SettingsEditorOpening();
		const auto weak = QPointer(this);
		clearSheet();
		_editingKey = row.key;
		sheetTitle(row.label);
		sheetPath(target.path);
		if (field && !session) {
			addSheetWidget(Label(_sheet, u"The source account is signed out. Recorded IDs and typed IDs are available."_q));
		}
		_collection = CreateSettingsCollection(_sheet, {
			.title = row.label,
			.address = std::move(address),
			.bytes = std::move(bytes),
			.peerField = field,
			.peers = std::move(peers),
			.seed = std::move(seed),
			.members = std::move(members),
			.named = named,
			.current = [weak, opening] {
				return weak && weak->visibleContent() && !weak->_applyInFlight
					&& weak->_opening.origin == opening;
			},
			.stage = [weak, key = row.key, previous](SettingsEditorIntent intent) {
				if (!weak || !weak->visibleContent()) return;
				const auto active = weak->_frames.current();
				if ((active ? active->review : nullptr) != previous) return;
				if (auto edits = std::get_if<std::vector<SettingsEdit>>(&intent); edits && active) {
					const auto current = std::get_if<std::vector<SettingsEdit>>(&active->intent);
					if (!current) { weak->showIncompatibleDraft(); return; }
					edits->insert(edits->begin(), current->begin(), current->end());
				}
				weak->stageIntent(std::move(intent), key);
			},
			.otherReferences = [referenceOpening, path = target.path](const SettingAddress &object) {
				return SettingsEditorOtherReferences(referenceOpening, path, object);
			},
			.inspect = [weak](SettingAddress selection) {
				if (!weak || !weak->visibleContent()) return;
				weak->_selection = SettingsTree::Address(selection);
				weak->returnToTree();
				if (weak) weak->rebuild(true);
			},
			.close = [weak] { if (weak) weak->returnToTree(); },
		});
		if (session) {
			const auto collection = _collection;
			session->data().sessionDataAboutToBeCleared() | rpl::on_next([collection] {
				if (collection) RefreshSettingsCollectionPeers(collection, true);
			}, _catalogueLifetime);
			rpl::merge(SettingsChanges(), StateChanges()) | rpl::on_next([collection] {
				if (collection) RefreshSettingsCollectionPeers(collection);
			}, _catalogueLifetime);
		}
		addSheetWidget(_collection);
		_formStatus = Label(_sheet);
		addSheetWidget(_formStatus);
		_sheetBack = [weak] { if (weak && weak->_collection) BackFromSettingsCollection(weak->_collection); };
		showSheet(_collection);
	}

	void pickBudgetChat(Row row) {
		if (!visibleContent() || !_line || !_opening.globalSource || _sheetBack) return;
		const auto session = LiveSessionFor(_opening.globalSource->file.account);
		const auto choices = PeerCatalogue(session, PeerField::Budget);
		if (choices.empty()) {
			AnnounceError(_formStatus, u"No visible chats for this source. Enter a chat:ID value directly."_q);
			return;
		}
		const auto list = new QListWidget(_sheet);
		list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		list->setTextElideMode(Qt::ElideRight);
		list->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		list->setAccessibleName(u"Budget chat"_q);
		for (const auto &choice : choices) {
			const auto item = new QListWidgetItem(choice.avatar, choice.label, list);
			item->setData(Qt::UserRole, choice.literal);
		}
		addSheetWidget(list);
		const auto cancel = Button(_sheet, u"Back to field"_q);
		addSheetWidget(cancel);
		const auto focus = SettingsFocus(_line);
		const auto close = [this, list = QPointer(list), cancel = QPointer(cancel), focus] {
			_sheetBack = {};
			_catalogueLifetime.destroy();
			for (const auto child : { QPointer<QWidget>(list.data()), QPointer<QWidget>(cancel.data()) }) {
				if (child) {
					child->hide();
					child->deleteLater();
				}
			}
			if (!focus.restore(_sheet)) _sheetPage->setFocus();
		};
		_sheetBack = close;
		connect(cancel, &QPushButton::clicked, this, close);
		connect(list, &QListWidget::itemActivated, this, [this, list, choices, key = row.key, close](QListWidgetItem *item) {
			if (!visibleContent() || _editingKey != key || !_line) return;
			const auto id = item->data(Qt::UserRole).toString();
			const auto choice = std::find_if(choices.begin(), choices.end(), [&](const auto &entry) {
				return entry.literal == id;
			});
			if (choice == choices.end() || (choice->current && !choice->current())) {
				list->clear();
				AnnounceError(_formStatus, u"That chat is no longer visible in the source account."_q);
				return;
			}
			_line->setText(id);
			const auto guard = QPointer(this);
			close();
			if (guard && _line) _line->end(false);
		});
		const auto weakList = QPointer(list);
		session->data().sessionDataAboutToBeCleared() | rpl::on_next([weakList] {
			if (weakList) weakList->clear();
		}, _catalogueLifetime);
		StateChanges() | rpl::on_next([weakList, choices] {
			if (weakList && std::any_of(choices.begin(), choices.end(), [](const auto &choice) {
				return choice.current && !choice.current();
			})) weakList->clear();
		}, _catalogueLifetime);
		list->setCurrentRow(0);
		list->setFocus();
		_sheetPage->ensureWidgetVisible(list);
	}

	void chooseAccount(base::weak_ptr<Main::Account> account) {
		const auto live = account.get();
		if (!live || !visibleContent() || !live->maybeSession() || live->loggingOut()) return;
		if (live->maybeSession() == _session.get()) {
			returnToTree();
			return;
		}
		const auto key = SessionAccountKey(live->maybeSession());
		const auto switchBuffer = [weak = QPointer(this), account, key] {
			const auto target = account.get();
			const auto session = target ? target->maybeSession() : nullptr;
			if (!weak || !weak->visibleContent() || !session || target->loggingOut()
				|| SessionAccountKey(session) != key || session->termsLocked()) return;
			const auto id = Window::SeparateId(not_null{ target });
			const auto window = Core::App().ensureSeparateWindowFor(id);
			new OpenCustomizeOnActivation(window, key);
			if (weak) weak->closeLayer();
		};
		if (!_frames.current()) {
			switchBuffer();
			return;
		}
		clearSheet();
		sheetTitle(u"Switch account with staged changes?"_q);
		const auto keep = sheetButton(u"Keep editing"_q, [this] { returnToTree(); });
		sheetButton(u"Review and apply"_q, [this, switchBuffer] {
			_afterApply = switchBuffer;
			showReview();
		});
		sheetButton(u"Discard and switch"_q, switchBuffer);
		showSheet(keep);
	}

	void showHelp() {
		clearSheet();
		const auto title = Label(_sheet, u"Customize settings help"_q);
		title->setFont(st::keysLegendTitleStyle.font);
		addSheetWidget(title);
		const auto row = selected();
		if (row && !row->help.isEmpty()) addSheetWidget(HelpLabel(_sheet, row->help));
		addSheetWidget(HelpLabel(_sheet, ModalKeysEnabled()
			? u"j / k Move · h / l Fold or enter · i or / Filter · Return Edit\n"
				"t Toggle · + / − Step · r Reset · u Undo · d Review · y Copy TOML path\n"
				"A Switch account · F6 Change panel · F8 Privacy screen outside text fields · Esc Back"_q
			: u"Arrows Move or fold · Return Edit · F6 Change panel · Esc Back\n"
				"F8 Privacy screen outside text fields"_q));
		addSheetWidget(HelpLabel(_sheet, u"Forms · Ctrl+Return Stage. Return inserts a newline in multiline fields."_q));
		if (ModalKeysEnabled()) {
			addSheetWidget(HelpLabel(_sheet, u"Collections · a Add · Shift+D Duplicate · x Remove · Shift+J/K Reorder · u Undo\n"
				"Chat picker · i or / Search · Space Add the selected chat"_q));
		}
		addSheetWidget(HelpLabel(_sheet, u"Review · Effects shows what changes. Lines shows the file diff. "
			"Use [ / ] to page through Lines while it has focus. Apply changes writes this draft to one file."_q));
		const auto close = Button(_sheet, u"Back"_q);
		addSheetWidget(close);
		connect(close, &QPushButton::clicked, this, [this] { returnToTree(); });
		_sheetLayout->addStretch();
		_pages->setCurrentWidget(_sheetPage);
		close->setFocus();
	}

	void showForm(Row row) {
		clearSheet();
		_editingKey = row.key;
		const auto title = Label(_sheet, row.label);
		title->setFont(st::keysLegendTitleStyle.font);
		addSheetWidget(title);
		addSheetWidget(Label(_sheet, Isolate(SettingsTree::Address(row.address))
			+ (row.destination.isEmpty() ? QString() : u" · "_q + row.destination)));
		if (!row.help.isEmpty()) addSheetWidget(HelpLabel(_sheet, row.help));
		const auto &descriptor = *row.descriptor;
		auto focus = static_cast<QWidget*>(nullptr);
		if (descriptor.codec == SettingCodec::Boolean) {
			_bool = new QButtonGroup(_sheet);
			const auto values = std::array{ row.resetLabel, u"On"_q, u"Off"_q };
			for (auto i = 0; i != int(values.size()); ++i) {
				const auto radio = new QRadioButton(values[i], _sheet);
				_bool->addButton(radio, i);
				addSheetWidget(radio);
			}
			const auto checked = _bool->button(row.explicitHere ? (row.effective.toBool() ? 1 : 2) : 0);
			checked->setChecked(true);
			focus = checked;
		} else if (const auto choices = SettingsTree::Choices(descriptor); !choices.isEmpty()) {
			_choice = new QComboBox(_sheet);
			_choice->setAccessibleName(row.label);
			_choice->addItems(choices);
			_choice->setEditable(descriptor.codec == SettingCodec::Choice);
			_choice->setInsertPolicy(QComboBox::NoInsert);
			if (const auto completer = _choice->completer()) {
				completer->setFilterMode(Qt::MatchContains);
			}
			const auto value = descriptor.codec == SettingCodec::Duration
				? (row.effective.toInt() ? FieldText(row) : u"off"_q) : FieldText(row);
			_choice->setCurrentIndex(choices.indexOf(value));
			if (_choice->isEditable()) {
				_choice->setEditText(value);
				_line = _choice->lineEdit();
				_line->installEventFilter(this);
			}
			addSheetWidget(_choice);
			focus = _choice;
		} else if (descriptor.id == SettingId::ReplyText) {
			_multi = new QPlainTextEdit(_sheet);
			_multi->setAccessibleName(row.label);
			_multi->setPlainText(FieldText(row));
			_multi->setMaximumHeight(fontMetrics().height() * 9);
			addSheetWidget(_multi);
			focus = _multi;
		} else {
			_line = new QLineEdit(FieldText(row), _sheet);
			_line->setAccessibleName(row.label);
			if (descriptor.codec != SettingCodec::String || IsHotkey(descriptor)) {
				_line->setLayoutDirection(Qt::LeftToRight);
			}
			addSheetWidget(_line);
			focus = _line;
			if (IsHotkey(descriptor)) addCaptureControls();
			if (descriptor.id == SettingId::BudgetTarget) {
				sheetButton(u"Choose budget chat"_q, [this, row] { pickBudgetChat(row); });
			}
			if (descriptor.codec == SettingCodec::Duration) {
				_line->setPlaceholderText(u"Duration, e.g. 5m or off"_q);
				addDurationControls(descriptor);
			} else if (SettingsTree::IsSecondsInteger(descriptor.id)) {
				_line->setPlaceholderText(u"Seconds, e.g. 300 or 5m"_q);
			} else if (descriptor.codec == SettingCodec::Time) {
				_line->setPlaceholderText(u"HH:mm"_q);
			}
			if (descriptor.codec == SettingCodec::Integer || descriptor.codec == SettingCodec::Duration) {
				addStepControls(row);
			}
		}
		_formStatus = Label(_sheet);
		addSheetWidget(_formStatus);
		if (_line) connect(_line, &QLineEdit::textChanged, this, [this] { updateFormStatus(); });
		if (_multi) connect(_multi, &QPlainTextEdit::textChanged, this, [this] { updateFormStatus(); });
		updateFormStatus();
		const auto stageButton = Button(_sheet, u"Stage"_q, u"C-Enter"_q);
		addSheetWidget(stageButton);
		connect(stageButton, &QPushButton::clicked, this, [this] { stageForm(); });
		if (descriptor.codec != SettingCodec::Boolean && SettingsTree::CanReset(descriptor)) {
			const auto reset = Button(_sheet, row.resetLabel);
			addSheetWidget(reset);
			connect(reset, &QPushButton::clicked, this, [this] { resetSelected(); });
		}
		const auto cancel = Button(_sheet, u"Cancel"_q);
		addSheetWidget(cancel);
		connect(cancel, &QPushButton::clicked, this, [this] { closeForm(); });
		_formBaseline = formValue();
		_sheetLayout->addStretch();
		_pages->setCurrentWidget(_sheetPage);
		focus->setFocus();
	}

	SettingsDraft::FormValue formValue() const {
		if (_bool) return _bool->checkedId();
		if (_multi) return _multi->toPlainText();
		if (_choice) return _choice->currentText();
		return _line ? _line->text() : QString();
	}

	void closeForm() {
		if (!_formBaseline || !SettingsDraft::NeedsDiscardConfirmation(*_formBaseline, formValue())) {
			returnToTree();
			return;
		}
		if (_formExit) {
			const auto prompt = std::exchange(_formExit, {});
			prompt->hide();
			prompt->deleteLater();
			if (!_exitFocus.restore(_sheet)) _sheetPage->setFocus();
			return;
		}
		_exitFocus = SettingsFocus(_lastField ? _lastField.data() : QApplication::focusWidget());
		const auto panel = new QWidget(_sheet);
		const auto layout = new QVBoxLayout(panel);
		layout->setContentsMargins({});
		layout->addWidget(Label(panel, u"Discard the text in this form?"_q));
		const auto keep = Button(panel, u"Keep editing"_q);
		const auto discard = Button(panel, u"Discard input"_q);
		for (const auto button : { keep, discard }) {
			CustomizeLook::AppendAction(layout, button);
			button->installEventFilter(this);
		}
		_formExit = panel;
		addSheetWidget(panel);
		connect(keep, &QPushButton::clicked, this, [this] { closeForm(); });
		connect(discard, &QPushButton::clicked, this, [this] { returnToTree(); });
		keep->setFocus();
		_sheetPage->ensureWidgetVisible(keep);
	}

	void addStepControls(const Row &row) {
		for (const auto delta : { -1, 1 }) {
			const auto button = Button(_sheet, delta < 0 ? u"Decrease"_q : u"Increase"_q);
			addSheetWidget(button);
			connect(button, &QPushButton::clicked, this, [this, descriptor = row.descriptor, delta] {
				if (descriptor->codec == SettingCodec::Duration) {
					if (const auto stepped = SettingsTree::StepDurationField(*descriptor, _line->text(), delta)) {
						_line->setText(*stepped);
					}
					return;
				}
				const auto value = SettingsTree::Decode(*descriptor, _line->text());
				if (!value) return;
				const auto current = QVariant::fromValue(std::get<int64>(*value));
				if (const auto stepped = SettingsTree::Step(*descriptor, current, delta)) {
					_line->setText(QString::number(std::get<int64>(*stepped)));
				}
			});
		}
	}

	void addDurationControls(const SettingDescriptor &descriptor) {
		const auto controls = new CustomizeLook::Actions(_sheet);
		for (const auto &[label, suffix, seconds] : {
			std::tuple{ u"Seconds"_q, u"s"_q, 1 },
			std::tuple{ u"Minutes"_q, u"m"_q, 60 },
			std::tuple{ u"Hours"_q, u"h"_q, 3600 },
		}) {
			const auto button = Button(controls, label);
			controls->append(button);
			button->installEventFilter(this);
			connect(button, &QPushButton::clicked, this, [this, suffix, seconds] {
				const auto current = _line->text().trimmed();
				auto units = false;
				const auto count = current.toInt(&units);
				if (units && count >= 0) {
					_line->setText(QString::number(count) + suffix);
				} else if (const auto value = ParseDuration(current); value && *value % seconds == 0) {
					_line->setText(QString::number(*value / seconds) + suffix);
				} else {
					AnnounceError(_formStatus, u"Enter a whole number in the chosen unit."_q);
				}
			});
		}
		const auto off = Button(controls, u"Off"_q);
		controls->append(off);
		off->setEnabled(SettingsTree::Decode(descriptor, u"off"_q).has_value());
		off->installEventFilter(this);
		connect(off, &QPushButton::clicked, this, [this] { _line->setText(u"off"_q); });
		addSheetWidget(controls);
	}

	void addCaptureControls() {
		_record = Button(_sheet, u"Record chord"_q);
		addSheetWidget(_record);
		_cancelCapture = Button(_sheet, u"Cancel capture"_q);
		addSheetWidget(_cancelCapture);
		_cancelCapture->hide();
		const auto clear = Button(_sheet, u"Clear shortcut"_q);
		addSheetWidget(clear);
		connect(_record, &QPushButton::clicked, this, [this] {
			_capture = true;
			_record->setText(u"Press a chord"_q);
			_cancelCapture->show();
			_record->setFocus();
		});
		connect(_cancelCapture, &QPushButton::clicked, this, [this] { endCapture(); });
		connect(clear, &QPushButton::clicked, this, [this] {
			_line->clear();
			endCapture();
		});
	}

	void endCapture() {
		_capture = false;
		_record->setText(u"Record chord"_q);
		_cancelCapture->hide();
		_line->setFocus();
	}

	void updateFormStatus() {
		const auto row = selected();
		if (!row || !_formStatus) return;
		_formStatus->clear();
		if (_multi) {
			_formStatus->setText(u"%1 / 4096 characters"_q.arg(_multi->toPlainText().size()));
		} else if (_line && IsHotkey(*row->descriptor)) {
			_formStatus->setText(Isolate(QKeySequence::fromString(_line->text(),
				QKeySequence::PortableText).toString(QKeySequence::NativeText)));
		} else if (_line && (_line->text().size() > 0)
			&& (row->descriptor->codec == SettingCodec::Duration
				|| SettingsTree::IsSecondsInteger(row->descriptor->id))) {
			const auto value = SettingsTree::Decode(*row->descriptor, _line->text());
			if (!value) _formStatus->setText(u"Enter a supported duration."_q);
			else if (const auto seconds = std::get_if<int64>(&*value)) {
				_formStatus->setText(u"%1 seconds"_q.arg(*seconds));
			} else {
				_formStatus->setText(Isolate(std::get<QString>(*value)));
			}
		}
	}

	void stageForm() {
		const auto row = selected();
		if (!row || _editingKey != row->key || !visibleContent() || _capture) return;
		if (_bool) {
			if (const auto checked = _bool->checkedId(); checked >= 0) {
				stage(*row, checked ? std::make_optional(SettingScalar(checked == 1)) : std::nullopt);
			}
			return;
		}
		auto text = _multi ? _multi->toPlainText() : _choice ? _choice->currentText() : _line->text();
		if (IsHotkey(*row->descriptor) && !text.isEmpty()) {
			const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
			if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown
				|| sequence.toString(QKeySequence::PortableText).isEmpty()) {
				AnnounceError(_formStatus, u"Enter one valid portable shortcut."_q);
				return;
			}
			text = sequence.toString(QKeySequence::PortableText);
		}
		const auto value = SettingsTree::Decode(*row->descriptor, text);
		if (!value) {
			AnnounceError(_formStatus, u"This setting does not accept that value."_q);
		} else {
			stage(*row, *value);
		}
	}

	void resetSelected() {
		if (const auto row = selected(); row && row->editable && SettingsTree::CanReset(*row->descriptor)) {
			stage(*row, std::nullopt);
		}
	}

	void stage(Row row, std::optional<SettingScalar> value) {
		if (!visibleContent()) return;
		const auto frame = _frames.current();
		const auto previous = frame ? std::get_if<std::vector<SettingsEdit>>(&frame->intent) : nullptr;
		if (frame && !previous) {
			showIncompatibleDraft();
			return;
		}
		auto edits = previous ? *previous : std::vector<SettingsEdit>();
		const auto scalarOnly = std::all_of(edits.begin(), edits.end(), [](const SettingsEdit &edit) {
			return std::holds_alternative<std::optional<SettingScalar>>(edit.operation);
		});
		const auto existing = scalarOnly ? std::find_if(edits.begin(), edits.end(), [&](const SettingsEdit &edit) {
			return edit.address == row.address;
		}) : edits.end();
		if (existing == edits.end() && frame && frame->review->preview().route == SettingsEditorRoute::Peek) {
			AnnounceError(_formStatus ? _formStatus : _feedback, u"Stage each Peek timing change separately."_q);
			return;
		}
		auto change = SettingsEdit{ row.address, std::move(value) };
		if (existing == edits.end()) edits.push_back(std::move(change));
		else *existing = std::move(change);
		stageIntent(std::move(edits), row.key);
	}

	void stageIntent(SettingsEditorIntent intent, QString key) {
		if (!visibleContent() || _applyInFlight) return;
		const auto guard = QPointer(this);
		const auto opening = _opening;
		const auto frame = _frames.current();
		const auto previous = frame ? frame->review : nullptr;
		auto prepared = PrepareWindowSettingsEditorEdit(opening, intent);
		if (!guard || !visibleContent() || opening.origin != _opening.origin) return;
		if (prepared.status == Status::Unchanged) {
			_frames.push(std::nullopt);
		} else if (!prepared.review) {
			if (previous && prepared.status == Status::Unsupported) {
				showIncompatibleDraft();
				return;
			}
			AnnounceError(_formStatus ? _formStatus : _feedback, StatusText(prepared));
			return;
		} else {
			if (previous && previous->preview().after == prepared.review->preview().after) {
				returnToTree();
				return;
			}
			_frames.push(Draft{ std::move(intent), std::move(prepared.review) });
		}
		_anchor = filtering() ? key : QString();
		_selection = std::move(key);
		returnToTree();
		if (!guard || !recaptureCleanDraft()) return;
		rebuild();
		describeDraft(prepared.status);
	}

	void showIncompatibleDraft() {
		const auto frame = _frames.current();
		if (!frame) return;
		const auto path = frame->review->preview().path;
		if (_formBaseline) {
			AnnounceError(_formStatus, u"Review or undo the current draft for %1 before staging a different purpose."_q.arg(path));
			return;
		}
		clearSheet();
		sheetTitle(u"Review the current destination first"_q);
		addSheetWidget(Label(_sheet, Isolate(path)));
		const auto keep = sheetButton(u"Keep editing"_q, [this] { returnToTree(); });
		sheetButton(u"Review existing draft"_q, [this] { showReview(); });
		sheetButton(u"Discard existing draft"_q, [this] { discardDraft(); });
		showSheet(keep);
	}

	void describeDraft(Status status = Status::Ready) {
		const auto frame = _frames.current();
		const auto edits = frame ? std::get_if<std::vector<SettingsEdit>>(&frame->intent) : nullptr;
		_feedback->setText(!frame ? u"No staged edits."_q
			: status == Status::IncompleteTrace ? u"Staged. Some effective reads could not be verified."_q
			: u"%1 staged · %2"_q.arg(edits ? edits->size() : 1).arg(frame->review->preview().path));
	}

	bool recaptureCleanDraft() {
		if (!SettingsDraft::ShouldRecapture(_draftStale, _frames.current())) return true;
		_frames.clear();
		return capture();
	}

	void undo() {
		if (_frames.canUndo()) {
			_frames.undo();
			if (!recaptureCleanDraft()) return;
			rebuild();
		}
		describeDraft();
	}

	void back() {
		if (_applyInFlight && visibleContent()) return;
		if (_pages->currentWidget() == _sheetPage) {
			const auto action = _sheetBack;
			if (action) action();
			else closeForm();
		} else if (!visibleContent() || _resumeRefused || !_frames.current()) {
			closeLayer();
		} else {
			clearSheet();
			addSheetWidget(Label(_sheet, u"Discard staged changes and close?"_q));
			const auto keep = Button(_sheet, u"Keep editing"_q);
			const auto discard = Button(_sheet, u"Discard and close"_q);
			addSheetWidget(keep);
			sheetButton(u"Review and apply"_q, [this] {
				_afterApply = [weak = QPointer(this)] { if (weak) weak->closeLayer(); };
				showReview();
			});
			addSheetWidget(discard);
			connect(keep, &QPushButton::clicked, this, [this] { returnToTree(); });
			connect(discard, &QPushButton::clicked, this, [this] { closeLayer(); });
			_sheetLayout->addStretch();
			_pages->setCurrentWidget(_sheetPage);
			keep->setFocus();
		}
	}

	struct Draft {
		SettingsEditorIntent intent;
		std::shared_ptr<SettingsEditorReview> review;
	};

	base::weak_ptr<Window::Controller> _host;
	base::weak_ptr<Main::Session> _session;
	QPointer<::MainWindow> _window;
	QPointer<Ui::LayerWidget> _presentation;
	QStackedWidget *_pages;
	QWidget *_browser;
	QScrollArea *_sheetPage;
	QWidget *_sheet;
	QWidget *_covered;
	QLabel *_coverMessage;
	QPushButton *_coverClose;
	QLabel *_title;
	CustomizeLook::Chip *_subject;
	CustomizeLook::Chip *_destination;
	QPushButton *_accountsButton;
	QLineEdit *_filter;
	QPushButton *_clear;
	QTreeWidget *_tree;
	QLabel *_inspector;
	QLabel *_feedback;
	QPushButton *_edit;
	QPushButton *_reset;
	QPushButton *_undo;
	QPushButton *_reviewButton;
	QPushButton *_help;
	QVBoxLayout *_sheetLayout;
	SettingsEditorOpening _opening;
	std::vector<Row> _rows;
	SettingsDraft::History<Draft> _frames;
	QSet<QString> _expanded;
	QSet<QObject*> _preedit;
	QString _editingKey;
	QString _selection;
	QString _anchor;
	QString _accountId;
	QLineEdit *_line = nullptr;
	QPlainTextEdit *_multi = nullptr;
	QComboBox *_choice = nullptr;
	QButtonGroup *_bool = nullptr;
	QLabel *_formStatus = nullptr;
	QPushButton *_record = nullptr;
	QPushButton *_cancelCapture = nullptr;
	QPointer<QPushButton> _confirm;
	SettingsProtectedForm _protectedForm;
	QElapsedTimer _privacyPrefix;
	SettingsFocus _exitFocus;
	QPointer<QWidget> _browserFocus;
	QPointer<QWidget> _lastField;
	QPointer<QWidget> _formExit;
	QPointer<QWidget> _collection;
	rpl::lifetime _catalogueLifetime;
	std::optional<SettingsDraft::FormValue> _formBaseline;
	QTextEdit *_reviewBody = nullptr;
	QPointer<QLabel> _applyMessage;
	QStringList _reviewPages;
	Fn<void()> _sheetBack;
	Fn<void()> _afterApply;
	int _reviewPage = 0;
	bool _reviewEffectsShown = false;
	uint64_t _applySerial = 0;
	bool _applyInFlight = false;
	bool _capture = false;
	bool _needsRefresh = false;
	bool _retired = false;
	bool _draftStale = false;
	bool _resumeRefused = false;
};

class CustomizePresentation final : public Ui::LayerWidget {
public:
	explicit CustomizePresentation(not_null<CustomizeLayer*> editor)
	: _editor(editor.get()) {
		setFont(editor->font());
		editor->setParent(this);
		editor->show();
	}

	void parentResized() override {
		if (const auto parent = parentWidget()) {
			const auto gap = fontMetrics().height();
			const auto w = std::max(1, std::min(parent->width() - 2 * gap, st::boxWideWidth + 14 * gap));
			const auto h = std::max(1, std::min(parent->height() - 2 * gap, 34 * gap));
			setGeometry((parent->width() - w) / 2, (parent->height() - h) / 2, w, h);
			if (_editor && _editor->parentWidget() == this) _editor->setGeometry(rect());
		}
	}

	bool closeByOutsideClick() const override { return false; }

	bool closeByBackButton() override {
		if (_editor && _editor->parentWidget() == this) _editor->back();
		return true;
	}

protected:
	void doSetInnerFocus() override {
		if (_editor && _editor->parentWidget() == this) _editor->focusPage();
	}

private:
	QPointer<CustomizeLayer> _editor;
};

void CustomizeLayer::present(anim::type animated) {
	const auto host = _host.get();
	if (!host || !visibleContent() || _presentation) return;
	auto layer = std::make_unique<CustomizePresentation>(this);
	_presentation = layer.get();
	host->showLayer(std::move(layer), Ui::LayerOption::KeepOther, animated);
}

} // namespace

bool CanCustomizeSettings(Window::Controller *window) {
	return !Protected(window) && window->maybeSession()
		&& !window->maybeSession()->account().loggingOut();
}

void ShowCustomizeSettings(
		not_null<Window::Controller*> window,
		QWidget *returnFocus) {
	if (!CanCustomizeSettings(window.get()) || Core::App().activeWindow() != window.get()) {
		return;
	}
	const auto host = window->widget();
	if (const auto existing = OpenEditors.value(host.get())) {
		existing->focusPage();
		return;
	}
	if (!OpenEditors.contains(host.get())) {
		QObject::connect(host.get(), &QObject::destroyed, [raw = host.get()] {
			OpenEditors.remove(raw);
			LastSelection.remove(raw);
		});
	}
	const auto layer = QPointer(new CustomizeLayer(window));
	OpenEditors[host.get()] = layer;
	if (returnFocus) {
		const auto row = QPointer(returnFocus);
		const auto owner = base::make_weak(window.get());
		const auto session = base::make_weak(window->maybeSession());
		QObject::connect(layer.data(), &QObject::destroyed, returnFocus, [=] {
			QTimer::singleShot(0, row.data(), [=] {
				const auto live = owner.get();
				if (row && row->isVisible() && CanCustomizeSettings(live)
					&& live->maybeSession() == session.get()
					&& Core::App().activeWindow() == live) {
					row->setFocus(Qt::OtherFocusReason);
				}
			});
		});
	}
	layer->open();
	if (layer) layer->present(anim::type::normal);
}

std::optional<bool> FilterCustomizeSettingsInput(QObject *receiver, QEvent *event) {
	const auto type = event->type();
	const auto shortcut = type == QEvent::ShortcutOverride || type == QEvent::Shortcut;
	if (!shortcut && type != QEvent::KeyPress && type != QEvent::KeyRelease
		&& type != QEvent::InputMethod) {
		return std::nullopt;
	}
	const auto focus = QApplication::focusWidget();
	const auto target = qobject_cast<QWidget*>(receiver);
	for (const auto &editor : OpenEditors) {
		if (!editor || !editor->isVisible()) continue;
		const auto owns = [&](QWidget *widget) {
			return widget && (editor.data() == widget || editor->isAncestorOf(widget));
		};
		if (owns(target) || ((shortcut || QApplication::activePopupWidget()) && owns(focus))) {
			if (HandleModalWindowHints(target, event)) {
				return true;
			}
			if (type == QEvent::KeyPress && editor->privacyKey(focus, *static_cast<QKeyEvent*>(event))) {
				return true;
			}
			if (shortcut) event->accept();
			return shortcut;
		}
	}
	return std::nullopt;
}

} // namespace Purple
