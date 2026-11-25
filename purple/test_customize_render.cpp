#include "purple/purple_settings_confirm_gesture.h"
#include "purple/purple_settings_customize_look.h"
#include "purple/purple_settings_focus.h"
#include "purple/purple_settings_draft.h"
#include "purple/purple_settings_review_display.h"

#include <QtCore/QDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

using namespace Purple;
using namespace Purple::CustomizeLook;
namespace Review = Purple::SettingsReviewDisplay;

int Checks = 0;

void Check(bool ok, const char *name) {
	++Checks;
	if (!ok) {
		std::cerr << name << '\n';
		std::exit(EXIT_FAILURE);
	}
}

void ConfirmGesture() {
	auto window = QWidget();
	auto underneath = Action(QStringLiteral("Edit"), &window);
	underneath.resize(120, 30);
	auto clicks = 0;
	QObject::connect(&underneath, &QPushButton::clicked, [&] { ++clicks; });
	const auto global = QPointF(100, 100);
	const auto release = QMouseEvent(QEvent::MouseButtonRelease, QPointF(10, 10), global,
		Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	for (const auto next : { QEvent::MouseButtonPress, QEvent::MouseButtonDblClick }) {
		SettingsConfirmGesture::Guard(&window, release);
		auto press = QMouseEvent(next, QPointF(10, 10), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		auto end = QMouseEvent(QEvent::MouseButtonRelease, QPointF(10, 10), global, Qt::LeftButton,
			Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(&underneath, &press);
		QApplication::sendEvent(&underneath, &end);
		Check(!underneath.isDown() && clicks == 0, "confirm gesture cannot click the replaced control");
	}
	SettingsConfirmGesture::Guard(&window, release);
	auto press = QMouseEvent(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(200, 100),
		Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	auto end = QMouseEvent(QEvent::MouseButtonRelease, QPointF(10, 10), QPointF(200, 100),
		Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(&underneath, &press);
	QApplication::sendEvent(&underneath, &end);
	Check(clicks == 1, "a distinct click still reaches the replacement page");
}

void ProtectedForm() {
	auto owner = Canvas();
	auto presentation = std::make_unique<QWidget>(&owner);
	auto editor = new QStackedWidget(presentation.get());
	auto form = new QWidget(editor);
	auto formLayout = new QVBoxLayout(form);
	auto text = new QPlainTextEdit(QStringLiteral("Draft line one\nDraft line two"), form);
	formLayout->addWidget(text);
	auto cover = new QLabel(QStringLiteral("Settings hidden while locked"), editor);
	editor->addWidget(form);
	editor->addWidget(cover);
	owner.resize(400, 250);
	presentation->resize(owner.size());
	editor->resize(owner.size());
	owner.show();
	presentation->show();
	editor->show();
	text->setFocus();
	auto cursor = text->textCursor();
	cursor.setPosition(7);
	text->setTextCursor(cursor);
	QApplication::processEvents();

	auto protection = SettingsProtectedForm();
	for (const auto detach : { false, true }) {
		protection.hide(editor, form, cover, true);
		Check(protection.held() && !text->isVisible() && !text->hasFocus(),
			"protected form is hidden and unfocusable");
		if (detach) {
			editor->hide();
			editor->setParent(&owner);
			presentation.reset();
			Check(!editor->isVisible(), "passcode teardown leaves the detached form hidden");
			presentation = std::make_unique<QWidget>(&owner);
			editor->setParent(presentation.get());
			presentation->resize(owner.size());
			editor->resize(owner.size());
			presentation->show();
			editor->show();
		}
		Check(!protection.restore(editor, form, SettingsDraft::CanRestoreForm(true, false))
			&& editor->currentWidget() == cover && !text->isVisible(), "stale opening keeps typed text covered");
		Check(!protection.restore(editor, form, SettingsDraft::CanRestoreForm(false, true))
			&& !text->isVisible(), "another session cannot reveal the form");
		Check(protection.restore(editor, form, SettingsDraft::CanRestoreForm(true, true))
			&& text->isVisible() && text->textCursor().position() == 7
			&& text->toPlainText() == QStringLiteral("Draft line one\nDraft line two"),
			"same opening resumes the exact unsaved text and caret");
	}
}

void Theme(QWidget *root, bool dark) {
	Apply(root,
		QColor(dark ? 0x17212b : 0xffffff),
		QColor(dark ? 0x202b36 : 0xf3f6f9),
		QColor(dark ? 0xf1f4f8 : 0x202020),
		QColor(dark ? 0x76899d : 0x727d88),
		QColor(dark ? 0x6ab5e9 : 0x168acd),
		QColor(dark ? 0x253240 : 0xe6eaf0));
}

void Frame(QWidget *root, QVBoxLayout *layout, const QString &heading) {
	const auto unit = root->fontMetrics().height();
	layout->setContentsMargins(unit, unit, unit, unit);
	layout->setSpacing(unit / 2);
	auto title = new QLabel(heading, root);
	title->setObjectName(QStringLiteral("fixtureHeading"));
	auto font = root->font();
	font.setBold(true);
	font.setPointSizeF(font.pointSizeF() * 1.25);
	title->setFont(font);
	layout->addWidget(title);
	auto chip = new Chip(root);
	chip->setDetails(QStringLiteral("Fixture account · App-wide"),
		QStringLiteral("/fixture/test/purple-telegram/config/settings/accounts/long-example/settings.toml"));
	layout->addWidget(chip);
	Check(chip->accessibleDescription().endsWith(QStringLiteral("settings.toml")), "full path remains accessible");
}

void Render(QWidget &widget, const QDir &output, const QString &name, QSize size) {
	widget.resize(size);
	widget.show();
	QApplication::processEvents();
	if (widget.size() != size) {
		std::cerr << widget.width() << 'x' << widget.height() << " expected "
			<< size.width() << 'x' << size.height() << '\n';
	}
	Check(widget.size() == size, "preview fits requested dimensions");
	const auto capture = widget.grab();
	const auto bitmap = capture.toImage();
	const auto dpr = capture.devicePixelRatio();
	const auto pixel = [&](QPoint point) {
		return bitmap.pixelColor(QPointF(point * dpr).toPoint());
	};
	const auto title = widget.findChild<QLabel*>(QStringLiteral("fixtureHeading"));
	Check(title && title->isVisible(), "heading is visible");
	for (const auto label : widget.findChildren<QLabel*>()) {
		if (!label->isVisible() || (label != title && !label->property("secondary").toBool())) continue;
		const auto bounds = QRect(label->mapTo(&widget, QPoint()), label->size());
		const auto background = pixel(QPoint(bounds.right() - 1, bounds.center().y()));
		Check(background == Tone(label, "customizeBackground", QPalette::Window),
			"pixels behind the label use the Customize canvas");
		auto contrast = 1.;
		for (auto y = bounds.top(); y < bounds.bottom(); ++y) {
			for (auto x = bounds.left(); x < bounds.right(); ++x) {
				contrast = std::max(contrast, ModalUi::TextContrast(pixel(QPoint(x, y)), background));
			}
		}
		Check(contrast >= 4.5, "painted label pixels meet contrast");
	}
	const auto buttons = widget.findChildren<QPushButton*>();
	auto occupied = QList<QRect>();
	for (const auto button : buttons) {
		if (!button->isVisible()) continue;
		const auto bounds = QRect(button->mapTo(&widget, QPoint()), button->size());
		Check(button->parentWidget()->rect().contains(button->geometry()), "action fits its existing row");
		Check(std::none_of(occupied.begin(), occupied.end(), [&](const QRect &other) {
			return other.intersects(bounds);
		}), "action buttons do not overlap");
		occupied.push_back(bounds);
	}
	Check(capture.save(output.filePath(name)), "PNG exported");
	widget.hide();
}

void TreeRender(const QDir &output, bool dark, int width, bool large = false) {
	auto root = Canvas();
	if (large) {
		auto font = root.font();
		font.setPointSize(19);
		root.setFont(font);
	}
	auto layout = new QVBoxLayout(&root);
	Frame(&root, layout, QStringLiteral("Customize Purple settings"));
	auto filter = new QLineEdit(&root);
	filter->setPlaceholderText(QStringLiteral("Filter settings…"));
	layout->addWidget(filter);
	auto tree = new Tree(&root);
	tree->setHeaderHidden(true);
	auto scope = new QTreeWidgetItem(tree, { QStringLiteral("File & account") });
	new QTreeWidgetItem(scope, { QStringLiteral("Send after saving\nOff · Own file") });
	new QTreeWidgetItem(scope, { QStringLiteral("Additional account settings") });
	scope->setExpanded(true);
	new QTreeWidgetItem(tree, { QStringLiteral("Work Mode") });
	new QTreeWidgetItem(tree, { QStringLiteral("Schedule & Focus") });
	new QTreeWidgetItem(tree, { QStringLiteral("Peek") });
	tree->setCurrentItem(scope->child(0));
	const auto tail = new QTreeWidgetItem(tree, { QStringLiteral("Last setting\nSecond line remains visible") });
	layout->addWidget(tree, 1);
	auto help = new QLabel(QStringLiteral("Effective Off · Default Off\nReview changes before writing the chosen settings file."), &root);
	help->setProperty("secondary", true);
	help->setWordWrap(true);
	layout->addWidget(help);
	auto actions = new Actions(&root);
	for (const auto &[label, key] : {
		std::pair{ "Edit", "RET" }, { "Reset", "r" }, { "Undo", "u" }, { "Review", "d" }, { "Help", "?" } }) {
		auto button = new Action(QString::fromLatin1(label), &root, QString::fromLatin1(key));
		if (key == QByteArray("u")) button->setEnabled(false);
		actions->append(button);
	}
	layout->addWidget(actions);
	Theme(&root, dark);
	tree->setFocus();
	Render(root, output, QStringLiteral("tree-%1-%2%3.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
		.arg(width).arg(large ? QStringLiteral("-large") : QString()),
		QSize(width, large ? 590 : 500));
	root.show();
	tree->scrollToBottom();
	QApplication::processEvents();
	Check(tree->viewport()->rect().contains(tree->visualItemRect(tail)), "last multiline item fits at scroll end");
	root.hide();
}

void FormRender(const QDir &output, bool dark, int width) {
	auto root = Canvas();
	auto font = root.font();
	font.setPointSize(13);
	root.setFont(font);
	auto layout = new QVBoxLayout(&root);
	Frame(&root, layout, QStringLiteral("Media reply"));
	auto help = new QLabel(QStringLiteral("Reply text for the fixture account."), &root);
	help->setProperty("secondary", true);
	help->setWordWrap(true);
	layout->addWidget(help);
	auto line = new QLineEdit(QStringLiteral("A synthetic reply for this offscreen test"), &root);
	layout->addWidget(line);
	auto edit = new QPlainTextEdit(QStringLiteral("A multi-line example\nSecond line"), &root);
	edit->setMaximumHeight(root.fontMetrics().height() * 5);
	layout->addWidget(edit);
	auto keep = new Action(QStringLiteral("Keep editing"), &root, QStringLiteral("RET"));
	AppendAction(layout, keep);
	AppendAction(layout, new Action(QStringLiteral("Use built-in"), &root));
	AppendAction(layout, new Action(QStringLiteral("Discard input"), &root));
	layout->addStretch();
	Theme(&root, dark);
	root.resize(width, 480);
	root.show();
	QApplication::processEvents();
	line->setFocus();
	line->setSelection(12, -4);
	auto lineFocus = SettingsFocus(line);
	keep->setFocus();
	line->setCursorPosition(0);
	Check(lineFocus.restore(&root) && line->hasFocus() && line->cursorPosition() == 8
		&& line->selectionStart() == 8 && line->selectionEnd() == 12, "line focus, cursor and reverse selection restored");
	auto cursor = edit->textCursor();
	cursor.setPosition(6);
	cursor.setPosition(2, QTextCursor::KeepAnchor);
	edit->setTextCursor(cursor);
	auto multiFocus = SettingsFocus(edit);
	keep->setFocus();
	Check(multiFocus.restore(&root) && edit->hasFocus() && edit->textCursor().position() == 2
		&& edit->textCursor().anchor() == 6, "multiline focus and selection restored");
	auto dead = new QLineEdit(&root);
	auto gone = SettingsFocus(dead);
	delete dead;
	Check(!gone.restore(&root), "destroyed field is never refocused");
	Check(lineFocus.restore(&root), "line restored after multiline test");
	Render(root, output, QStringLiteral("form-%1-%2.png")
		.arg(dark ? QStringLiteral("dark") : QStringLiteral("light")).arg(width), QSize(width, 480));
}

ConfigChangeSummary ChangeFixture() {
	const auto str = [](const char *value) { return QString::fromLatin1(value); };
	return { .entries = { { .keys = {
		{ .table = str("lists.fixture"), .key = str("title"), .beforeValue = str("'Fixture'") },
		{ .table = str("lists.renamed"), .key = str("title"), .afterValue = str("'Fixture'") },
		{ .table = str("lists.empty"), .afterValue = QString() },
		{ .table = str("presets.work"), .key = str("list_order"),
			.beforeValue = str("[{ list = 'fixture' }]"),
			.afterValue = str("[{ list = 'renamed', show_mode = 'message' }, { list = 'fixture_other', show_mode = 'notification' }]") },
	} } }, .parsed = true };
}

void ReviewRenameLosses() {
	const auto draft = ConfigChangeSummary{ .entries = { { .keys = {
		{ .table = QStringLiteral("lists.fixture"), .key = QStringLiteral("title"),
			.beforeValue = QStringLiteral("'Removed title'") },
		{ .table = QStringLiteral("lists.fixture"), .key = QStringLiteral("members"),
			.beforeValue = QStringLiteral("[1, 2]") },
		{ .table = QStringLiteral("lists.renamed"), .key = QStringLiteral("members"),
			.afterValue = QStringLiteral("[1]") },
	} } }, .parsed = true };
	const auto renames = std::vector{ Review::Rename{ QStringLiteral("lists.fixture"),
		QStringLiteral("lists.renamed"), QStringLiteral("fixture"), QStringLiteral("renamed"), {} } };
	const auto page = Review::Effects(Review::Flatten(draft), renames, QColor(0xff8d92), QColor(0x81d6a3));
	Check(page.contains(QStringLiteral("<b>lists.renamed.title</b><br/>"
		"<span style=\"color:#ff8d92\">− 'Removed title'</span><br/>"
		"<span style=\"color:#81d6a3\">+ absent</span>")),
		"rename plus field deletion shows the old value and absence at the new path");
	Check(page.contains(QStringLiteral("<b>lists.renamed.members</b><br/>"
		"<span style=\"color:#ff8d92\">− [1, 2]</span><br/>"
		"<span style=\"color:#81d6a3\">+ [1]</span>")),
		"rename plus member removal shows both member lists");

	using Impact = ConfigImpactKind;
	auto loss = ConfigImpactFinding{ .kind = Impact::ListLoss, .change = {
		.kind = ConfigChangeKind::Removed, .table = renames.front().from,
		.addressKind = ConfigChangeAddressKind::ListDefinition,
	} };
	auto smaller = draft;
	smaller.entries.front().keys.erase(smaller.entries.front().keys.begin());
	Check(!Review::IsRenameOnlyLoss(loss, Review::Flatten(smaller), renames),
		"member loss alone keeps the list warning");
	smaller.entries.front().keys.back().afterValue = QStringLiteral("[1, 2]");
	Check(Review::IsRenameOnlyLoss(loss, Review::Flatten(smaller), renames),
		"a pure rename suppresses the list loss caused by the old name disappearing");
	loss.kind = Impact::StrandedMembers;
	Check(!Review::IsRenameOnlyLoss(loss, Review::Flatten(smaller), renames),
		"a pure rename keeps stranded member warnings");
	loss.kind = Impact::ListLoss;
	loss.account = QStringLiteral("fixture account");
	Check(!Review::IsRenameOnlyLoss(loss, Review::Flatten(smaller), renames),
		"a pure rename keeps account-specific warnings");
	loss.account.clear();
	smaller.entries.front().keys.push_back(draft.entries.front().keys.front());
	Check(!Review::IsRenameOnlyLoss(loss, Review::Flatten(smaller), renames),
		"field loss alone keeps the list warning");

	const auto presetRenames = std::vector{ Review::Rename{ QStringLiteral("presets.fixture"),
		QStringLiteral("presets.renamed"), QStringLiteral("fixture"), QStringLiteral("renamed"), {} } };
	const auto status = QStringLiteral("'active'");
	auto preset = ConfigChangeSummary{ .entries = { { .keys = {
		{ .table = presetRenames.front().from, .key = QStringLiteral("title"), .beforeValue = status },
		{ .table = presetRenames.front().to, .key = QStringLiteral("title"), .afterValue = status },
		{ .table = presetRenames.front().from, .key = QStringLiteral("list_order"),
			.beforeValue = QStringLiteral("[{ list = 'fixture' }]") },
	} } }, .parsed = true };
	loss.kind = Impact::PresetLoss;
	loss.change.table = presetRenames.front().from;
	loss.change.addressKind = ConfigChangeAddressKind::Unit;
	Check(!Review::IsRenameOnlyLoss(loss, Review::Flatten(preset), presetRenames),
		"field loss keeps the preset warning");
	loss.kind = Impact::AppWideLoss;
	Check(!Review::IsRenameOnlyLoss(loss, Review::Flatten(preset), presetRenames),
		"field loss keeps the app-wide warning");
	preset.entries.front().keys.pop_back();
	loss.kind = Impact::PresetLoss;
	Check(Review::IsRenameOnlyLoss(loss, Review::Flatten(preset), presetRenames),
		"a pure rename suppresses the preset loss caused by the old name disappearing");
}

void ReviewCollidingEntries() {
	const auto str = [](const char *value) { return QString::fromLatin1(value); };
	const auto entry = [&](const QString &path, const char *label, const char *value) {
		return ConfigChangeEntry{ .label = str(label),
			.keys = { { .table = path, .afterValue = str(value) } } };
	};
	const auto path = str("fixture");
	const auto draft = ConfigChangeSummary{ .entries = {
		entry(path, "", "first-value"),
		entry(path, "", "second-value"),
		entry(path, "same", "third-value"),
		entry(path, "same", "fourth-value"),
		entry(path, "same", "fifth-value"),
		entry(QStringLiteral("fixture · same"), "", "literal-path-value"),
	}, .parsed = true };
	auto disk = ConfigChangeSummary{ .entries = {
		entry(path, "same", "disk-first"),
		entry(path, "same", "disk-second"),
		entry(path, "same", "disk-third"),
		entry(path, "same", "disk-fourth"),
	}, .parsed = true };
	disk.entries.front().keys.front().beforeValue = str("disk-original");
	const auto page = Review::Effects(Review::Flatten(draft), {}, QColor(0xff8d92), QColor(0x81d6a3));
	const auto conflict = Review::Conflict(draft, disk);
	for (const auto *value : { "first-value", "second-value", "third-value", "fourth-value",
			"fifth-value", "literal-path-value" }) {
		Check(page.contains(str(value)), "Effects keeps each colliding entry");
		Check(conflict.contains(str(value)), "Conflict keeps each colliding entry");
	}
	for (const auto *value : { "disk-first", "disk-second", "disk-third", "disk-fourth" }) {
		Check(conflict.contains(str(value)), "Conflict keeps each colliding disk entry");
	}
	Check(conflict.contains(str("disk-original")), "Conflict keeps a distinct opening value at the same address");
}

void ReviewRender(const QDir &output, bool conflict, bool dark, int width) {
	auto root = Canvas();
	auto layout = new QVBoxLayout(&root);
	Frame(&root, layout, conflict ? QStringLiteral("Settings changed since this draft")
		: QStringLiteral("Review settings changes"));
	auto pane = new QTextEdit(&root);
	pane->setReadOnly(true);
	pane->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	pane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	layout->addWidget(pane, 1);
	Theme(&root, dark);
	const auto draft = ChangeFixture();
	if (conflict) {
		auto disk = ConfigChangeSummary{ .entries = { { .keys = { {
			.table = QStringLiteral("presets.work"), .key = QStringLiteral("list_order"),
			.beforeValue = QStringLiteral("[{ list = 'fixture' }]"),
			.afterValue = QStringLiteral("[]"),
		} } } }, .parsed = true };
		const auto page = Review::Conflict(draft, disk);
		Check(page.contains(QStringLiteral("<b>Yours</b>")) && page.contains(QStringLiteral("<b>On disk</b> · []"))
			&& page.contains(QStringLiteral("<b>Original</b>")), "conflict has all three states");
		pane->setHtml(page);
		for (const auto text : { "Keep draft for comparison", "Reload and review", "Discard draft", "Show raw files" }) {
			AppendAction(layout, new Action(QString::fromLatin1(text), &root));
		}
	} else {
		const auto background = Tone(pane, "customizeSurface", QPalette::Base);
		const auto ink = Review::InkFor(background);
		Check(ModalUi::TextContrast(ink.removed, background) >= 4.5
			&& ModalUi::TextContrast(ink.added, background) >= 4.5, "review ink meets contrast in each palette");
		const auto page = Review::Effects(Review::Flatten(draft), { { QStringLiteral("lists.fixture"), QStringLiteral("lists.renamed"),
			QStringLiteral("fixture"), QStringLiteral("renamed"), { QStringLiteral("presets.work.list_order") } } },
			ink.removed, ink.added);
		Check(!page.contains(QStringLiteral("lists.fixture.title")) && !page.contains(QStringLiteral("lists.renamed.title")),
			"one rename block replaces paired descendants");
		Check(page.contains(QStringLiteral("Renamed fixture to renamed")) && page.contains(QStringLiteral("Added, empty"))
			&& page.contains(QStringLiteral("References updated")) && page.contains(QStringLiteral("presets.work.list_order")), "rename, empty add, and reference effects shown");
		Check(Review::Escaped(QStringLiteral("<b>literal</b>")).contains(QStringLiteral("&lt;b&gt;")), "plain TOML is escaped");
		pane->setHtml(page);
		for (const auto text : { "Back", "Effects", "Lines", "Previous page", "Next page", "Apply changes" }) {
			AppendAction(layout, new Action(QString::fromLatin1(text), &root));
		}
	}
	Render(root, output, QStringLiteral("%1-%2-%3.png").arg(conflict ? QStringLiteral("conflict") : QStringLiteral("review"),
		dark ? QStringLiteral("dark") : QStringLiteral("light")).arg(width), QSize(width, 510));
}

void SheetRender(const QDir &output, bool dark, int width, const QString &kind,
		const QString &heading, std::initializer_list<const char*> actions) {
	auto host = QWidget();
	host.setAutoFillBackground(true);
	auto palette = host.palette();
	palette.setColor(QPalette::Window, QColor(0xeaeaea));
	host.setPalette(palette);
	auto outer = new QVBoxLayout(&host);
	outer->setContentsMargins({});
	auto stack = new QStackedWidget(&host);
	auto scroll = new QScrollArea(stack);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setViewport(new Canvas(scroll));
	auto page = new Canvas(scroll);
	scroll->setWidget(page);
	stack->addWidget(scroll);
	outer->addWidget(stack);
	auto layout = new QVBoxLayout(page);
	Frame(page, layout, heading);
	if (kind == QStringLiteral("collection")) {
		auto search = new QLineEdit(page);
		search->setPlaceholderText(QStringLiteral("Filter available chats"));
		layout->addWidget(search);
		auto lists = new QHBoxLayout();
		for (const auto name : { "Selected chat", "Available chat" }) {
			auto list = new QListWidget(page);
			list->addItem(QString::fromLatin1(name));
			lists->addWidget(list);
		}
		layout->addLayout(lists, 1);
	} else if (kind == QStringLiteral("budget")) {
		auto search = new QLineEdit(page);
		search->setPlaceholderText(QStringLiteral("Filter eligible chats"));
		layout->addWidget(search);
		auto list = new QListWidget(page);
		list->addItem(QStringLiteral("Fixture chat"));
		layout->addWidget(list, 1);
	} else if (kind == QStringLiteral("rename")) {
		auto refs = new QLabel(QStringLiteral("Same-file references\npresets.example.list_order[0].list"), page);
		refs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		layout->addWidget(refs);
		layout->addWidget(new QLineEdit(QStringLiteral("fixture-renamed"), page));
	} else if (kind == QStringLiteral("confirm")) {
		auto body = new QTextEdit(page);
		body->setPlainText(QStringLiteral("Renamed fixture to fixture-renamed\nReferences updated in the destination file."));
		body->setReadOnly(true);
		layout->addWidget(body, 1);
	} else {
		const auto description = (kind == QStringLiteral("help"))
			? QStringLiteral("Return edits a setting. F8 opens the privacy screen outside a text field.")
			: (kind == QStringLiteral("waiting"))
			? QStringLiteral("Waiting for the reviewed settings operation.")
			: QStringLiteral("Fixture settings and actions");
		auto body = new QLabel(description, page);
		body->setProperty("secondary", true);
		layout->addWidget(body);
		layout->addStretch();
	}
	auto firstAction = static_cast<QPushButton*>(nullptr);
	for (const auto action : actions) {
		const auto button = new Action(QString::fromLatin1(action), page);
		AppendAction(layout, button);
		if (!firstAction) firstAction = button;
	}
	if (kind == QStringLiteral("rename")) layout->addStretch();
	Theme(&host, !dark);
	Theme(stack, !dark);
	host.resize(width, 460);
	host.show();
	QApplication::processEvents();
	Theme(stack, dark);
	if (firstAction) firstAction->setFocus();
	Render(host, output, QStringLiteral("%1-%2-%3.png").arg(kind,
		dark ? QStringLiteral("dark") : QStringLiteral("light")).arg(width), QSize(width, 460));
}

} // namespace

int main(int argc, char **argv) {
	auto app = QApplication(argc, argv);
	app.setStyle(QStringLiteral("Fusion"));
	app.setFont(QFont(QStringLiteral("Helvetica Neue"), 12));
	Check(argc == 2, "pass the render directory");
	const auto output = QDir(QString::fromLocal8Bit(argv[1]));
	Check(output.mkpath(QStringLiteral(".")), "create render directory");
	ConfirmGesture();
	ProtectedForm();
	ReviewRenameLosses();
	ReviewCollidingEntries();
	for (const auto dark : { false, true }) {
		for (const auto width : { 640, 800, 1200 }) {
			TreeRender(output, dark, width);
			FormRender(output, dark, width);
			ReviewRender(output, false, dark, width);
			ReviewRender(output, true, dark, width);
			SheetRender(output, dark, width, QStringLiteral("collection"), QStringLiteral("Members"),
				{ "Add", "Enter ID", "Edit", "Delete", "Move up", "Move down", "Undo",
					"Reset members", "Stage collection", "Back" });
			SheetRender(output, dark, width, QStringLiteral("rename"), QStringLiteral("Rename collection"),
				{ "Cancel", "Keep change" });
			SheetRender(output, dark, width, QStringLiteral("confirm"), QStringLiteral("Confirm settings change"),
				{ "Cancel", "Confirm and apply" });
			SheetRender(output, dark, width, QStringLiteral("recovery"), QStringLiteral("Settings need repair"),
				{ "Retry after repair", "Copy location", "Browse effective values", "Close" });
			SheetRender(output, dark, width, QStringLiteral("account"), QStringLiteral("Account to inspect"),
				{ "Switch buffer", "Back" });
			SheetRender(output, dark, width, QStringLiteral("discard"), QStringLiteral("Discard staged changes?"),
				{ "Keep editing", "Review and apply", "Discard and close" });
			SheetRender(output, dark, width, QStringLiteral("budget"), QStringLiteral("Choose budget chat"),
				{ "Choose chat", "Cancel" });
			SheetRender(output, dark, width, QStringLiteral("help"), QStringLiteral("Customize help"),
				{ "Back" });
			SheetRender(output, dark, width, QStringLiteral("waiting"), QStringLiteral("Applying reviewed settings"),
				{});
		}
	}
	TreeRender(output, true, 640, true);
	std::cout << Checks << " checks, 0 failures\n";
}
