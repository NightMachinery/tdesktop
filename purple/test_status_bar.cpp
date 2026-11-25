#include "purple/purple_status_bar.h"

#include <QtCore/QDir>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <iostream>

namespace {

using namespace Purple::ModalUi;
using namespace Qt::StringLiterals;

int Checks = 0;
int Failures = 0;

void Check(bool ok, const char *label) {
	++Checks;
	if (!ok) {
		++Failures;
		std::cerr << "FAIL: " << label << '\n';
	}
}

struct Theme {
	StatusPalette palette;
	QColor wallpaper;
	QColor pattern;
	QColor bubble;
};

const auto Light = Theme{
	.palette = {
		.window = Qt::white, .rail = QColor(0x29, 0x3a, 0x4c),
		.dialogs = Qt::white, .chat = Qt::white, .info = Qt::white,
		.divider = QColor(0, 0, 0, 24),
		.text = Qt::black, .muted = QColor(0x99, 0x99, 0x99),
		.error = QColor(0xd1, 0x4e, 0x4e), .accent = QColor(0x16, 0x8a, 0xcd),
	},
	.wallpaper = QColor(0xc3, 0xd9, 0xa7),
	.pattern = QColor(0x8a, 0xb1, 0x82, 120),
	.bubble = Qt::white,
};

const auto Dark = Theme{
	.palette = {
		.window = QColor(0x17, 0x21, 0x2b), .rail = QColor(0x0e, 0x16, 0x21),
		.dialogs = QColor(0x17, 0x21, 0x2b), .chat = QColor(0x17, 0x21, 0x2b),
		.info = QColor(0x17, 0x21, 0x2b), .divider = QColor(4, 8, 14, 86),
		.text = QColor(0xf5, 0xf5, 0xf5),
		.muted = QColor(0x70, 0x84, 0x99), .error = QColor(0xec, 0x39, 0x42),
		.accent = QColor(0x6a, 0xb3, 0xf3),
	},
	.wallpaper = QColor(0x1c, 0x2d, 0x3d),
	.pattern = QColor(0x51, 0x6c, 0x81, 100),
	.bubble = QColor(0x18, 0x25, 0x33),
};

[[nodiscard]] StatusColumns Columns(
		int rail, int dialogs, int chat, int info,
		bool rtl = false, int scale = 100) {
	const auto total = rail + dialogs + chat + info;
	const auto span = [&](int x, int width) {
		return QRect(rtl ? total - x - width : x, 0, width, 1);
	};
	return {
		.rail = span(0, rail),
		.dialogs = span(rail, dialogs),
		.chat = span(rail + dialogs, chat),
		.info = span(rail + dialogs + chat, info),
		.dialogsDivider = span(rail + dialogs, (dialogs && chat) ? 1 : 0),
		.infoDivider = span(rail + dialogs + chat - 1, info ? 1 : 0),
		.dialogsLeadingInset = qRound(.10 * scale),
		.chatLeadingInset = qRound(.48 * scale),
		.chatTrailingInset = qRound(.11 * scale),
	};
}

[[nodiscard]] QColor ModeColor(BadgeKind mode) {
	const auto colors = std::array{
		QColor(0x2f, 0x7d, 0x44), QColor(0x25, 0x63, 0xeb),
		QColor(0x6d, 0x28, 0xd9), QColor(0x0f, 0x76, 0x6e),
		QColor(0x7c, 0x3a, 0xed), QColor(0x6d, 0x28, 0xd9),
		QColor(0xb4, 0x53, 0x09), QColor(0x03, 0x69, 0xa1),
	};
	return colors[int(mode)];
}

[[nodiscard]] StatusState Idle() {
	auto state = StatusState();
	state.badgeText = u"NORMAL"_s;
	state.paneLabel = u"Chat"_s;
	state.localleaderKey = u","_s;
	state.hints = {
		{ u"SPC"_s, u"leader"_s },
		{ u"M-x"_s, u"run"_s }, { u"?"_s, u"help"_s },
	};
	state.unreadKnown = true;
	return state;
}

[[nodiscard]] QFont Font(int scale, bool bold, int pixels = 13) {
	auto font = QFont(u"Open Sans"_s);
	font.setPixelSize(qRound(pixels * scale / 100.));
	font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
	return font;
}

[[nodiscard]] QImage Render(
		int width,
		int scale,
		int above,
		const StatusColumns &columns,
		const StatusState &state,
		const Theme &theme,
		bool rtl = false) {
	const auto regular = Font(scale, false);
	const auto bold = Font(scale, true);
	const auto folderFont = Font(scale, true, 11);
	const auto height = QFontMetrics(regular).height();
	const auto barHeight = height + height / 2 + 2 * qRound(scale / 100.);
	auto image = QImage(width, above + barHeight, QImage::Format_ARGB32_Premultiplied);
	image.fill(theme.palette.window);
	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	if (above) {
		const auto tops = QRect(0, 0, width, above);
		const auto rail = StatusPaint::Across(columns.rail, tops);
		const auto dialogs = StatusPaint::Across(columns.dialogs, tops);
		const auto chat = StatusPaint::Across(columns.chat, tops);
		p.fillRect(rail, theme.palette.rail);
		p.fillRect(dialogs, theme.palette.dialogs);
		p.fillRect(chat, theme.palette.chat);
		if (!rail.isEmpty()) {
			StatusPaint::Text(p, rail, u"All chats"_s, folderFont,
				ReadableText(theme.palette.muted, theme.palette.rail), Qt::AlignHCenter);
		}
		if (!dialogs.isEmpty() && (above >= 100
			|| state.paneInDialogs || !UnreadStatusHints(state).empty())) {
			const auto row = StatusPaint::TextArea(dialogs, columns.dialogsLeadingInset, 0, rtl);
			const auto size = std::min(46, above - 8);
			const auto avatar = StatusPaint::Across(row, QRect(0, std::min(16, (above - size) / 2), width, size));
			p.setPen(Qt::NoPen);
			p.setBrush(ReadableText(theme.palette.accent, theme.palette.dialogs));
			p.drawEllipse(rtl ? avatar.right() + 1 - size : avatar.left(), avatar.top(), size, size);
			StatusPaint::Text(p, StatusPaint::TextArea(avatar, size + 12, 0, rtl),
				u"Fixture chat"_s, bold, theme.palette.text, rtl ? Qt::AlignRight : Qt::AlignLeft);
		}
		if (!chat.isEmpty()) {
			const auto wallpaper = chat.adjusted(0, 0, 0, -above / 3);
			p.save();
			p.setClipRect(wallpaper);
			p.fillRect(wallpaper, theme.wallpaper);
			p.setPen(theme.pattern);
			p.setBrush(Qt::NoBrush);
			for (auto y = -12; y < wallpaper.bottom() + 20; y += 26) {
				for (auto x = wallpaper.left() - 12; x < wallpaper.right() + 20; x += 34) {
					p.drawEllipse(x + (y % 3) * 5, y, 22, 22);
				}
			}
			const auto bubble = QRect(chat.left() + 21, 7, std::min(310, chat.width() / 2), above / 3);
			p.setPen(Qt::NoPen);
			p.setBrush(theme.bubble);
			p.drawRoundedRect(bubble, 7, 7);
			p.setPen(ReadableText(theme.palette.text, theme.bubble));
			p.setFont(regular);
			p.drawText(bubble.adjusted(12, 0, -10, 0), Qt::AlignVCenter, u"Fixture conversation"_s);
			p.restore();
			const auto composer = StatusPaint::TextArea(
				chat.adjusted(0, above * 2 / 3, 0, 0),
				*columns.chatLeadingInset, *columns.chatTrailingInset, rtl);
			StatusPaint::Text(p, composer, u"Write a message…"_s, regular,
				theme.palette.muted, rtl ? Qt::AlignRight : Qt::AlignLeft);
			const auto iconStart = rtl ? composer.left() : composer.right() - 17;
			const auto iconMiddle = composer.center().y();
			p.setPen(QPen(theme.palette.muted, 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(iconStart, iconMiddle - 8, 17, 16);
			p.drawLine(iconStart + 8, iconMiddle - 4, iconStart + 8, iconMiddle + 4);
		}
		p.fillRect(StatusPaint::Across(columns.info, tops), theme.palette.info);
		p.fillRect(StatusPaint::Across(columns.dialogsDivider, tops), theme.palette.divider);
		p.fillRect(StatusPaint::Across(columns.infoDivider, tops), theme.palette.divider);
	}
	PaintModalStatusBar(p, QRect(0, above, width, barHeight), columns, state, theme.palette,
		regular, bold, folderFont, ModeColor(state.badge), rtl);
	p.end();
	return image;
}

[[nodiscard]] bool HasInk(const QImage &image, QRect span, QColor ink) {
	for (auto y = 0; y < image.height(); ++y) {
		for (auto x = span.left(); x <= span.right(); ++x) {
			if (image.pixelColor(x, y) == ink) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] bool Filled(const QImage &image, QRect span, QColor background) {
	for (auto y = 0; y < image.height(); ++y) {
		for (auto x = span.left(); x <= span.right(); ++x) {
			if (image.pixelColor(x, y) != background) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] std::vector<StatusState> States() {
	auto result = std::vector<StatusState>();
	const auto add = [&](BadgeKind badge, QString name) -> StatusState & {
		auto state = Idle();
		state.badge = badge;
		state.badgeText = std::move(name);
		result.push_back(std::move(state));
		return result.back();
	};
	add(BadgeKind::Insert, u"INSERT"_s);
	auto &leader = add(BadgeKind::Leader, u"LEADER"_s);
	leader.pendingText = u"SPC b -"_s;
	leader.pendingGroup = u"+buffer"_s;
	leader.hints = { { u"DEL"_s, u"back"_s }, { u"ESC"_s, u"cancel"_s } };
	auto &local = add(BadgeKind::Local, u"LOCAL"_s);
	local.pendingText = u", -"_s;
	local.localleaderKey.clear();
	local.hints = { { u"DEL"_s, u"back"_s }, { u"ESC"_s, u"cancel"_s } };
	add(BadgeKind::Hints, u"HINTS"_s).hints = { { u"DEL"_s, u"back"_s }, { u"ESC"_s, u"cancel"_s } };
	auto &mx = add(BadgeKind::Palette, u"M-x"_s);
	mx.paneLabel = u"Command Palette"_s;
	mx.localleaderKey.clear();
	mx.hints = { { u"C-n/p"_s, u"move"_s }, { u"RET"_s, u"run"_s }, { u"ESC"_s, u"close"_s } };
	auto &describe = add(BadgeKind::DescribeKey, u"KEY?"_s);
	describe.message = u"Press a key sequence to describe it"_s;
	describe.hints = { { u"ESC"_s, u"cancel"_s } };
	auto &help = add(BadgeKind::Help, u"HELP"_s);
	help.paneLabel = u"Keyboard Cheatsheet"_s;
	help.localleaderKey.clear();
	help.hints = { { u"TAB"_s, u"all bindings"_s }, { u"j/k"_s, u"scroll"_s }, { u"ESC"_s, u"close"_s } };
	add(BadgeKind::Normal, u"NORMAL"_s).message = u"Copied message link"_s;
	auto &error = add(BadgeKind::Normal, u"NORMAL"_s);
	error.message = u"SPC g r is undefined"_s;
	error.messageIsError = true;
	auto &count = add(BadgeKind::Normal, u"NORMAL"_s);
	count.pendingText = u"12"_s;
	count.unreadDm = 3;
	count.unreadCh = 1;
	auto &list = add(BadgeKind::Normal, u"NORMAL"_s);
	list.paneLabel = u"Chats"_s;
	list.paneInDialogs = true;
	list.unreadGp = 4;
	return result;
}

void SavePreviews(const QDir &out, const QString &themeName, const Theme &theme) {
	const auto columns = Columns(79, 342, 697, 322);
	const auto image = Render(1440, 100, 128, columns, Idle(), theme);
	Check(image.save(out.filePath(u"a-idle-"_s + themeName + u"-final.png"_s)), "save idle");

	auto rows = std::vector<QImage>();
	for (const auto &state : States()) {
		rows.push_back(Render(1440, 100, 46, columns, state, theme));
	}
	auto zen = Idle();
	zen.zenMode = true;
	rows.push_back(Render(1440, 100, 46, Columns(0, 342, 1098, 0), zen, theme));
	zen.badge = BadgeKind::Leader;
	zen.badgeText = u"LEADER"_s;
	zen.pendingText = u"SPC -"_s;
	zen.hints = { { u"ESC"_s, u"cancel"_s } };
	rows.push_back(Render(1440, 100, 46, Columns(0, 0, 1440, 0), zen, theme));
	const auto height = rows.front().height() + 10;
	auto sheet = QImage(1440, height * int(rows.size()) - 10, QImage::Format_ARGB32_Premultiplied);
	sheet.fill(BlendColor(theme.palette.window, theme.palette.rail, .25));
	auto p = QPainter(&sheet);
	for (auto i = 0; i != int(rows.size()); ++i) {
		p.drawImage(0, i * height, rows[i]);
	}
	p.end();
	Check(sheet.save(out.filePath(u"a-states-"_s + themeName + u"-final.png"_s)), "save all states");
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QGuiApplication(argc, argv);
	if (argc != 3) return 2;
	Check(QFontDatabase::addApplicationFont(QString::fromUtf8(argv[2]) + u"/OpenSans-Regular.ttf"_s) >= 0,
		"Open Sans regular font");
	Check(QFontDatabase::addApplicationFont(QString::fromUtf8(argv[2]) + u"/OpenSans-SemiBold.ttf"_s) >= 0,
		"Open Sans semibold font");
	const auto out = QDir(QString::fromUtf8(argv[1]));
	Check(QDir().mkpath(out.absolutePath()), "create render directory");
	SavePreviews(out, u"light"_s, Light);
	SavePreviews(out, u"dark"_s, Dark);
	auto custom = Dark;
	custom.palette.chat = QColor(0x6a, 0x72, 0x78);
	custom.palette.dialogs = QColor(0x46, 0x4d, 0x70);
	custom.palette.info = QColor(0x91, 0x8a, 0xa8);
	custom.palette.rail = QColor(0x9c, 0x7b, 0x60);

	Check(UnreadStatusHints(Idle()).empty(), "no zero counts");
	auto counts = Idle();
	counts.unreadGp = 2;
	Check(UnreadStatusHints(counts).size() == 1
		&& UnreadStatusHints(counts).front().label == u"GP"_s, "skip zero buckets");
	counts.unreadKnown = false;
	Check(UnreadStatusHints(counts).empty(), "hide unavailable counts");

	const auto states = States();
	for (const auto &theme : { Light, Dark, custom }) {
		for (const auto scale : { 100, 125, 150, 200 }) {
			for (const auto width : { 640, 800, 1200 }) {
				for (const auto rtl : { false, true }) {
					const auto rail = qRound(.79 * scale);
					for (const auto list : { 0, width / 8, width / 4 }) {
						const auto info = width / 5;
						const auto columns = Columns(rail, list, width - rail - list - info, info, rtl, scale);
						const auto image = Render(width, scale, 0, columns, Idle(), theme, rtl);
						Check(Filled(image, columns.info, theme.palette.info), "info segment stays empty");
						const auto railPixel = image.pixelColor(columns.rail.center().x(), 0);
						Check(railPixel == theme.palette.rail, "rail surface meets its pane");
						const auto chatPixel = image.pixelColor(columns.chat.center().x(), 0);
						Check(chatPixel == theme.palette.chat, "chat surface meets its pane");
						const auto chatBar = StatusPaint::Across(columns.chat, image.rect());
						const auto chatText = StatusPaint::TextArea(
							chatBar, *columns.chatLeadingInset, *columns.chatTrailingInset, rtl);
						auto left = chatBar;
						auto right = chatBar;
						left.setRight(chatText.left() - 1);
						right.setLeft(chatText.right() + 1);
						if (list) {
							if (rtl) {
								right.adjust(0, 0, -1, 0);
							} else {
								left.adjust(1, 0, 0, 0);
							}
						}
						if (rtl) {
							left.adjust(1, 0, 0, 0);
						} else {
							right.adjust(0, 0, -1, 0);
						}
						Check(Filled(image, left, chatPixel) && Filled(image, right, chatPixel),
							"chat labels and hints stay within composer edges");
						const auto title = ReadableText(theme.palette.text, chatPixel);
						Check(HasInk(image, chatText, title), "painted text uses the composer text span");
						if (list) {
							Check(Filled(image, columns.dialogs, theme.palette.dialogs),
								"zero counts leave the list segment untouched");
							Check(image.pixelColor(columns.dialogsDivider.left(), image.height() - 1)
								!= chatPixel, "divider reaches the bottom pixel");
							auto countState = Idle();
							countState.unreadDm = 2;
							const auto positive = Render(width, scale, 0, columns, countState, theme, rtl);
							auto dialogsBar = StatusPaint::Across(columns.dialogs, positive.rect());
							const auto gutter = StatusPaint::TakeEdge(dialogsBar, columns.dialogsLeadingInset, rtl);
							const auto number = ReadableText(theme.palette.text, theme.palette.dialogs);
							const auto regular = Font(scale, false);
							const auto bold = Font(scale, true);
							const auto groupWidth = StatusPaint::TextWidth(regular, u"DM"_s)
								+ StatusPaint::TextWidth(regular, u" "_s) + StatusPaint::TextWidth(bold, u"2"_s);
							const auto room = dialogsBar.width() - QFontMetrics(regular).horizontalAdvance(u"  "_s);
							const auto count = StatusPaint::TakeEdge(dialogsBar, groupWidth, rtl);
							Check(Filled(positive, gutter, theme.palette.dialogs)
								&& (room >= groupWidth
									? HasInk(positive, count, number) : Filled(positive, count, theme.palette.dialogs)),
								"counts start at the avatar inset or yield on narrow lists");
						}
						for (const auto &state : states) {
							const auto mode = ReadableText(ModeColor(state.badge), railPixel);
							Check(TextContrast(mode, railPixel) >= 4.5, "mode text over actual rail");
							const auto stateImage = Render(width, scale, 0, columns, state, theme, rtl);
							Check(HasInk(stateImage, columns.rail, mode), "mode text stays visible");
							Check(Filled(stateImage, columns.info, theme.palette.info), "transient stays out of info");
							if (state.messageIsError) {
								const auto error = ReadableText(theme.palette.error, chatPixel);
								Check(TextContrast(error, chatPixel) >= 4.5
									&& HasInk(stateImage, columns.chat, error), "readable painted error");
							}
						}
						for (const auto base : { theme.palette.text, theme.palette.muted, theme.palette.accent }) {
							Check(TextContrast(ReadableText(base, chatPixel), chatPixel) >= 4.5,
								"chat text contrast over actual surface");
						}
					}
					for (const auto listOnly : { false, true }) {
						const auto pane = Columns(0, listOnly ? width : 0, listOnly ? 0 : width, 0, rtl, scale);
						auto state = Idle();
						state.message = u"A long notice must remain inside this column even at 200 percent"_s;
						const auto image = Render(width, scale, 0, pane, state, theme, rtl);
						const auto bg = listOnly ? theme.palette.dialogs : theme.palette.chat;
						const auto seam = QRect(0, 0, width, 1);
						Check(Filled(image.copy(seam), seam, bg), "no rail adds no top rule");
						auto first = image.rect();
						const auto mode = StatusPaint::TakeEdge(first, width / 6, rtl);
						Check(HasInk(image, mode, ReadableText(ModeColor(state.badge), bg)),
							"no-rail mode text stays visible");
					}
				}
			}
		}
	}
	std::cout << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}
