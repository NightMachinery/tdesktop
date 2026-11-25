#include "purple/purple_settings_customize_look.h"
#include "purple/purple_settings_focus.h"
#include "purple/purple_settings_review_display.h"

#include <QtCore/QDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <cstdlib>
#include <iostream>

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
	Check(widget.grab().save(output.filePath(name)), "PNG exported");
	widget.hide();
}

void TreeRender(const QDir &output, bool dark, int width, bool large = false) {
	auto root = QWidget();
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
	Render(root, output, large ? QStringLiteral("tree-dark-large-640.png")
		: dark ? QStringLiteral("tree-dark-1000.png") : QStringLiteral("tree-light-640.png"),
		QSize(width, large ? 590 : 500));
}

void FormRender(const QDir &output) {
	auto root = QWidget();
	auto font = root.font();
	font.setPointSize(19);
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
	layout->addWidget(keep, 0, Qt::AlignLeft);
	layout->addStretch();
	Theme(&root, true);
	root.resize(640, 480);
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
	Render(root, output, QStringLiteral("form-dark-large-640.png"), QSize(640, 480));
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

void ReviewRender(const QDir &output, bool conflict) {
	auto root = QWidget();
	auto layout = new QVBoxLayout(&root);
	Frame(&root, layout, conflict ? QStringLiteral("Settings changed since this draft")
		: QStringLiteral("Review settings changes"));
	auto pane = new QTextEdit(&root);
	pane->setReadOnly(true);
	pane->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	pane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	layout->addWidget(pane, 1);
	Theme(&root, !conflict);
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
		layout->addWidget(new Action(QStringLiteral("Show raw files"), &root), 0, Qt::AlignLeft);
	} else {
		const auto page = Review::Effects(draft, { { QStringLiteral("lists.fixture"), QStringLiteral("lists.renamed"),
			QStringLiteral("fixture"), QStringLiteral("renamed"), { QStringLiteral("presets.work.list_order") } } }, QColor(0xff8d92), QColor(0x81d6a3));
		Check(!page.contains(QStringLiteral("lists.fixture.title")) && !page.contains(QStringLiteral("lists.renamed.title")),
			"one rename block replaces paired descendants");
		Check(page.contains(QStringLiteral("Renamed fixture to renamed")) && page.contains(QStringLiteral("Added, empty"))
			&& page.contains(QStringLiteral("References updated")) && page.contains(QStringLiteral("presets.work.list_order")), "rename, empty add, and reference effects shown");
		Check(Review::Escaped(QStringLiteral("<b>literal</b>")).contains(QStringLiteral("&lt;b&gt;")), "plain TOML is escaped");
		pane->setHtml(page);
		layout->addWidget(new Action(QStringLiteral("Apply changes"), &root), 0, Qt::AlignLeft);
	}
	Render(root, output, conflict ? QStringLiteral("conflict-light-1200.png")
		: QStringLiteral("effects-dark-640.png"), QSize(conflict ? 1200 : 640, 510));
}

} // namespace

int main(int argc, char **argv) {
	auto app = QApplication(argc, argv);
	app.setStyle(QStringLiteral("Fusion"));
	app.setFont(QFont(QStringLiteral("Helvetica Neue"), 12));
	Check(argc == 2, "pass the render directory");
	const auto output = QDir(QString::fromLocal8Bit(argv[1]));
	Check(output.mkpath(QStringLiteral(".")), "create render directory");
	TreeRender(output, false, 640);
	TreeRender(output, true, 1000);
	TreeRender(output, true, 640, true);
	FormRender(output);
	ReviewRender(output, false);
	ReviewRender(output, true);
	std::cout << Checks << " checks, 0 failures\n";
}
