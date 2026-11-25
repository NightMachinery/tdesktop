#include "purple/purple_music_widgets.h"

#include "lang/lang_keys.h"
#include "ui/controls/keys_legend.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"

#include "styles/style_keys_legend.h"
#include "styles/style_layers.h"
#include "styles/style_purple_music.h"
#include "styles/style_widgets.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QTextLayout>

#include <cmath>

namespace Purple {
namespace {

Ui::KeysLegend::St CapStyle() {
	return {
		.capStyle = &st::keysLegendCapStyle,
		.capHeight = st::keysLegendCapHeight,
		.capMinWidth = st::keysLegendCapMinWidth,
		.capMaxWidth = st::keysLegendCapMaxWidth,
		.capPadding = st::keysLegendCapPadding,
		.capRadius = st::keysLegendCapRadius,
		.capSkip = st::keysLegendCapSkip,
		.capBg = &st::keysLegendCapBg,
		.capBorder = &st::keysLegendCapBorder,
		.capFg = &st::keysLegendCapFg,
	};
}

class KeyHint final : public Ui::RpWidget {
public:
	KeyHint(QWidget *parent, const MusicKeyHint &keys, QWidget *accessibleOwner = nullptr);

protected:
	void paintEvent(QPaintEvent *) override;

private:
	void setKeys(const QStringList &keys);

	std::vector<Ui::KeysLegend::Cap> _caps;

};

KeyHint::KeyHint(
		QWidget *parent,
		const MusicKeyHint &keys,
		QWidget *accessibleOwner)
: Ui::RpWidget(parent) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setKeys({});
	const auto owner = QPointer(accessibleOwner);
	rpl::duplicate(keys) | rpl::on_next([=](const QStringList &values) {
		setKeys(values);
		if (owner) owner->setAccessibleDescription(values.join(u' '));
	}, lifetime());
}

void KeyHint::setKeys(const QStringList &keys) {
	_caps.clear();
	const auto style = CapStyle();
	auto width = 0;
	for (const auto &key : keys) {
		if (width) width += style.capSkip;
		_caps.push_back(Ui::KeysLegend::MakeCap(key, style));
		width += _caps.back().width;
	}
	resize(width, Ui::KeysLegend::CapsMaxHeight(_caps, style.capHeight));
	update();
}

void KeyHint::paintEvent(QPaintEvent *) {
	auto p = QPainter(this);
	const auto style = CapStyle();
	auto left = 0;
	for (const auto &cap : _caps) {
		Ui::KeysLegend::PaintCap(p, cap, left, (height() - cap.height) / 2, style);
		left += cap.width + style.capSkip;
	}
}

class MusicActionButton final : public Ui::RoundButton {
public:
	MusicActionButton(
		QWidget *parent,
		rpl::producer<QString> label,
		const MusicKeyHint &keys,
		Fn<void()> callback);

	QString accessibilityName() override;
	[[nodiscard]] int textBaseline() const;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void fit();

	const not_null<KeyHint*> _hint;
	QString _label;
	int _labelWidth = 0;
};

MusicActionButton::MusicActionButton(
		QWidget *parent,
		rpl::producer<QString> label,
		const MusicKeyHint &keys,
		Fn<void()> callback)
: Ui::RoundButton(parent, rpl::single(QString()), st::defaultLightButton)
, _hint(Ui::CreateChild<KeyHint>(this, keys, this)) {
	setClickedCallback(std::move(callback));
	std::move(label) | rpl::on_next([=](QString text) {
		_label = std::move(text);
		_labelWidth = QFontMetrics(st().style.font->f).horizontalAdvance(_label);
		fit();
	}, lifetime());
	_hint->widthValue() | rpl::on_next([=] { fit(); }, lifetime());
}

QString MusicActionButton::accessibilityName() {
	return _label;
}

int MusicActionButton::textBaseline() const {
	return st().textTop + st().style.font->ascent;
}

void MusicActionButton::fit() {
	const auto inset = st::purpleMusicButtonInset;
	const auto caps = _hint->width();
	setFullWidth(2 * inset + _labelWidth + (caps ? st::purpleMusicShortcutGap + caps : 0));
	_hint->moveToRight(inset, (height() - _hint->height()) / 2);
}

void MusicActionButton::paintEvent(QPaintEvent *e) {
	Ui::RoundButton::paintEvent(e);
	auto p = Painter(this);
	p.setFont(st().style.font);
	p.setPen((isOver() || isDown()) ? st().textFgOver : st().textFg);
	const auto area = style::rtlrect(st::purpleMusicButtonInset, 0, _labelWidth, height(), width());
	p.drawText(area.left(), textBaseline(), _label);
}

} // namespace

double MusicRelativeLuminance(QColor color) {
	const auto channel = [](double value) {
		return (value <= 0.04045)
			? value / 12.92
			: std::pow((value + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
		+ 0.0722 * channel(color.blueF());
}

QColor MusicReadableColor(QColor foreground, QColor background, double minimum) {
	const auto bg = MusicRelativeLuminance(background);
	const auto target = (bg > 0.18) ? 0. : 1.;
	for (auto step = 0; step != 9; ++step) {
		const auto fg = MusicRelativeLuminance(foreground);
		if ((std::max(bg, fg) + 0.05) / (std::min(bg, fg) + 0.05) >= minimum) {
			return foreground;
		}
		foreground = QColor::fromRgbF(
			(foreground.redF() + target) / 2.,
			(foreground.greenF() + target) / 2.,
			(foreground.blueF() + target) / 2.);
	}
	return QColor::fromRgbF(target, target, target);
}

int ChatMusicWidth(int windowWidth) {
	return std::max(st::boxWidth, std::min(
		st::purpleChatMusicWidth, windowWidth - 2 * st::purpleChatMusicMargin));
}

ChatMusicLayout AddChatMusicLayout(
		not_null<Ui::GenericBox*> box,
		ChatMusicLayoutArgs args,
		const MusicShortcutHints &keys) {
	box->addRow(std::move(args.player), st::purpleMusicNowMargin);
	auto tiles = rpl::duplicate(args.tiles);
	box->addRow(MakeMusicViewControl(
		box, std::move(tiles), std::move(args.selectView), keys.view), st::purpleMusicActionMargin);
	const auto filter = box->addRow(MakeMusicFilter(box, keys.filter), st::purpleMusicSearchMargin);
	const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
		box, tr::lng_contacts_loading(tr::now), st::boxLabel));
	const auto tracks = box->addRow(object_ptr<PlaylistSongList>(
		box, std::vector<PlaylistSongListRow>(), 0, 0, std::move(args.play),
		Fn<void(std::vector<uint64_t>)>(), PlaylistSongList::Mode::Library));
	tracks->reserveTileScrollbarSpace();
	std::move(args.tiles) | rpl::on_next([=](bool tiles) {
		tracks->setTiles(tiles);
	}, tracks->lifetime());
	args.selection.more = [=] { tracks->showActions(); };
	box->addRow(MakeMusicSelectionBar(
		box, std::move(args.marks), std::move(args.selection), keys));
	box->setTitle(rpl::single(u"Music shared in this chat"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addButton(rpl::single(u"Actions"_q), [=] { tracks->showActions(); });
	const auto more = box->addLeftButton(tr::lng_purple_music_more(), std::move(args.loadMore));
	more->hide();
	if (style::RightToLeft()) {
		box->setInitScrollCallback([=] {
			const auto body = box->verticalLayout();
			// The wrapper positions hidden RTL content before its width is ready.
			body->positionValue() | rpl::on_next([=](QPoint position) {
				if (!position.isNull()) body->move(0, 0);
			}, body->lifetime());
		});
	}
	return { filter, status, tracks, more };
}

object_ptr<Ui::RpWidget> MakeMusicKeyHint(
		QWidget *parent,
		const MusicKeyHint &keys,
		QWidget *accessibleOwner) {
	return object_ptr<KeyHint>(parent, keys, accessibleOwner);
}

object_ptr<Ui::RpWidget> MakeMusicActionButton(
		QWidget *parent,
		rpl::producer<QString> label,
		const MusicKeyHint &keys,
		Fn<void()> callback) {
	return object_ptr<MusicActionButton>(parent, std::move(label), keys, std::move(callback));
}

object_ptr<Ui::RpWidget> MakeMusicHeader(
		QWidget *parent,
		const MusicKeyHint &playlistKeys,
		Fn<void()> back,
		Fn<void()> playlists,
		Fn<void(not_null<Ui::IconButton*>, std::optional<QString>)> more) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto header = result.data();
	header->setFixedHeight(st::purpleMusicHeaderHeight);
	const auto previous = Ui::CreateChild<Ui::IconButton>(header, st::purpleMusicBack);
	previous->setAccessibleName(tr::lng_purple_music_back(tr::now));
	previous->setClickedCallback(std::move(back));
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		header, tr::lng_purple_music_title(), st::purpleMusicTitle);
	const auto playlist = Ui::CreateChild<MusicActionButton>(
		header, tr::lng_purple_playlists_title(), playlistKeys, std::move(playlists));
	const auto menu = Ui::CreateChild<Ui::IconButton>(header, st::purpleMusicMore);
	menu->setAccessibleName(tr::lng_purple_music_options(tr::now));
	menu->setClickedCallback([=] {
		more(menu, playlist->isHidden()
			? std::optional(playlist->accessibleDescription())
			: std::nullopt);
	});
	const auto place = [=] {
		const auto centerY = header->height() / 2;
		const auto gap = st::purpleMusicActionGap;
		const auto playlistTop = centerY - playlist->height() / 2;
		const auto baseline = playlistTop + playlist->textBaseline();
		previous->moveToLeft(0, centerY - previous->height() / 2);
		menu->moveToRight(0, centerY - menu->height() / 2);
		const auto left = previous->width() + gap;
		const auto menuEdge = header->width() - menu->width() - gap;
		const auto playlistEdge = menuEdge - playlist->width() - gap;
		const auto centered = (header->width() - title->naturalWidth()) / 2;
		const auto showPlaylist = centered >= left
			&& centered + title->naturalWidth() <= playlistEdge;
		playlist->setVisible(showPlaylist);
		playlist->moveToRight(menu->width() + gap, playlistTop);
		const auto right = showPlaylist ? playlistEdge : menuEdge;
		title->resizeToNaturalWidth(std::max(1, right - left));
		const auto x = std::clamp((header->width() - title->width()) / 2,
			left, std::max(left, right - title->width()));
		title->moveToLeft(x, baseline - title->getMargins().top()
			- st::purpleMusicTitle.style.font->ascent);
	};
	header->widthValue() | rpl::on_next(place, header->lifetime());
	playlist->naturalWidthValue() | rpl::on_next(place, header->lifetime());
	title->naturalWidthValue() | rpl::on_next(place, header->lifetime());
	return result;
}

object_ptr<Ui::InputField> MakeMusicFilter(QWidget *parent, const MusicKeyHint &keys) {
	auto result = object_ptr<Ui::InputField>(
		parent, st::purpleMusicSearch, Ui::InputField::Mode::SingleLine,
		tr::lng_purple_music_search());
	const auto field = result.data();
	field->customUpDown(true);
	const auto icon = Ui::CreateChild<Ui::RpWidget>(field);
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(icon);
		st::purpleMusicSearchIcon.paintInCenter(p, icon->rect());
	}, icon->lifetime());
	const auto hint = Ui::CreateChild<KeyHint>(field, keys);
	rpl::combine(field->sizeValue(), hint->widthValue()) | rpl::on_next(
		[=](QSize size, int) {
			const auto gap = st::purpleMusicShortcutGap;
			icon->resize(st::purpleMusicSearchIconWidth, size.height());
			icon->moveToLeft(0, 0, size.width());
			hint->moveToRight(gap, (size.height() - hint->height()) / 2);
		}, field->lifetime());
	hint->widthValue() | rpl::on_next([=](int width) {
		const auto rtl = style::RightToLeft();
		const auto margins = st::purpleMusicSearch.textMargins;
		const auto reserved = rtl ? margins.left() : margins.right();
		const auto extra = std::max(0, width + 2 * st::purpleMusicShortcutGap - reserved);
		field->setAdditionalMargins(rtl ? QMargins(extra, 0, 0, 0) : QMargins(0, 0, extra, 0));
	}, field->lifetime());
	return result;
}

object_ptr<Ui::RpWidget> MakeMusicViewControl(
		QWidget *parent,
		rpl::producer<bool> tiles,
		Fn<void(bool)> select,
		const MusicKeyHint &keys) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto group = result.data();
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		group, tr::lng_purple_music_tracks(), st::purpleMusicTitle);
	const auto toggle = Ui::CreateChild<Ui::RpWidget>(group);
	const auto mode = group->lifetime().make_state<rpl::variable<bool>>();
	std::move(tiles) | rpl::on_next([=](bool isTiles) {
		*mode = isTiles;
	}, group->lifetime());
	const auto forView = [&](bool targetTiles) -> MusicKeyHint {
		return rpl::combine(mode->value(), rpl::duplicate(keys)) | rpl::map(
			[=](bool isTiles, const QStringList &caps) {
				return isTiles == targetTiles ? QStringList() : caps;
			});
	};
	const auto list = Ui::CreateChild<MusicActionButton>(
		toggle, tr::lng_purple_music_list(), forView(false), [=] { select(false); });
	const auto grid = Ui::CreateChild<MusicActionButton>(
		toggle, tr::lng_purple_music_tiles(), forView(true), [=] { select(true); });
	const auto gap = st::purpleMusicShortcutGap;
	group->setFixedHeight(std::max(list->height(), grid->height()));
	const auto place = [=] {
		const auto height = group->height();
		const auto width = list->width() + grid->width() + gap;
		toggle->resize(width, height);
		toggle->moveToRight(0, 0);
		list->moveToLeft(0, (height - list->height()) / 2);
		grid->moveToLeft(list->width() + gap, (height - grid->height()) / 2);
		title->resizeToNaturalWidth(std::max(1, group->width() - width - gap));
		title->moveToLeft(0, (height - title->height()) / 2);
	};
	rpl::combine(group->widthValue(), list->naturalWidthValue(), grid->naturalWidthValue())
		| rpl::on_next(place, group->lifetime());
	mode->value() | rpl::on_next([=](bool isTiles) {
		const auto brush = std::optional<QBrush>(st::windowBgRipple->b);
		list->setBrushOverride(isTiles ? std::nullopt : brush);
		grid->setBrushOverride(isTiles ? brush : std::nullopt);
	}, group->lifetime());
	return result;
}

void PaintMusicText(
		QPainter &p,
		const QRect &rect,
		const QString &text,
		int lines,
		Qt::Alignment horizontal) {
	if (text.isEmpty() || rect.isEmpty()) return;
	const auto rtl = text.isRightToLeft();
	const auto direction = rtl ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight;
	const auto align = (horizontal ? horizontal : (rtl ? Qt::AlignRight : Qt::AlignLeft))
		| Qt::AlignAbsolute | Qt::AlignTop;
	const auto metrics = QFontMetrics(p.font());
	if (lines == 1 || metrics.horizontalAdvance(text) <= rect.width()) {
		p.drawText(rect, align | direction,
			metrics.elidedText(text, Qt::ElideRight, rect.width(), direction));
		return;
	}
	auto layout = QTextLayout(text, p.font());
	auto option = QTextOption();
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	option.setTextDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
	layout.setTextOption(option);
	layout.beginLayout();
	auto first = layout.createLine();
	first.setLineWidth(rect.width());
	layout.endLayout();
	const auto split = first.textLength();
	const auto lineHeight = rect.height() / lines;
	p.drawText(QRect(rect.topLeft(), QSize(rect.width(), lineHeight)),
		align | direction, text.left(split).trimmed());
	p.drawText(rect.adjusted(0, lineHeight, 0, 0), align | direction,
		metrics.elidedText(text.mid(split).trimmed(), Qt::ElideRight, rect.width(), direction));
}

} // namespace Purple
