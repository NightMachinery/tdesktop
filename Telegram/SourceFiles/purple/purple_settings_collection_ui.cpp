#include "purple/purple_settings_collection_ui.h"

#include "purple/purple_keys.h"
#include "purple/purple_keys_mx.h"
#include "purple/purple_settings_tree.h"
#include "purple/purple_settings_focus.h"
#include "purple/purple_settings_customize_look.h"

#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtCore/QTimeZone>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

namespace Purple {
namespace {

namespace Structures = SettingsStructures;
using Op = SettingsCollectionOp;

#ifdef Q_OS_MAC
constexpr auto kCtrlMod = Qt::MetaModifier;
#else
constexpr auto kCtrlMod = Qt::ControlModifier;
#endif // Q_OS_MAC

class Collection final : public QWidget {
public:
	Collection(QWidget *parent, SettingsCollectionConfig config)
	: QWidget(parent)
	, _config(std::move(config))
	, _candidate(_config.bytes)
	, _members(_config.members)
	, _parts(new QStackedWidget(this))
	, _main(new QWidget(_parts))
	, _lists(new QBoxLayout(QBoxLayout::LeftToRight))
	, _form(new QWidget(_parts))
	, _formLayout(new QVBoxLayout(_form))
	, _list(new QListWidget(_main))
	, _search(new QLineEdit(_main))
	, _peerList(new QListWidget(_main))
	, _effects(new QLabel(_main))
	, _status(new QLabel(_main)) {
		const auto shell = new QVBoxLayout(this);
		shell->setContentsMargins({});
		shell->addWidget(_parts);
		_parts->addWidget(_main);
		_parts->addWidget(_form);
		const auto main = new QVBoxLayout(_main);
		const auto gap = fontMetrics().height();
		main->setContentsMargins(0, gap / 3, 0, gap / 3);
		main->setSpacing(gap / 3);
		_formLayout->setContentsMargins(0, gap / 3, 0, gap / 3);
		_formLayout->setSpacing(gap / 3);
		_formLayout->setAlignment(Qt::AlignTop);
		_effects->setProperty("secondary", true);
		for (const auto list : { _list, _peerList }) {
			CustomizeLook::ConfigureCollectionList(list);
		}
		_list->setAccessibleName(u"Selected ordered entries"_q);
		_peerList->setAccessibleName(u"Available chats for this settings source"_q);
		_search->setPlaceholderText(u"Filter available chats"_q);
		_search->setAccessibleName(u"Filter available chats"_q);
		if (_config.peerField) main->addWidget(_search);
		else {
			_search->hide();
			_peerList->hide();
		}
		if (_config.members && _config.peerField) {
			const auto column = [&](QListWidget *list, const QString &title) {
				const auto layout = new QVBoxLayout();
				layout->addWidget(new QLabel(title, _main));
				layout->addWidget(list);
				_lists->addLayout(layout);
			};
			column(_list, u"Members"_q);
			column(_peerList, u"Available chats"_q);
		} else {
			_lists->addWidget(_list);
			if (_config.peerField) _lists->addWidget(_peerList);
		}
		main->addLayout(_lists);
		for (const auto label : { _effects, _status }) {
			label->setWordWrap(true);
			label->setTextFormat(Qt::PlainText);
			label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
			main->addWidget(label);
		}
		const auto operations = new CustomizeLook::Actions(_main);
		const auto ordering = new CustomizeLook::Actions(_main);
		const auto footer = new CustomizeLook::Actions(_main);
		main->addWidget(operations);
		main->addWidget(ordering);
		main->addWidget(footer);
		button(operations, u"Add"_q, ModalKeysEnabled() ? u"a"_q : QString(), [this] { add(); });
		if (_config.peerField) {
			button(operations, u"Enter ID"_q, ModalKeysEnabled() ? u"i"_q : QString(), [this] { edit(Op::Insert); });
		}
		button(operations, u"Edit"_q, [this] { activate(); });
		if (!_config.members) {
			button(operations, u"Duplicate"_q, ModalKeysEnabled() ? u"Shift+D"_q : QString(), [this] { edit(Op::Duplicate); });
		}
		if (_config.named) button(operations, u"Rename"_q, ModalKeysEnabled() ? u"Shift+R"_q : QString(), [this] { edit(Op::Rename); });
		button(operations, u"Delete"_q, ModalKeysEnabled() ? u"x"_q : QString(), [this] { edit(Op::Erase); });
		button(ordering, u"Move up"_q, ModalKeysEnabled() ? u"Shift+K"_q : QString(), [this] { move(-1); });
		button(ordering, u"Move down"_q, ModalKeysEnabled() ? u"Shift+J"_q : QString(), [this] { move(1); });
		button(ordering, u"Undo"_q, ModalKeysEnabled() ? u"u"_q : QString(), [this] { undo(); });
		if (_config.members) {
			button(ordering, u"Reset members"_q, ModalKeysEnabled() ? u"r"_q : QString(),
				[this] { resetMembers(); });
		}
		button(footer, u"Stage collection"_q, u"C-Enter"_q, [this] { stage(); });
		button(footer, u"Back"_q, u"Esc"_q, [this] { back(); });
		for (const auto widget : { static_cast<QWidget*>(_list), static_cast<QWidget*>(_peerList),
				static_cast<QWidget*>(_search) }) {
			widget->installEventFilter(this);
		}
		connect(_list, &QListWidget::itemDoubleClicked, this, [this] { activate(); });
		connect(_list, &QListWidget::currentRowChanged, this, [this] { effect(); });
		connect(_search, &QLineEdit::textChanged, this, [this] { refilter(); });
		connect(_peerList, &QListWidget::itemDoubleClicked, this, [this] { addPeer(); });
		_list->setDragDropMode(QAbstractItemView::InternalMove);
		_list->setDefaultDropAction(Qt::MoveAction);
		connect(_list->model(), &QAbstractItemModel::rowsMoved, this,
			[this](const QModelIndex &, int from, int last, const QModelIndex &, int destination) {
				if (from != last || !_config.current()) return;
				const auto to = destination > from ? destination - 1 : destination;
				const auto generation = _generation;
				QTimer::singleShot(0, this, [this, from, to, generation] {
					if (_generation == generation && _config.current()) moveTo(from, to);
				});
			});
		setFocusProxy(_list);
		connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
			if (!now) return;
			if (_main->isAncestorOf(now)) {
				_mainFocus = now;
			} else if (_form->isAncestorOf(now) && !_exitPrompt
				&& !qobject_cast<QPushButton*>(now)) {
				_formField = now;
			}
		});
		if (!_config.seed.empty()) {
			const auto result = ApplySettingsEdits(_candidate, MapSettingsSource(_candidate).revision, _config.seed);
			if (result.ok()) {
				_candidate = result.bytes;
				_operations = _config.seed;
			}
		}
		render(0);
		refilter();
	}

	void back() {
		if (_parts->currentWidget() == _form) {
			if (_formInput && _formInput() != _formBaseline && !_exitPrompt) {
				_exitFocus = SettingsFocus(_formField ? _formField.data() : QApplication::focusWidget());
				const auto prompt = new QWidget(_form);
				const auto layout = new QVBoxLayout(prompt);
				layout->setContentsMargins({});
				layout->addWidget(new QLabel(u"Discard unsubmitted input?"_q, prompt));
				const auto keep = button(layout, u"Keep editing"_q, [this] { cancelExitPrompt(); });
				button(layout, u"Discard input"_q, [this] { mainPage(); });
				_exitPrompt = prompt;
				field(prompt);
				keep->setFocus();
			} else if (_exitPrompt) {
				cancelExitPrompt();
			} else {
				mainPage();
			}
		} else if (_history.empty()) {
			const auto close = _config.close;
			if (close) close();
		} else {
			clearForm();
			label(u"Discard unsubmitted collection changes?"_q);
			const auto keep = button(_formLayout, u"Keep editing"_q, [this] { back(); });
			button(_formLayout, u"Discard collection changes"_q, [this] {
				const auto close = _config.close;
				if (close) close();
			});
			_parts->setCurrentWidget(_form);
			keep->setFocus();
		}
	}

	void refreshPeers(bool cleared) {
		if (!_config.peerField) return;
		if (cleared) std::erase_if(_config.peers, [](const auto &choice) { return bool(choice.current); });
		else std::erase_if(_config.peers, [](const auto &choice) { return choice.current && !choice.current(); });
		render(_list->currentRow());
		refilter();
	}

	[[nodiscard]] bool hasPreedit() const {
		return _preedit.contains(QApplication::focusWidget());
	}

protected:
	void resizeEvent(QResizeEvent *event) override {
		QWidget::resizeEvent(event);
		_lists->setDirection(CustomizeLook::CollectionListDirection(width(), fontMetrics()));
	}

	bool eventFilter(QObject *object, QEvent *event) override {
		if (event->type() == QEvent::InputMethod) {
			const auto input = static_cast<QInputMethodEvent*>(event);
			if (input->preeditString().isEmpty()) _preedit.remove(object);
			else _preedit.insert(object);
			return false;
		}
		if (event->type() == QEvent::FocusOut) _preedit.remove(object);
		if (event->type() != QEvent::KeyPress || _preedit.contains(object)) return false;
		const auto key = static_cast<QKeyEvent*>(event);
		const auto mods = key->modifiers() & ~Qt::KeypadModifier;
		const auto code = ModalPhysicalKey(*key);
		const auto enter = code == Qt::Key_Return || code == Qt::Key_Enter;
		if (!_config.current()) return true;
		if (code == Qt::Key_Escape || (code == Qt::Key_G && mods == kCtrlMod)) {
			if (AllowModalLayerKey(*key, this)) back();
			return true;
		}
		if (enter && mods == kCtrlMod) {
			if (AllowModalLayerKey(*key, this)) {
				if (_parts->currentWidget() == _main) stage();
				else if (_accept) _accept->click();
			}
			return true;
		}
		if (enter && !mods) {
			if (!qobject_cast<QPushButton*>(object) && object != _list
				&& object != _peerList && object != _search) {
				return false;
			}
			if (!AllowModalLayerKey(*key, this)) return true;
			if (const auto button = qobject_cast<QPushButton*>(object)) button->click();
			else if (object == _list) activate();
			else if (object == _peerList) addPeer();
			else _peerList->setFocus();
			return true;
		}
		const auto list = qobject_cast<QListWidget*>(object);
		if (!list && (!qobject_cast<QPushButton*>(object)
			|| _parts->currentWidget() != _main)) {
			return false;
		}
		const auto modal = ModalKeysEnabled();
		const auto down = (!mods && (code == Qt::Key_Down || (modal && code == Qt::Key_J)))
			|| (mods == kCtrlMod && code == Qt::Key_N);
		const auto up = (!mods && (code == Qt::Key_Up || (modal && code == Qt::Key_K)))
			|| (mods == kCtrlMod && code == Qt::Key_P);
		if (list && (down || up)) {
			if (AllowModalLayerKey(*key, this, true)) {
				list->setCurrentRow(std::clamp(list->currentRow() + (down ? 1 : -1),
					0, std::max(0, list->count() - 1)));
			}
			return true;
		}
		if (!modal) return false;
		if ((code == Qt::Key_J || code == Qt::Key_K)
			&& mods == Qt::ShiftModifier && list != _peerList) {
			if (AllowModalLayerKey(*key, this)) move(code == Qt::Key_J ? 1 : -1);
			return true;
		}
		const auto once = [&] { return AllowModalLayerKey(*key, this); };
		if (code == Qt::Key_D && mods == Qt::ShiftModifier && !_config.members) { if (once()) edit(Op::Duplicate); }
		else if (code == Qt::Key_R && mods == Qt::ShiftModifier && _config.named) { if (once()) edit(Op::Rename); }
		else if (code == Qt::Key_I && !mods && _config.peerField) { if (once()) edit(Op::Insert); }
		else if (code == Qt::Key_R && !mods && _config.members) { if (once()) resetMembers(); }
		else if (code == Qt::Key_A && !mods) { if (once()) add(); }
		else if (code == Qt::Key_X && !mods) { if (once()) edit(Op::Erase); }
		else if (code == Qt::Key_U && !mods) { if (once()) undo(); }
		else if (code == Qt::Key_Slash && !mods && _config.peerField) { if (once()) _search->setFocus(); }
		else if (code == Qt::Key_Space && !mods && list == _peerList) { if (once()) addPeer(); }
		else return false;
		return true;
	}

private:
	void resetMembers() {
		auto value = _members.value();
		value.members = std::nullopt;
		commit(std::move(value));
	}

	template <typename Layout>
	QPushButton *button(Layout *layout, const QString &label, Fn<void()> run) {
		return button(layout, label, QString(), std::move(run));
	}

	template <typename Layout>
	QPushButton *button(
			Layout *layout,
			const QString &label,
			QString shortcut,
			Fn<void()> run) {
		const auto result = new CustomizeLook::Action(label, this, std::move(shortcut));
		if constexpr (std::is_same_v<Layout, CustomizeLook::Actions>) {
			layout->append(result);
		} else {
			CustomizeLook::AppendAction(layout, result);
		}
		result->installEventFilter(this);
		connect(result, &QPushButton::clicked, this, std::move(run));
		return result;
	}

	void field(QWidget *widget) {
		widget->installEventFilter(this);
		_formLayout->addWidget(widget);
	}

	void label(const QString &text) {
		const auto result = new QLabel(text, _form);
		result->setTextFormat(Qt::PlainText);
		result->setWordWrap(true);
		result->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		_formLayout->addWidget(result);
	}

	void clearForm() {
		_accept.clear();
		_exitPrompt.clear();
		_formField.clear();
		_exitFocus = SettingsFocus();
		_formInput = {};
		_formBaseline.clear();
		while (const auto item = _formLayout->takeAt(0)) {
			if (const auto widget = item->widget()) {
				widget->hide();
				widget->deleteLater();
			}
			delete item;
		}
	}

	void cancelExitPrompt() {
		if (_exitPrompt) {
			_exitPrompt->hide();
			_exitPrompt->deleteLater();
			_exitPrompt.clear();
		}
		if (!_exitFocus.restore(_form) && _accept) _accept->setFocus();
	}

	void mainPage() {
		clearForm();
		const auto focus = SettingsFocus(_mainFocus);
		_parts->setCurrentWidget(_main);
		if (!focus.restore(_main)) _list->setFocus();
	}

	const SettingsPeerChoice *peer(const QString &id) const {
		const auto found = std::find_if(_config.peers.begin(), _config.peers.end(), [&](const auto &value) {
			return value.literal == id && (!value.current || value.current());
		});
		return found != _config.peers.end() ? &*found : nullptr;
	}

	void refilter() {
		const auto terms = ModalMx::ParseTokens(_search->text());
		const auto selected = _peerList->currentItem()
			? _peerList->currentItem()->data(Qt::UserRole).toString() : QString();
		_peerList->clear();
		for (const auto &choice : _config.peers) {
			if (choice.current && !choice.current()) continue;
			const auto name = choice.label.toCaseFolded();
			if (!std::all_of(terms.begin(), terms.end(), [&](const auto &term) {
					return ModalMx::MatchToken(term, name, {});
				})) continue;
			const auto entry = new QListWidgetItem(choice.avatar, choice.label, _peerList);
			entry->setData(Qt::UserRole, choice.literal);
			entry->setToolTip(choice.label);
			if (choice.literal == selected) _peerList->setCurrentItem(entry);
		}
		if (!_peerList->currentItem() && _peerList->count()) _peerList->setCurrentRow(0);
	}

	void render(int selection) {
		const auto blocker = QSignalBlocker(_list);
		_list->clear();
		_items = Structures::Items(_candidate, _config.address, _config.named);
		if (_members) {
			_items.clear();
			const auto &members = _members->members.value_or(std::vector<PeerIdValue>());
			for (const auto id : members) {
				auto path = _config.address;
				path.emplace_back(_items.size());
				_items.push_back({ .address = std::move(path), .value = QString::number(id) });
			}
		}
		for (const auto &item : _items) {
			const auto choice = (_config.peerField && !item.value.isEmpty()) ? peer(item.value) : nullptr;
			const auto name = (_config.peerField && !item.value.isEmpty())
				? Structures::PeerLabel(item.value, choice ? choice->label : QString(), choice != nullptr)
				: item.label;
			const auto entry = new QListWidgetItem(choice ? choice->avatar : QIcon(),
				item.recognized ? name : u"Skipped value · "_q + name, _list);
			entry->setToolTip(item.line ? u"%1\nSource line %2"_q.arg(entry->text()).arg(item.line) : entry->text());
		}
		if (!_items.empty()) _list->setCurrentRow(std::clamp(selection, 0, int(_items.size()) - 1));
		effect();
	}

	void effect() {
		const auto index = _list->currentRow();
		if (index < 0 || index >= int(_items.size())) {
			_effects->clear();
			return;
		}
		const auto &item = _items[index];
		auto message = Structures::RecordEffect(_candidate, item.address);
		if (!item.recognized) {
			message = u"The reader may skip this record. Its original position is preserved."_q;
		} else if (message.isEmpty()) {
			if (_config.named) message = SettingsTree::Address(item.address);
			else if (!_config.peerField && !item.literal.isEmpty()) {
				message = QString::fromUtf8(item.literal).left(240);
			}
		}
		const auto descriptor = FindSettingDescriptor(_config.address);
		if (descriptor && (descriptor->id == SettingId::PresetListOrder
			|| descriptor->id == SettingId::PresetFolders || descriptor->id == SettingId::Rulesets
			|| descriptor->id == SettingId::FlatRules || descriptor->id == SettingId::NestedRules)) {
			message += u"\nFirst match takes precedence in the displayed order."_q;
		}
		_effects->setText(message);
	}

	void activate() {
		const auto at = _list->currentRow();
		if (at < 0 || at >= int(_items.size())) return;
		if (_config.named || !_items[at].inlineValue) {
			if (!_history.empty()) {
				_status->setText(u"Stage these collection changes before opening the record fields."_q);
				return;
			}
			const auto inspect = _config.inspect;
			const auto path = _items[at].address;
			if (inspect) inspect(path);
		} else if (!_config.peerField) {
			edit(Op::Replace);
		}
	}

	void addPeer() {
		if (const auto item = _peerList->currentItem()) {
			const auto id = item->data(Qt::UserRole).toString();
			if (peer(id)) appendPeer(id);
			else refreshPeers(false);
		} else {
			_status->setText(u"Select an available chat or choose Enter ID."_q);
		}
	}

	void add() {
		if (_config.peerField) addPeer();
		else edit(Op::Insert);
	}

	void appendPeer(const QString &text) {
		if (!_config.peerField || !_config.current()) return;
		const auto value = Structures::ParsePeerLiteral(*_config.peerField, text);
		if (!value) {
			_status->setText(u"Enter a valid ID for this setting."_q);
			return;
		}
		const auto id = std::holds_alternative<int64>(*value)
			? QString::number(std::get<int64>(*value)) : std::get<QString>(*value);
		if (_config.members && !std::holds_alternative<int64>(*value)) return;
		if (std::any_of(_items.begin(), _items.end(), [&](const auto &other) { return other.value == id; })) {
			_status->setText(u"This entry is already selected."_q);
			return;
		}
		if (_members) {
			auto next = *_members;
			if (!next.members) next.members.emplace();
			next.members->push_back(std::get<int64>(*value));
			commit(std::move(next));
		} else {
			run({ Structures::ArrayEdit(_config.address, Op::Insert, _items.size(), 0, *value) }, int(_items.size()));
		}
		mainPage();
	}

	void move(int delta) {
		moveTo(_list->currentRow(), _list->currentRow() + delta);
	}

	void moveTo(int from, int to) {
		if (from < 0 || to < 0 || from >= int(_items.size()) || to >= int(_items.size()) || from == to) {
			render(from);
			return;
		}
		if (_members && _members->members) {
			auto next = *_members;
			auto &ordered = *next.members;
			const auto id = ordered[from];
			ordered.erase(ordered.begin() + from);
			ordered.insert(ordered.begin() + to, id);
			commit(std::move(next), to);
		} else if (_config.named) {
			run({ { _items[from].address, SettingsObjectEdit{ .operation = Op::Move, .destination = size_t(to) } } }, to);
		} else {
			run({ Structures::ArrayEdit(_config.address, Op::Move, from, to) }, to);
		}
	}

	Fn<QString()> recordField(const Structures::RecordField &spec) {
		using Kind = Structures::FieldKind;
		if (spec.kind == Kind::Boolean) {
			const auto fieldWidget = new QCheckBox(spec.label, _form);
			fieldWidget->setChecked(spec.initial == u"true"_q);
			field(fieldWidget);
			return [fieldWidget] { return fieldWidget->isChecked() ? u"true"_q : u"false"_q; };
		}
		label(spec.label);
		if (spec.kind == Kind::Weekdays) {
			const auto dayRow = new QWidget(_form);
			const auto layout = new QHBoxLayout(dayRow);
			layout->setContentsMargins({});
			auto days = std::vector<QCheckBox*>();
			for (const auto day : { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" }) {
				const auto chip = new QCheckBox(QString::fromLatin1(day), dayRow);
				layout->addWidget(chip);
				chip->installEventFilter(this);
				days.push_back(chip);
			}
			field(dayRow);
			return [days] {
				auto values = QStringList();
				for (const auto day : days) if (day->isChecked()) values.push_back(day->text().toLower());
				return values.join(u',');
			};
		}
		if (spec.kind == Kind::Choice || spec.kind == Kind::TimeZone) {
			const auto choices = new QComboBox(_form);
			choices->setAccessibleName(spec.label);
			if (spec.kind == Kind::TimeZone) {
				choices->setEditable(true);
				choices->setInsertPolicy(QComboBox::NoInsert);
				choices->lineEdit()->installEventFilter(this);
				for (const auto &zone : QTimeZone::availableTimeZoneIds()) {
					choices->addItem(QString::fromUtf8(zone));
				}
			} else {
				choices->addItems(spec.choices);
			}
			choices->setCurrentText(spec.initial);
			field(choices);
			return [choices] { return choices->currentText(); };
		}
		const auto input = new QLineEdit(spec.initial, _form);
		input->setAccessibleName(spec.label);
		field(input);
		return [input] { return input->text(); };
	}

	void showOtherReferences(const SettingAddress &object) {
		const auto reports = _config.otherReferences
			? _config.otherReferences(object) : std::vector<SettingsReferenceReport>();
		for (const auto &report : reports) {
			const auto role = report.role == SettingsOpenRole::Own
				? u"This account’s settings"_q : u"App-wide settings"_q;
			label(report.path.isEmpty() ? role : u"%1 · %2"_q.arg(role, report.path));
			switch (report.status) {
			case SettingsReferenceStatus::Unavailable:
				label(u"This opening has no read access to that file."_q);
				break;
			case SettingsReferenceStatus::Missing:
				label(u"The file was absent at opening."_q);
				break;
			case SettingsReferenceStatus::Unreadable:
				label(u"The file could not be read. Its references were not checked."_q);
				break;
			case SettingsReferenceStatus::Invalid:
				label(u"The file could not be parsed. Its references were not checked."_q);
				break;
			case SettingsReferenceStatus::Checked:
				if (report.usesRecovery) label(u"The check used the captured last-good copy."_q);
				for (const auto &address : report.references) label(SettingsTree::Address(address));
				if (report.memberTable) {
					label(u"Stored member table · %1"_q.arg(SettingsTree::Address(*report.memberTable))
						+ (report.ignoredInFollow ? u" · Inactive in Follow mode"_q : QString()));
				}
				if (report.references.empty() && !report.memberTable) {
					label(u"No recognized references were found in this file."_q);
				}
				break;
			}
		}
		label(u"This draft can change its destination file only. Other accounts’ files are not checked."_q);
	}

	void edit(Op op) {
		if (!_config.current()) return;
		const auto at = _list->currentRow();
		const auto insert = op == Op::Insert;
		if (!insert && (at < 0 || at >= int(_items.size()))) return;
		if (_config.peerField && op == Op::Duplicate) {
			_status->setText(u"Peer lists contain unique entries."_q);
			return;
		}
		const auto descriptor = FindSettingDescriptor(_config.address);
		if (!descriptor) {
			_status->setText(u"This collection no longer has an editable settings descriptor."_q);
			return;
		}
		const auto id = descriptor->id;
		const auto item = insert ? Structures::Item() : _items[at];
		const auto objectDescriptor = (_config.named && !insert) ? FindSettingDescriptor(item.address) : nullptr;
		if (_config.named && !insert && !objectDescriptor) {
			_status->setText(u"This record does not have an editable settings descriptor."_q);
			return;
		}
		const auto objectId = objectDescriptor ? objectDescriptor->id : SettingId::Count;
		clearForm();
		auto name = static_cast<QLineEdit*>(nullptr);
		auto literal = static_cast<QPlainTextEdit*>(nullptr);
		const auto namedCopy = Structures::NeedsCopyName(id);
		const auto needsName = (_config.named && (insert || op == Op::Rename || op == Op::Duplicate))
			|| (op == Op::Duplicate && namedCopy);
		auto uses = (_config.named && (op == Op::Rename || op == Op::Erase))
			? Structures::RewriteReferences(_candidate, item.address, {}) : Structures::ReferenceChanges();
		if (op == Op::Erase) label(u"Remove %1 and its fields from this collection?"_q.arg(item.label));
		if (!uses.uses.empty()) {
			label(u"Same-file references"_q);
			for (const auto &address : uses.uses) label(SettingsTree::Address(address));
			if (op == Op::Erase) label(u"Choose the name that replaces these references before removal."_q);
		}
		if (_config.named && (op == Op::Rename || op == Op::Erase)) {
			showOtherReferences(item.address);
		}
		if (needsName || (op == Op::Erase && !uses.uses.empty()) || (insert && _config.peerField)) {
			name = new QLineEdit(_form);
			name->setAccessibleName(insert && _config.peerField ? u"Recorded peer ID"_q : u"Name"_q);
			name->setPlaceholderText(insert && _config.peerField ? u"Enter an ID"_q : u"Name"_q);
			field(name);
		}
		auto readers = std::vector<Fn<QString()>>();
		const auto editingClock = id == SettingId::WorldClocks && op == Op::Replace;
		if ((insert && !_config.peerField) || editingClock) {
			auto fields = Structures::RecordFields(id);
			if (editingClock) {
				const auto parsed = ParseSettings(
					u"privacy_screen.world_clocks = [%1]"_q.arg(QString::fromUtf8(item.literal)),
					QString());
				if (!parsed.settings.worldClocks.empty()) {
					fields[0].initial = parsed.settings.worldClocks.front().zone;
					fields[1].initial = parsed.settings.worldClocks.front().label;
				}
			}
			for (const auto &spec : fields) {
				readers.push_back(recordField(spec));
			}
		}
		if ((insert && !_config.peerField && readers.empty())
			|| (op == Op::Replace && !editingClock)) {
			literal = new QPlainTextEdit(QString::fromUtf8(insert
				? Structures::RecordTemplate(id) : item.literal), _form);
			literal->setAccessibleName(u"One TOML value or inline record"_q);
			field(literal);
		}
		auto error = new QLabel(_form);
		error->setTextFormat(Qt::PlainText);
		error->setWordWrap(true);
		field(error);
		const auto liveFields = [readers] {
			auto values = QStringList();
			for (const auto &read : readers) values.push_back(read());
			return values;
		};
		_formInput = [name, literal, liveFields] {
			auto values = liveFields();
			if (name) values.push_back(name->text());
			if (literal) values.push_back(literal->toPlainText());
			return values;
		};
		_formBaseline = _formInput();
		const auto cancel = button(_formLayout, u"Cancel"_q, u"Esc"_q, [this] { back(); });
		_accept = button(_formLayout, op == Op::Erase ? u"Remove from draft"_q : u"Keep change"_q,
			u"C-Enter"_q, [this, op, at, item, name, literal, error, insert, id, objectId, liveFields] {
				if (!_config.current()) return;
				const auto contents = name ? name->text().trimmed() : QString();
				if (insert && _config.peerField) {
					const auto value = Structures::ParsePeerLiteral(*_config.peerField, contents);
					if (value) appendPeer(contents);
					else error->setText(u"Enter a valid ID for this field."_q);
					return;
				}
				if (_members && op == Op::Erase) {
					auto next = *_members;
					if (next.members && at < int(next.members->size())) {
						next.members->erase(next.members->begin() + at);
						commit(std::move(next), at);
					}
					mainPage();
					return;
				}
				auto edits = std::vector<SettingsEdit>();
				auto value = literal ? literal->toPlainText().toUtf8() : QByteArray();
				const auto fields = liveFields();
				if (!fields.empty()) {
					auto message = QString();
					const auto encoded = Structures::EncodeRecord(id, fields, message);
					if (!encoded) {
						error->setText(message);
						return;
					}
					value = *encoded;
				}
				if (_config.named) {
					auto address = insert ? _config.address : item.address;
					if (insert) address.emplace_back(contents);
					if (op == Op::Rename || op == Op::Erase) {
						auto changes = Structures::RewriteReferences(_candidate, address, contents);
						if (!changes.uses.empty() && op == Op::Erase) {
							const auto replacement = std::find_if(_items.begin(), _items.end(), [&](const auto &other) {
								return other.address != address && other.label == contents;
							});
							const auto preset = objectId == SettingId::PresetObject;
							if (replacement == _items.end() && (!preset || contents != u"normal"_q)) {
								error->setText(u"Choose an existing replacement for the referenced record."_q);
								return;
							}
						}
						edits = std::move(changes.edits);
					}
					edits.push_back({ std::move(address), SettingsObjectEdit{ .operation = op,
						.name = contents, .value = value } });
				} else {
					edits.push_back(Structures::ArrayEdit(_config.address, op, insert ? _items.size() : size_t(at),
						op == Op::Duplicate ? size_t(at + 1) : size_t(0), value,
						contents.isEmpty() ? std::nullopt : std::make_optional(contents)));
				}
				if (run(std::move(edits), insert ? int(_items.size()) : at, error)) {
					mainPage();
				}
			});
		_parts->setCurrentWidget(_form);
		(static_cast<QWidget*>(name) ? static_cast<QWidget*>(name)
			: literal ? static_cast<QWidget*>(literal) : static_cast<QWidget*>(cancel))->setFocus();
	}

	bool run(std::vector<SettingsEdit> operations, int selection, QLabel *error = nullptr) {
		const auto revision = MapSettingsSource(_candidate).revision;
		const auto result = ApplySettingsEdits(_candidate, revision, operations);
		if (!result.ok()) {
			(error ? error : _status)->setText(result.error);
			render(selection);
			return false;
		}
		if (result.status == SettingsEditStatus::Changed) {
			++_generation;
			_history.push_back({ _candidate, _operations.size(), _members });
			_candidate = result.bytes;
			for (auto &operation : operations) _operations.push_back(std::move(operation));
			render(selection);
		}
		return true;
	}

	void commit(SettingsEditorMembers members, int selected = 0) {
		++_generation;
		_history.push_back({ _candidate, _operations.size(), _members });
		_members = std::move(members);
		render(selected);
	}

	void undo() {
		if (_history.empty()) return;
		++_generation;
		auto frame = std::move(_history.back());
		_history.pop_back();
		_candidate = std::move(frame.bytes);
		_operations.resize(frame.edits);
		_members = std::move(frame.members);
		render(_list->currentRow());
	}

	void stage() {
		if (!_config.current() || _history.empty()) return;
		const auto handler = _config.stage;
		auto intent = _members ? SettingsEditorIntent(*_members) : SettingsEditorIntent(_operations);
		if (handler) handler(std::move(intent));
	}

	struct Frame {
		QByteArray bytes;
		size_t edits = 0;
		std::optional<SettingsEditorMembers> members;
	};

	SettingsCollectionConfig _config;
	QByteArray _candidate;
	std::optional<SettingsEditorMembers> _members;
	QStackedWidget *_parts;
	QWidget *_main;
	QBoxLayout *_lists;
	QWidget *_form;
	QVBoxLayout *_formLayout;
	QListWidget *_list;
	QLineEdit *_search;
	QListWidget *_peerList;
	QLabel *_effects;
	QLabel *_status;
	QPointer<QPushButton> _accept;
	QPointer<QWidget> _exitPrompt;
	QPointer<QWidget> _formField;
	QPointer<QWidget> _mainFocus;
	SettingsFocus _exitFocus;
	Fn<QStringList()> _formInput;
	QStringList _formBaseline;
	QSet<QObject*> _preedit;
	std::vector<Structures::Item> _items;
	std::vector<SettingsEdit> _operations;
	std::vector<Frame> _history;
	uint64_t _generation = 0;
};

} // namespace

QWidget *CreateSettingsCollection(QWidget *parent, SettingsCollectionConfig config) {
	return new Collection(parent, std::move(config));
}

void BackFromSettingsCollection(QWidget *collection) {
	if (const auto editor = dynamic_cast<Collection*>(collection)) editor->back();
}

void RefreshSettingsCollectionPeers(QWidget *collection, bool sourceCleared) {
	if (const auto editor = dynamic_cast<Collection*>(collection)) editor->refreshPeers(sourceCleared);
}

bool SettingsCollectionHasPreedit(QWidget *collection) {
	const auto editor = dynamic_cast<Collection*>(collection);
	return editor && editor->hasPreedit();
}

} // namespace Purple
