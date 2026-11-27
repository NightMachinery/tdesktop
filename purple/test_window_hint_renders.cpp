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

using Role = Purple::HintRole;

Purple::HintPalette BadgePalette(bool dark, int percent = 80, bool symbols = true) {
	auto colors = Purple::HintPalette{ .symbols = symbols, .markWidth = 10, .stroke = 1, .radius = 4 };
	const auto accents = dark
		? std::array{ 0x708499, 0x6ab3f3, 0xb48bf2, 0x62d4e3, 0x62d4e3, 0xf3bc5c, 0xec3942, 0xec3942 }
		: std::array{ 0x999999, 0x168acd, 0x8544d6, 0x2996ad, 0x2996ad, 0xd09306, 0xd14e4e, 0xd14e4e };
	for (auto i = size_t(0); i != accents.size(); ++i) {
		colors.colors[i] = Purple::ResolveHintColors(
			dark ? QColor(0x17212b) : QColor(Qt::white),
			dark ? QColor(0xf5f5f5) : QColor(Qt::black),
			QColor(accents[i]), Role(i), percent);
	}
	return colors;
}

bool Readable(const Purple::HintColors &colors) {
	for (const auto backdrop : { QColor(Qt::white), QColor(Qt::black) }) {
		const auto fill = Purple::MixHintColor(backdrop, colors.fill, colors.fill.alphaF());
		const auto contrast = [&](QColor ink) {
			const auto a = Purple::HintLuminance(fill) + .05;
			const auto b = Purple::HintLuminance(ink) + .05;
			return std::max(a, b) / std::min(a, b);
		};
		if (colors.ink.alpha() != 255 || colors.prefix.alpha() != 255 || colors.edge.alpha() != 255
			|| contrast(colors.ink) < 4.5 || contrast(colors.prefix) < 4.5 || contrast(colors.edge) < 3.) {
			return false;
		}
	}
	return true;
}

bool CheckPalette() {
	for (auto opacity = 60; opacity <= 100; ++opacity) {
		for (const auto dark : { false, true }) {
			for (const auto &colors : BadgePalette(dark, opacity).colors) {
				if (!Readable(colors)) return false;
			}
		}
		if (!Readable(Purple::ResolveHintColors(QColor(139, 136, 133, 142),
			QColor(140, 137, 134, 100), QColor(147, 143, 135, 47), Role::Reply, opacity))) {
			return false;
		}
	}
	for (const auto dark : { false, true }) {
		const auto palette = BadgePalette(dark, 80);
		for (const auto background : { 0x192c39, 0xb1ce92, 0xfff3c8, 0x72484f }) {
			auto accents = std::vector<QColor>();
			for (const auto role : { Role::Navigate, Role::Reply, Role::Playback, Role::Menu }) {
				const auto &fill = palette.colors[int(role)].fill;
				const auto mixed = Purple::MixHintColor(QColor(background), fill, fill.alphaF());
				if (mixed.hsvSaturation() < 72) return false;
				for (const auto &other : accents) {
					const auto delta = std::abs(other.hsvHue() - mixed.hsvHue());
					if (std::min(delta, 360 - delta) < 30) return false;
				}
				accents.push_back(mixed);
			}
		}
	}
	auto silhouettes = std::vector<QImage>();
	auto palette = BadgePalette(false, 80, false);
	for (auto i = 0; i < int(Role::Count); ++i) {
		const auto role = Role(i);
		if (role == Role::PlayPause) continue;
		auto image = QImage(60, 24, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		palette.colors[i] = palette.colors[0];
		auto painter = QPainter(&image);
		Purple::PaintHintBadge(painter, image.rect().adjusted(1, 1, -1, -1), {}, 0, role, palette, false);
		painter.end();
		if (palette.inset(role) || std::find(silhouettes.begin(), silhouettes.end(), image) != silhouettes.end()) {
			return false;
		}
		silhouettes.push_back(image);
	}
	return true;
}

bool BadgeSheet(QDir dir, bool dark) {
	const auto titles = QStringList{ u"General"_s, u"Navigate"_s, u"Reply"_s, u"Playback"_s,
		u"Play/pause"_s, u"Menu"_s, u"Close"_s, u"Delete"_s };
	auto image = QImage(1020, 230, QImage::Format_ARGB32_Premultiplied);
	image.fill(dark ? QColor(0x17212b) : QColor(Qt::white));
	auto p = QPainter(&image);
	p.setFont(QFont(u"Menlo"_s, 12, QFont::Bold));
	for (auto i = 0; i != int(Role::Count); ++i) {
		const auto x = 138 + 110 * i;
		p.setPen(dark ? Qt::white : Qt::black);
		p.drawText(x, 23, titles[i]);
		for (auto j = 0; j < 6; ++j) {
			const auto alpha = std::array{ 60, 80, 100 }[j % 3];
			const auto colors = BadgePalette(dark, alpha, j < 3);
			const auto area = QRect(x, 32 + j * 32, 56, 23);
			Purple::PaintHintBadge(p, area, u"KF"_s, 1, Role(i), colors, false);
			if (!i) {
				p.setPen(dark ? Qt::white : Qt::black);
				p.drawText(7, area.center().y()+4,
					QString::number(alpha) + (j < 3 ? u"%  marks"_s : u"%  plain"_s));
			}
		}
	}
	p.end();
	return image.save(dir.filePath(dark ? u"palette-night.png"_s : u"palette-classic.png"_s));
}

bool AudioCluster(QDir dir, const Palette &pal, const QString &theme, bool rtl) {
	const auto view = QRect(0, 0, 354, 182);
	auto image = QImage(view.width() * 2, view.height() + 38, QImage::Format_ARGB32_Premultiplied);
	image.fill(pal.bg);
	auto p = QPainter(&image);
	const auto orient = [&](QRect rect) {
		if (rtl) rect.moveLeft(view.width() - rect.right() - 1);
		return rect;
	};
	const auto face = std::array{
		orient(QRect(72, 49, 34, 34)), orient(QRect(120, 49, 132, 23)),
		orient(QRect(115, 108, 104, 27)), orient(QRect(52, 30, 236, 121)),
	};
	const auto roles = std::array{ Role::PlayPause, Role::Navigate, Role::General, Role::Reply };
	const auto labels = QStringList{ u"KJ"_s, u"KF"_s, u"KL"_s, u"AJ"_s };
	const auto palette = BadgePalette(theme == u"dark"_s);
	const auto font = QFont(u"Menlo"_s, 12, QFont::Bold);
	const auto metrics = QFontMetrics(font);
	auto items = std::vector<Purple::HintGeometry>();
	for (auto i = size_t(0); i != face.size(); ++i) {
		const auto width = metrics.horizontalAdvance(labels[i]) + palette.inset(roles[i]);
		items.push_back({ face[i], QSize(width + 10, 21), false, i == 3, width + 2 });
	}
	const auto placements = Purple::LayoutHintBadges(items, view, 2, rtl);
	for (const auto &placement : placements) {
		if (placement.badge.isEmpty()) return false;
	}
	for (const auto filtered : { false, true }) {
		p.save();
		p.translate(filtered ? view.width() : 0, 0);
		Line(p, QRect(8, 0, 320, 30), filtered ? u"After K"_s : u"All hints"_s, pal.text, 15);
		p.translate(0, 32);
		p.fillRect(face[3], pal.surface);
		p.setPen(Qt::NoPen);
		p.setBrush(pal.selected);
		p.drawEllipse(face[0]);
		p.setPen(pal.text);
		p.drawText(face[0], Qt::AlignCenter, u"▶"_s);
		p.setFont(QFont(u"Helvetica Neue"_s, 12));
		p.drawText(face[1], Qt::AlignCenter, u"Room tone.m4a"_s);
		p.fillRect(face[2], pal.selected);
		p.drawText(face[2], Qt::AlignCenter, u"Transcribe"_s);
		p.setFont(font);
		for (auto i = size_t(0); i != items.size(); ++i) {
			if (filtered && !labels[i].startsWith(u'K')) continue;
			const auto &color = palette.colors[int(roles[i])].edge;
			p.setPen(QPen(color, 1));
			if (!placements[i].leader.isNull()) {
				p.drawLine(placements[i].leader);
				p.setBrush(color);
				p.drawEllipse(placements[i].leader.p2(), 1, 1);
			}
			if (filtered) {
				p.setBrush(Qt::NoBrush);
				p.drawRect(face[i]);
			}
		}
		for (auto i = size_t(0); i != items.size(); ++i) {
			if (!filtered || labels[i].startsWith(u'K')) {
				Purple::PaintHintBadge(p, placements[i].badge, labels[i], int(filtered), roles[i], palette, rtl);
			}
		}
		p.restore();
	}
	p.end();
	return image.save(dir.filePath(u"audio-"_s + theme + (rtl ? u"-rtl.png"_s : u"-ltr.png"_s)));
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
	auto regions = std::vector<Purple::HintGeometry>();
	auto roles = std::vector<Role>();
	const auto colors = BadgePalette(theme == u"dark"_s);
	p.setFont(QFont(u"Menlo"_s, 12, QFont::Bold));
	for (auto i = size_t(0); i < targets.size(); ++i) {
		const auto row = targets[i].x() >= 69 && targets[i].x() < 350 && scene != u"settings"_s;
		const auto role = !bubbles[i].isEmpty() ? Role::Reply : row ? Role::Navigate : Role::General;
		roles.push_back(role);
		const auto label = QString::fromStdString(labels[i]).toUpper();
		regions.push_back({ targets[i], QSize(p.fontMetrics().horizontalAdvance(label)
			+ 10 + colors.inset(role), 21), row, !bubbles[i].isEmpty() });
	}
	const auto placed = Purple::LayoutHintBadges(regions, image.rect(), 2, false);
	for (auto i = size_t(0); i != placed.size(); ++i) {
		if (placed[i].badge.isEmpty()) continue;
		p.setPen(colors.colors[int(roles[i])].edge);
		if (!placed[i].leader.isNull()) p.drawLine(placed[i].leader);
		Purple::PaintHintBadge(p, placed[i].badge, QString::fromStdString(labels[i]).toUpper(),
			0, roles[i], colors, false);
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
		Purple::PaintHintBadge(p, row.adjusted(19, 5, -row.width() + 77, -5),
			QString::fromStdString(keys[i]), 0, rows[i].known == 'd' ? Role::Destructive : Role::Menu,
			BadgePalette(theme == u"dark"_s), false);
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
	if (argc != 2 || !CheckPalette()) return 1;
	const auto dir = QDir(QString::fromLocal8Bit(argv[1]));
	if (!QDir().mkpath(dir.path())) return 2;
	for (const auto dark : { false, true }) {
		const auto pal = dark
			? Palette{ QColor(0x17212b), QColor(0x202b36), QColor(0x31455a), QColor(0x576b81), QColor(0xe5edf5), QColor(0xb2c1d0) }
			: Palette{ QColor(0xe4edf4), QColor(0xffffff), QColor(0xd7e9f8), QColor(0x9db4c7), QColor(0x183045), QColor(0x4c6174) };
		if (!BadgeSheet(dir, dark)) return 5;
		const auto theme = dark ? u"dark"_s : u"light"_s;
		for (const auto rtl : { false, true }) {
			if (!AudioCluster(dir, pal, theme, rtl)) return 6;
		}
		for (const auto &scene : { u"chat"_s, u"folders"_s, u"settings"_s }) {
			if (!Hints(dir, pal, theme, scene)) return 3;
		}
		for (const auto &scene : { u"own-message"_s, u"other-message"_s, u"chat"_s, u"folder"_s }) {
			if (!Hydra(dir, pal, theme, scene)) return 4;
		}
	}
	std::cout << "20 offscreen hint fixtures rendered\n";
}
