#include "purple/purple_hint_labels.h"
#include "purple/purple_hint_mnemonics.h"
#include "purple/purple_hint_paint.h"
#include "purple/purple_message_hint_placement.h"

#include <QtCore/QDir>
#include <QtGui/QGuiApplication>

#include <iostream>

namespace {

using namespace Qt::StringLiterals;

struct Palette {
	QColor bg;
	QColor surface;
	QColor selected;
	QColor border;
	QColor text;
	QColor muted;
};

void Line(QPainter &p, QRect rect, const QString &title, QColor color, int size = 15) {
	p.setFont(QFont(u"Helvetica Neue"_s, size));
	p.setPen(color);
	p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, title);
}

bool Hints(QDir dir, const Palette &pal, const QString &theme, const QString &scene) {
	auto image = QImage(820, 520, QImage::Format_ARGB32_Premultiplied);
	image.fill(pal.bg);
	auto p = QPainter(&image);
	auto targets = std::vector<QRect>();
	auto bubbles = std::vector<QRect>();
	const auto target = [&](QRect rect, bool bubble = false) {
		targets.push_back(rect);
		bubbles.push_back(bubble ? rect : QRect());
	};
	const auto button = [&](QRect rect, const QString &text) {
		p.fillRect(rect, pal.selected);
		Line(p, rect.adjusted(8, 0, -8, 0), text, pal.text, 13);
		target(rect);
	};
	if (scene == u"chat"_s || scene == u"folders"_s) {
		p.fillRect(QRect(0, 0, 68, 520), pal.surface);
		for (auto i = 0; i < 5; ++i) {
			button(QRect(6, 12 + i * 53, 56, 38),
				QStringList{ u"All"_s, u"Work"_s, u"News"_s, u"Local"_s, u"Lists"_s }[i]);
		}
		p.fillRect(QRect(69, 0, 280, 520), pal.surface);
		Line(p, QRect(84, 0, 250, 48), u"Chats"_s, pal.text, 18);
		for (auto i = 0; i < 8; ++i) {
			const auto row = QRect(78, 48 + i * 50, 260, 46);
			if (i == 0) p.fillRect(row, pal.selected);
			p.setPen(Qt::NoPen);
			p.setBrush(pal.border);
			p.drawEllipse(QRect(row.x() + 8, row.y() + 7, 32, 32));
			Line(p, row.adjusted(50, 0, -5, 0), QStringList{ u"Project notes"_s, u"Design review"_s,
				u"Archive"_s, u"Music"_s, u"Weekend plans"_s, u"Reading group"_s, u"Recipes"_s, u"Saved Messages"_s }[i], pal.text, 13);
			target(QRect(row.x() + 3, row.center().y() - 10, 32, 20));
		}
		if (scene == u"chat"_s) {
			p.fillRect(QRect(350, 0, 470, 52), pal.surface);
			Line(p, QRect(366, 0, 330, 52), u"Design review"_s, pal.text, 18);
			button(QRect(738, 8, 68, 36), u"Search"_s);
			for (auto i = 0; i < 7; ++i) {
				const auto x = i % 2 ? 510 : 392;
				const auto bubble = QRect(x, 63 + i * 57, i % 2 ? 270 : 299, 48);
				p.fillRect(bubble, i % 2 ? pal.selected : pal.surface);
				Line(p, bubble.adjusted(10, 0, -10, 0), i % 2 ? u"I’ll check the latest preview."_s
					: u"Preview, notes, and attachments"_s, pal.text, 13);
				target(bubble, true);
				if (!(i % 2)) target(QRect(x + 12, bubble.bottom() - 14, 48, 13));
			}
			p.fillRect(QRect(350, 473, 470, 47), pal.surface);
			button(QRect(362, 479, 57, 34), u"Attach"_s);
			Line(p, QRect(438, 474, 250, 40), u"Write a message…"_s, pal.muted, 13);
			button(QRect(745, 479, 64, 34), u"Send"_s);
		}
	} else {
		p.fillRect(QRect(192, 60, 436, 400), pal.surface);
		Line(p, QRect(219, 76, 360, 45), u"Privacy and security"_s, pal.text, 19);
		button(QRect(562, 73, 46, 38), u"✕"_s);
		for (auto i = 0; i < 4; ++i) {
			button(QRect(217, 134 + i * 57, 380, 40), QStringList{ u"Passcode lock"_s,
				u"Active sessions"_s, u"Privacy screen"_s, u"About this device"_s }[i]);
		}
		button(QRect(496, 391, 100, 36), u"Done"_s);
	}
	const auto labels = Purple::HintLabels(targets.size());
	auto placed = std::vector<QRect>();
	p.setFont(QFont(u"Menlo"_s, 12, QFont::Bold));
	for (auto i = std::size_t(0); i < targets.size(); ++i) {
		const auto label = QString::fromStdString(labels[i]);
		const auto size = QSize(p.fontMetrics().horizontalAdvance(label.toUpper()) + 10, 21);
		const auto anchor = bubbles[i].isEmpty()
			? Purple::WindowHintAnchor(targets[i], size,
				scene != u"settings"_s && targets[i].x() >= 69 && targets[i].x() < 350, false)
			: Purple::MessageHintBadgeRect(bubbles[i], bubbles[i].top(), size, image.rect(), 2, false);
		const auto badge = Purple::PlaceHintBadge(anchor, image.rect(), placed);
		if (badge.isEmpty()) return false;
		placed.push_back(badge);
		Purple::PaintHintBadge(p, badge, label, 0);
	}
	p.end();
	return image.save(dir.filePath(u"hint-"_s + scene + u"-"_s + theme + u".png"_s));
}

bool Hydra(QDir dir, const Palette &pal, const QString &theme, const QString &scene) {
	auto rows = std::vector<Purple::HintMnemonic>();
	auto title = QString();
	if (scene == u"own-message"_s || scene == u"other-message"_s) {
		title = u"Message actions"_s;
		rows = { { 'r', "Reply" }, { 'y', "Copy Text" }, { 'f', "Forward" },
			{ 'p', "Pin" }, { 's', "Select" }, { 't', "Translate" }, { 'R', "React (native picker)" },
			{ 'm', "Full native menu" } };
		if (scene == u"own-message"_s) {
			rows.insert(rows.begin() + 1, { 'e', "Edit" });
			rows.insert(rows.begin() + 5, { 'd', "Delete" });
		}
	} else if (scene == u"chat"_s) {
		title = u"Design review"_s;
		rows = { { 'o', "Open" }, { 'r', "Mark as unread" }, { 'm', "Mute notifications  ›" },
			{ 'p', "Pin" }, { 'a', "Archive" }, { 'f', "Add to folder  ›" },
			{ 'd', "Delete chat" }, { 'w', "Open in new window" } };
	} else {
		title = u"Work"_s;
		rows = { { 'o', "Open" }, { 'e', "Edit folder" }, { 'r', "Mark all chats as read" }, { 'd', "Remove" } };
	}
	const auto keys = Purple::HintMnemonics(rows);
	auto image = QImage(520, 114 + int(rows.size()) * 32, QImage::Format_ARGB32_Premultiplied);
	image.fill(pal.bg);
	auto p = QPainter(&image);
	const auto panel = image.rect().adjusted(24, 18, -24, -18);
	p.fillRect(panel, pal.surface);
	Line(p, QRect(panel.x() + 19, 30, panel.width() - 38, 29), title, pal.text, 19);
	Line(p, QRect(panel.x() + 19, 61, panel.width() - 38, 24), u"RET default  ·  j/k move  ·  ? help  ·  Esc close"_s, pal.muted, 13);
	for (auto i = std::size_t(0); i < rows.size(); ++i) {
		const auto row = QRect(panel.x(), 85 + int(i) * 32, panel.width(), 32);
		if (i == 0) p.fillRect(row, pal.selected);
		Purple::PaintHydraCap(p, row.adjusted(19, 5, -row.width() + 77, -5),
			QString::fromStdString(keys[i]), pal.bg, pal.border, pal.text, 4);
		p.setPen(QPen(pal.muted, 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QRect(row.x() + 91, row.y() + 11, 10, 10));
		Line(p, row.adjusted(116, 0, -19, 0), QString::fromStdString(rows[i].title), pal.text, 14);
	}
	p.end();
	return image.save(dir.filePath(u"hydra-"_s + scene + u"-"_s + theme + u".png"_s));
}

} // namespace

int main(int argc, char **argv) {
	auto app = QGuiApplication(argc, argv);
	if (argc != 2) return 1;
	const auto dir = QDir(QString::fromLocal8Bit(argv[1]));
	if (!QDir().mkpath(dir.path())) return 2;
	for (const auto dark : { false, true }) {
		const auto pal = dark
			? Palette{ QColor(0x17212b), QColor(0x202b36), QColor(0x31455a), QColor(0x576b81), QColor(0xe5edf5), QColor(0xb2c1d0) }
			: Palette{ QColor(0xe4edf4), QColor(0xffffff), QColor(0xd7e9f8), QColor(0x9db4c7), QColor(0x183045), QColor(0x4c6174) };
		const auto theme = dark ? u"dark"_s : u"light"_s;
		for (const auto &scene : { u"chat"_s, u"folders"_s, u"settings"_s }) {
			if (!Hints(dir, pal, theme, scene)) return 3;
		}
		for (const auto &scene : { u"own-message"_s, u"other-message"_s, u"chat"_s, u"folder"_s }) {
			if (!Hydra(dir, pal, theme, scene)) return 4;
		}
	}
	std::cout << "14 offscreen hint fixtures rendered\n";
}
