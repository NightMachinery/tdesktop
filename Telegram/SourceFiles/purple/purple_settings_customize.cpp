#include "purple/purple_settings_customize.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "purple/purple_config.h"
#include "purple/purple_privacy_screen.h"
#include "purple/purple_settings_editor.h"
#include "purple/purple_settings_tree.h"
#include "styles/style_keys_legend.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "ui/layers/layer_widget.h"
#include "ui/style/style_core.h"
#include "window/window_controller.h"

#include <QtCore/QPointer>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <tuple>
#include <utility>

namespace Purple {
namespace {

using SettingsTree::Row;
using SettingsTree::EditorKey;
using Status = SettingsEditorStatus;

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

void AnnounceError(QLabel *label, const QString &message) {
	if (label->text() == message) return;
	label->setText(message);
	auto event = QAccessibleEvent(label, QAccessible::Alert);
	QAccessible::updateAccessibility(&event);
}

QLabel *Label(QWidget *parent, const QString &name = {}) {
	const auto label = new QLabel(name, parent);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	return label;
}

QPushButton *Button(QWidget *parent, const QString &text) {
	const auto result = new QPushButton(text, parent);
	result->setAutoDefault(false);
	return result;
}

bool IsHotkey(const SettingDescriptor &descriptor) {
	return descriptor.id == SettingId::PeekHotkey || descriptor.id == SettingId::PresetHotkey;
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

class CustomizeLayer final : public Ui::LayerWidget {
public:
	explicit CustomizeLayer(not_null<Window::Controller*> host)
	: _host(base::make_weak(host.get()))
	, _session(base::make_weak(host->maybeSession()))
	, _window(host->widget().get())
	, _pages(new QStackedWidget(this))
	, _browser(new QWidget(_pages))
	, _sheetPage(new QScrollArea(_pages))
	, _sheet(new QWidget(_sheetPage))
	, _covered(new QWidget(_pages))
	, _coverMessage(Label(_covered, u"Settings hidden while locked"_q))
	, _coverClose(Button(_covered, u"Close"_q))
	, _title(Label(_browser, u"Customize Purple settings"_q))
	, _subject(Label(_browser))
	, _filter(new QLineEdit(_browser))
	, _clear(Button(_browser, u"Clear filter"_q))
	, _tree(new QTreeWidget(_browser))
	, _inspector(Label(_browser))
	, _feedback(Label(_browser))
	, _edit(Button(_browser, u"Edit"_q))
	, _reset(Button(_browser, u"Reset"_q))
	, _undo(Button(_browser, u"Undo"_q))
	, _help(Button(_browser, u"Help"_q))
	, _sheetLayout(new QVBoxLayout(_sheet)) {
		setAccessibleName(u"Customize Purple settings"_q);
		setFocusPolicy(Qt::StrongFocus);
		auto outer = new QVBoxLayout(this);
		outer->setContentsMargins({});
		outer->addWidget(_pages);
		_sheetPage->setWidgetResizable(true);
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

		connect(_filter, &QLineEdit::textChanged, this, [this] {
			_anchor.clear();
			renderTree();
		});
		connect(_clear, &QPushButton::clicked, this, [this] {
			_filter->clear();
			_filterBeforeEdit.clear();
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
		capture();
		_selection = LastSelection.value(_window.data()).value(_accountId);
		rebuild(true);
	}

	bool closeByOutsideClick() const override { return false; }

	bool closeByBackButton() override {
		back();
		return true;
	}

protected:
	void parentResized() override {
		if (const auto parent = parentWidget()) {
			const auto gap = fontMetrics().height();
			const auto w = std::max(1, std::min(parent->width() - 2 * gap, st::boxWideWidth + 14 * gap));
			const auto h = std::max(1, std::min(parent->height() - 2 * gap, 34 * gap));
			setGeometry((parent->width() - w) / 2, (parent->height() - h) / 2, w, h);
		}
	}

	void doSetInnerFocus() override {
		if (_pages->currentWidget() == _browser) _tree->setFocus();
		else if (_resumeRefused && visibleContent()) _coverClose->setFocus();
	}

	void keyPressEvent(QKeyEvent *event) override {
		if (handleKey(this, event)) return;
		event->accept();
	}

	bool eventFilter(QObject *object, QEvent *event) override {
		if (object == _window.data()) {
			if (_needsRefresh && event->type() == QEvent::WindowActivate) {
				QTimer::singleShot(0, this, [this] { protectionChanged(); });
			}
			return false;
		}
		if (event->type() == QEvent::InputMethod) {
			const auto input = static_cast<QInputMethodEvent*>(event);
			if (input->preeditString().isEmpty()) _preedit.remove(object);
			else _preedit.insert(object);
		} else if (event->type() == QEvent::FocusIn && object == _filter) {
			_filterBeforeEdit = _filter->text();
		} else if (event->type() == QEvent::FocusOut) {
			_preedit.remove(object);
		} else if (event->type() == QEvent::ShortcutOverride && visibleContent()) {
			event->accept();
		} else if (event->type() == QEvent::KeyPress) {
			return handleKey(object, static_cast<QKeyEvent*>(event));
		}
		return false;
	}

private:
	void setupBrowser() {
		_browser->setAutoFillBackground(true);
		_sheet->setAutoFillBackground(true);
		_title->setFont(st::keysLegendTitleStyle.font);
		auto layout = new QVBoxLayout(_browser);
		auto header = new QHBoxLayout();
		header->addWidget(_title, 1);
		header->addWidget(_subject);
		const auto close = Button(_browser, u"Close"_q);
		header->addWidget(close);
		connect(close, &QPushButton::clicked, this, [this] { back(); });
		layout->addLayout(header);

		auto filterRow = new QHBoxLayout();
		_filter->setPlaceholderText(u"Filter settings…"_q);
		_filter->setAccessibleName(u"Filter settings"_q);
		filterRow->addWidget(_filter, 1);
		filterRow->addWidget(_clear);
		layout->addLayout(filterRow);

		_tree->setHeaderHidden(true);
		_tree->setAccessibleName(u"Settings tree"_q);
		_tree->setWordWrap(true);
		_tree->setExpandsOnDoubleClick(false);
		_tree->setFont(st::normalFont);
		_tree->setAlternatingRowColors(true);
		layout->addWidget(_tree, 1);

		_inspector->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
		_inspector->setAccessibleName(u"Selected setting details"_q);
		_feedback->setAccessibleName(u"Draft status"_q);
		layout->addWidget(_inspector);
		layout->addWidget(_feedback);
		auto footer = new QHBoxLayout();
		for (const auto button : { _edit, _reset, _undo, _help }) {
			footer->addWidget(button);
			button->installEventFilter(this);
		}
		footer->addStretch();
		layout->addLayout(footer);
		for (const auto child : { static_cast<QWidget*>(_filter), static_cast<QWidget*>(_tree),
			static_cast<QWidget*>(_inspector), static_cast<QWidget*>(_clear), static_cast<QWidget*>(close) }) {
			child->installEventFilter(this);
		}
	}

	void applyPalette() {
		auto roles = palette();
		roles.setColor(QPalette::Window, st::windowBg->c);
		roles.setColor(QPalette::Base, st::windowBg->c);
		roles.setColor(QPalette::AlternateBase, st::windowBgOver->c);
		roles.setColor(QPalette::WindowText, st::windowFg->c);
		roles.setColor(QPalette::Text, st::windowFg->c);
		roles.setColor(QPalette::Button, st::windowBgOver->c);
		roles.setColor(QPalette::ButtonText, st::windowFg->c);
		roles.setColor(QPalette::Highlight, st::activeButtonBg->c);
		roles.setColor(QPalette::HighlightedText, st::activeButtonFg->c);
		roles.setColor(QPalette::PlaceholderText, st::windowSubTextFg->c);
		roles.setColor(QPalette::Disabled, QPalette::Text, st::windowSubTextFg->c);
		setPalette(roles);
		setAutoFillBackground(true);
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
		_retired = true;
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
			_coverMessage->setText(u"Settings hidden while locked"_q);
			_coverClose->hide();
			_pages->setCurrentWidget(_covered);
			clearSheet();
		} else {
			const auto wasCovered = _pages->currentWidget() == _covered;
			QTimer::singleShot(0, this, [this, wasCovered] {
				if (!visibleContent()) return;
				refreshFromDisk();
				if (wasCovered && !_needsRefresh) {
					_resumeRefused = _draftStale || !_opening.current;
					if (_resumeRefused) {
						_coverMessage->setText(u"The source changed while settings were hidden. Close and reopen Customize to see the current settings."_q);
						_coverClose->show();
						_coverClose->setFocus();
					} else {
						_pages->setCurrentWidget(_browser);
						_tree->setFocus();
					}
				}
			});
		}
	}

	void capture() {
		const auto weak = QPointer(this);
		if (const auto host = _host.get()) {
			_opening = CaptureWindowSettingsEditor(host, [weak] {
				return weak && weak->visibleContent();
			});
		}
		if (_opening.current) {
			const auto key = _opening.requester;
			_accountId = AccountKeyText(key.environment, key.userId).value_or(QString());
		}
	}

	void refreshFromDisk() {
		if (!visibleContent()) return;
		if (Core::App().activeWindow() != _host.get()) {
			_needsRefresh = true;
			return;
		}
		_needsRefresh = false;
		if (_frames.empty()) {
			_draftStale = false;
			if (_editingKey.isEmpty()) {
				capture();
				rebuild();
			} else {
				_needsRefresh = true;
			}
			return;
		}
		const auto check = PrepareWindowSettingsEditorEdit(_opening, _frames.back().edits);
		_draftStale = !check.review;
		if (_draftStale) {
			AnnounceError(_feedback, StatusText(check));
			if (_pages->currentWidget() == _sheetPage) returnToTree();
		}
	}

	QString sourceName(const std::optional<AccountSettingsOpenTarget> &source) const {
		return source ? AccountName(source->file.account) : u"Unavailable source"_q;
	}

	void rebuild(bool revealSelection = false) {
		if (!visibleContent() || !_opening.current) return;
		const auto frame = _frames.empty() ? nullptr : &_frames.back();
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
		_rows = SettingsTree::Build(layers, files);
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
		_subject->setText(u"%1 · App-wide from %2"_q.arg(
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
				? u"\n"_q + Isolate(value) + u"  ·  "_q + row.source : QString()));
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
		return !_frames.empty() && std::any_of(_frames.back().edits.begin(), _frames.back().edits.end(),
			[&](const SettingsEdit &edit) { return edit.address == row.address; });
	}

	void refreshInspector() {
		const auto row = selected();
		_edit->setEnabled(row && row->editable);
		_reset->setEnabled(row && row->editable && SettingsTree::CanReset(*row->descriptor));
		_undo->setEnabled(!_frames.empty());
		_clear->setEnabled(filtering());
		if (!row) {
			_inspector->clear();
			return;
		}
		auto lines = QStringList();
		if (!row->address.empty()) lines.push_back(Isolate(SettingsTree::Address(row->address)));
		if (row->descriptor && SettingsTree::IsScalar(row->descriptor->codec)) {
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

	void startFilter() {
		_filterBeforeEdit = _filter->text();
		_filter->setFocus();
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
		if (!visibleContent()) return true;
		const auto mods = event->modifiers() & ~Qt::KeypadModifier;
		const auto key = event->key();
		const auto enter = key == Qt::Key_Return || key == Qt::Key_Enter;
		const auto cancel = key == Qt::Key_Escape || (key == Qt::Key_G && mods == kCtrlMod);
		if (_capture && _pages->currentWidget() == _sheetPage && object == _record) {
			if (key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Meta
				&& key != Qt::Key_Alt && key != Qt::Key_AltGr && key != Qt::Key_unknown
				&& !event->isAutoRepeat()) {
				const auto chord = QKeySequence(event->keyCombination());
				_line->setText(chord.toString(QKeySequence::PortableText));
				endCapture();
			}
			return true;
		}
		if (_preedit.contains(object) || _preedit.contains(QApplication::focusWidget())) return false;
		if (const auto button = qobject_cast<QPushButton*>(object); button && enter && !mods) {
			if (!event->isAutoRepeat()) button->click();
			return true;
		}
		if (object == _filter) {
			if (cancel) {
				_filter->setText(_filterBeforeEdit);
				_tree->setFocus();
				return true;
			}
			if (enter && !mods) {
				_filterBeforeEdit = _filter->text();
				_tree->setFocus();
				return true;
			}
			if ((key == Qt::Key_Down || key == Qt::Key_Up) && !mods) {
				selectSibling(key == Qt::Key_Down ? 1 : -1);
				return true;
			}
		}
		if (_pages->currentWidget() == _sheetPage) {
			if (_choice && _choice->view()->isVisible()) return false;
			const auto submit = enter && (mods == kCtrlMod || (!mods && object == _line));
			switch (SettingsTree::ShouldHandleEditorKey(
					_preedit.contains(object), submit, !_multi || mods == kCtrlMod, cancel)) {
			case EditorKey::Stage: if (!event->isAutoRepeat() && !_editingKey.isEmpty()) stageForm(); return true;
			case EditorKey::Back: back(); return true;
			case EditorKey::Native: return false;
			}
		}
		if (cancel) { back(); return true; }
		if (key == Qt::Key_F6) { changePanel(mods.testFlag(Qt::ShiftModifier)); return true; }
		if (key == Qt::Key_Tab && !mods && object == _inspector) {
			_tree->setFocus();
			return true;
		}
		if (object != _tree && object != this) return false;
		if (((key == Qt::Key_J || key == Qt::Key_Down) && !mods)
			|| (key == Qt::Key_N && mods == kCtrlMod)) selectSibling(1);
		else if (((key == Qt::Key_K || key == Qt::Key_Up) && !mods)
			|| (key == Qt::Key_P && mods == kCtrlMod)) selectSibling(-1);
		else if (key == Qt::Key_H && !mods) traverse(false);
		else if (key == Qt::Key_L && !mods) traverse(true);
		else if ((key == Qt::Key_Left || key == Qt::Key_Right) && !mods) {
			traverse((key == Qt::Key_Right) == (layoutDirection() != Qt::RightToLeft));
		} else if (key == Qt::Key_Tab && !mods) {
			if (const auto item = _tree->currentItem(); item && item->childCount()) {
				item->setExpanded(!item->isExpanded());
			} else {
				_inspector->setFocus();
			}
		} else if (enter && !mods && !event->isAutoRepeat()) activateRow();
		else if (key == Qt::Key_Slash && !mods) startFilter();
		else if (key == Qt::Key_Question) showHelp();
		else if (key == Qt::Key_R && !mods && !event->isAutoRepeat()) resetSelected();
		else if (key == Qt::Key_U && !mods && !event->isAutoRepeat()) undo();
		else if (key == Qt::Key_Q && !mods) back();
		else if (((key == Qt::Key_Y || key == Qt::Key_T || key == Qt::Key_Minus) && !mods)
			|| (key == Qt::Key_Plus && (!mods || mods == Qt::ShiftModifier))) {
			if (!event->isAutoRepeat()) editShortcut(key);
		} else return false;
		return true;
	}

	void activateRow() {
		const auto row = selected();
		if (!row || !visibleContent()) return;
		if (row->editable) {
			showForm(*row);
		} else if (const auto current = _tree->currentItem(); current && current->childCount()) {
			current->setExpanded(!current->isExpanded());
		} else {
			_inspector->setFocus();
		}
	}

	void addSheetWidget(QWidget *widget) {
		widget->installEventFilter(this);
		_sheetLayout->addWidget(widget);
	}

	void clearSheet() {
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
		while (const auto item = _sheetLayout->takeAt(0)) {
			if (const auto widget = item->widget()) {
				widget->hide();
				widget->deleteLater();
			}
			delete item;
		}
	}

	void returnToTree() {
		clearSheet();
		if (_resumeRefused) {
			_pages->setCurrentWidget(_covered);
			_coverClose->setFocus();
			return;
		}
		_pages->setCurrentWidget(_browser);
		_tree->setFocus();
		if (_needsRefresh) refreshFromDisk();
	}

	void showHelp() {
		clearSheet();
		const auto title = Label(_sheet, u"Customize settings help"_q);
		title->setFont(st::keysLegendTitleStyle.font);
		addSheetWidget(title);
		const auto row = selected();
		if (row && !row->help.isEmpty()) addSheetWidget(Label(_sheet, row->help));
		addSheetWidget(Label(_sheet, u"j / k Move · h / l Fold or enter · / Filter · Return Edit\n"
			"t Toggle · + / − Step · r Reset · u Undo · y Copy TOML path\n"
			"F6 Change panel · Esc Back\nEdits are staged in memory."_q));
		const auto close = Button(_sheet, u"Back"_q);
		addSheetWidget(close);
		connect(close, &QPushButton::clicked, this, [this] { returnToTree(); });
		_sheetLayout->addStretch();
		_pages->setCurrentWidget(_sheetPage);
		close->setFocus();
	}

	void showForm(const Row &row) {
		clearSheet();
		_editingKey = row.key;
		const auto title = Label(_sheet, row.label);
		title->setFont(st::keysLegendTitleStyle.font);
		addSheetWidget(title);
		addSheetWidget(Label(_sheet, Isolate(SettingsTree::Address(row.address)) + u" · "_q + row.destination));
		addSheetWidget(Label(_sheet, row.help));
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
		const auto stageButton = Button(_sheet, u"Stage"_q);
		addSheetWidget(stageButton);
		connect(stageButton, &QPushButton::clicked, this, [this] { stageForm(); });
		if (descriptor.codec != SettingCodec::Boolean && SettingsTree::CanReset(descriptor)) {
			const auto reset = Button(_sheet, row.resetLabel);
			addSheetWidget(reset);
			connect(reset, &QPushButton::clicked, this, [this] { resetSelected(); });
		}
		const auto cancel = Button(_sheet, u"Cancel"_q);
		addSheetWidget(cancel);
		connect(cancel, &QPushButton::clicked, this, [this] { returnToTree(); });
		_sheetLayout->addStretch();
		_pages->setCurrentWidget(_sheetPage);
		focus->setFocus();
	}

	void addStepControls(const Row &row) {
		for (const auto delta : { -1, 1 }) {
			const auto button = Button(_sheet, delta < 0 ? u"Decrease"_q : u"Increase"_q);
			addSheetWidget(button);
			connect(button, &QPushButton::clicked, this, [this, descriptor = row.descriptor, delta] {
				const auto value = SettingsTree::Decode(*descriptor, _line->text());
				if (!value) return;
				const auto current = std::get_if<int64>(&*value)
					? QVariant::fromValue(std::get<int64>(*value))
					: QVariant(ParseDuration(std::get<QString>(*value)).value_or(0));
				if (const auto stepped = SettingsTree::Step(*descriptor, current, delta)) {
					_line->setText(std::get_if<int64>(&*stepped)
						? QString::number(std::get<int64>(*stepped)) : std::get<QString>(*stepped));
				}
			});
		}
	}

	void addDurationControls(const SettingDescriptor &descriptor) {
		const auto controls = new QWidget(_sheet);
		const auto layout = new QHBoxLayout(controls);
		layout->setContentsMargins({});
		for (const auto &[label, suffix, seconds] : {
			std::tuple{ u"Seconds"_q, u"s"_q, 1 },
			std::tuple{ u"Minutes"_q, u"m"_q, 60 },
			std::tuple{ u"Hours"_q, u"h"_q, 3600 },
		}) {
			const auto button = Button(controls, label);
			layout->addWidget(button);
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
		layout->addWidget(off);
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

	void stage(const Row &row, std::optional<SettingScalar> value) {
		if (!visibleContent()) return;
		auto edits = _frames.empty() ? std::vector<SettingsEdit>() : _frames.back().edits;
		const auto existing = std::find_if(edits.begin(), edits.end(), [&](const SettingsEdit &edit) {
			return edit.address == row.address;
		});
		if (existing == edits.end() && !_frames.empty()
			&& _frames.back().review->preview().route == SettingsEditorRoute::Peek) {
			AnnounceError(_formStatus ? _formStatus : _feedback, u"Stage each Peek timing change separately."_q);
			return;
		}
		auto change = SettingsEdit{ row.address, std::move(value) };
		if (existing == edits.end()) edits.push_back(std::move(change));
		else *existing = std::move(change);
		auto prepared = PrepareWindowSettingsEditorEdit(_opening, edits);
		if (prepared.status == Status::Unchanged) {
			_frames.clear();
		} else if (!prepared.review) {
			AnnounceError(_formStatus ? _formStatus : _feedback, StatusText(prepared));
			return;
		} else {
			if (!_frames.empty() && _frames.back().review->preview().after == prepared.review->preview().after) {
				returnToTree();
				return;
			}
			_frames.push_back({ std::move(edits), std::move(prepared.review) });
		}
		_anchor = filtering() ? row.key : QString();
		_selection = row.key;
		returnToTree();
		rebuild();
		describeDraft(prepared.status);
	}

	void describeDraft(Status status = Status::Ready) {
		_feedback->setText(_frames.empty() ? u"No staged edits."_q
			: status == Status::IncompleteTrace ? u"Staged. Some effective reads could not be verified."_q
			: u"%1 staged · %2"_q.arg(_frames.back().edits.size()).arg(_frames.back().review->preview().path));
	}

	void undo() {
		if (!_frames.empty()) {
			_frames.pop_back();
			rebuild();
		}
		describeDraft();
	}

	void back() {
		if (_pages->currentWidget() == _sheetPage) {
			returnToTree();
		} else if (!visibleContent() || _frames.empty()) {
			closeLayer();
		} else {
			clearSheet();
			addSheetWidget(Label(_sheet, u"Discard staged changes and close?"_q));
			const auto keep = Button(_sheet, u"Keep editing"_q);
			const auto discard = Button(_sheet, u"Discard and close"_q);
			addSheetWidget(keep);
			addSheetWidget(discard);
			connect(keep, &QPushButton::clicked, this, [this] { returnToTree(); });
			connect(discard, &QPushButton::clicked, this, [this] { closeLayer(); });
			_sheetLayout->addStretch();
			_pages->setCurrentWidget(_sheetPage);
			keep->setFocus();
		}
	}

	struct Draft {
		std::vector<SettingsEdit> edits;
		std::shared_ptr<SettingsEditorReview> review;
	};

	base::weak_ptr<Window::Controller> _host;
	base::weak_ptr<Main::Session> _session;
	QPointer<::MainWindow> _window;
	QStackedWidget *_pages;
	QWidget *_browser;
	QScrollArea *_sheetPage;
	QWidget *_sheet;
	QWidget *_covered;
	QLabel *_coverMessage;
	QPushButton *_coverClose;
	QLabel *_title;
	QLabel *_subject;
	QLineEdit *_filter;
	QPushButton *_clear;
	QTreeWidget *_tree;
	QLabel *_inspector;
	QLabel *_feedback;
	QPushButton *_edit;
	QPushButton *_reset;
	QPushButton *_undo;
	QPushButton *_help;
	QVBoxLayout *_sheetLayout;
	SettingsEditorOpening _opening;
	std::vector<Row> _rows;
	std::vector<Draft> _frames;
	QSet<QString> _expanded;
	QSet<QObject*> _preedit;
	QString _editingKey;
	QString _selection;
	QString _anchor;
	QString _accountId;
	QString _filterBeforeEdit;
	QLineEdit *_line = nullptr;
	QPlainTextEdit *_multi = nullptr;
	QComboBox *_choice = nullptr;
	QButtonGroup *_bool = nullptr;
	QLabel *_formStatus = nullptr;
	QPushButton *_record = nullptr;
	QPushButton *_cancelCapture = nullptr;
	bool _capture = false;
	bool _needsRefresh = false;
	bool _retired = false;
	bool _draftStale = false;
	bool _resumeRefused = false;
};

} // namespace

bool CanCustomizeSettings(Window::Controller *window) {
	return !Protected(window) && window->maybeSession()
		&& !window->maybeSession()->account().loggingOut();
}

void ShowCustomizeSettings(not_null<Window::Controller*> window) {
	if (!CanCustomizeSettings(window.get()) || Core::App().activeWindow() != window.get()) {
		return;
	}
	const auto host = window->widget();
	if (const auto existing = OpenEditors.value(host.get())) {
		existing->setInnerFocus();
		return;
	}
	if (!OpenEditors.contains(host.get())) {
		QObject::connect(host.get(), &QObject::destroyed, [raw = host.get()] {
			OpenEditors.remove(raw);
			LastSelection.remove(raw);
		});
	}
	auto layer = std::make_unique<CustomizeLayer>(window);
	layer->open();
	OpenEditors[host.get()] = layer.get();
	window->showLayer(std::move(layer), Ui::LayerOption::KeepOther);
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
			if (shortcut) event->accept();
			return shortcut;
		}
	}
	return std::nullopt;
}

} // namespace Purple
