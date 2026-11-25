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

} // namespace

int ChatMusicWidth(int windowWidth) {
	return std::max(st::boxWidth, std::min(
		st::purpleChatMusicWidth, windowWidth - 2 * st::purpleChatMusicMargin));
}

ChatMusicLayout AddChatMusicLayout(
		not_null<Ui::GenericBox*> box,
		ChatMusicLayoutArgs args,
		const MusicShortcutHints &keys) {
	box->addRow(std::move(args.player));
	auto tiles = rpl::duplicate(args.tiles);
	box->addRow(MakeMusicViewControl(
		box, std::move(tiles), std::move(args.selectView), keys.view));
	const auto filter = box->addRow(MakeMusicFilter(box, keys.filter));
	const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
		box, tr::lng_contacts_loading(tr::now), st::boxLabel));
	const auto tracks = box->addRow(object_ptr<PlaylistSongList>(
		box, std::vector<PlaylistSongListRow>(), 0, 0, std::move(args.play),
		Fn<void(std::vector<uint64_t>)>(), PlaylistSongList::Mode::Library));
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
	auto action = object_ptr<Ui::RpWidget>(parent);
	const auto group = action.data();
	const auto button = Ui::CreateChild<Ui::RoundButton>(
		group, std::move(label), st::defaultLightButton);
	button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	button->setClickedCallback(std::move(callback));
	const auto hint = Ui::CreateChild<KeyHint>(group, keys, button);
	const auto place = [=] {
		const auto height = std::max(button->height(), hint->height());
		const auto hintLeft = button->width() + (hint->width() ? st::purpleMusicShortcutGap : 0);
		group->resize(hintLeft + hint->width(), height);
		button->moveToLeft(0, (height - button->height()) / 2);
		hint->moveToLeft(hintLeft, (height - hint->height()) / 2);
	};
	button->setWidthChangedCallback(place);
	hint->widthValue() | rpl::on_next(place, group->lifetime());
	return action;
}

object_ptr<Ui::RpWidget> MakeMusicHeader(
		QWidget *parent,
		const MusicKeyHint &playlistKeys,
		Fn<void()> back,
		Fn<void()> playlists,
		Fn<void(not_null<Ui::IconButton*>)> more) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto header = result.data();
	header->setFixedHeight(st::purpleMusicHeaderHeight);
	const auto previous = Ui::CreateChild<Ui::IconButton>(header, st::purpleMusicBack);
	previous->setAccessibleName(tr::lng_purple_music_back(tr::now));
	previous->setClickedCallback(std::move(back));
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		header, tr::lng_purple_music_title(), st::purpleMusicTitle);
	const auto playlist = MakeMusicActionButton(
		header, tr::lng_purple_playlists_title(), playlistKeys, std::move(playlists)).release();
	const auto menu = Ui::CreateChild<Ui::IconButton>(header, st::purpleMusicMore);
	menu->setAccessibleName(tr::lng_purple_music_options(tr::now));
	menu->setClickedCallback([=] { more(menu); });
	const auto place = [=] {
		const auto centerY = header->height() / 2;
		const auto gap = st::purpleMusicActionGap;
		previous->moveToLeft(0, centerY - previous->height() / 2);
		menu->moveToRight(0, centerY - menu->height() / 2);
		playlist->moveToRight(menu->width() + gap, centerY - playlist->height() / 2);
		const auto left = previous->width() + gap;
		const auto right = header->width() - menu->width() - playlist->width()
			- gap - st::purpleMusicShortcutGap;
		title->resizeToNaturalWidth(std::max(1, right - left));
		const auto x = std::clamp((header->width() - title->width()) / 2,
			left, std::max(left, right - title->width()));
		title->moveToLeft(x, centerY - title->height() / 2);
	};
	header->widthValue() | rpl::on_next(place, header->lifetime());
	playlist->widthValue() | rpl::on_next(place, header->lifetime());
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
	const auto list = Ui::CreateChild<Ui::RoundButton>(
		toggle, tr::lng_purple_music_list(), st::defaultLightButton);
	const auto grid = Ui::CreateChild<Ui::RoundButton>(
		toggle, tr::lng_purple_music_tiles(), st::defaultLightButton);
	list->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	grid->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	list->setClickedCallback([=] { select(false); });
	grid->setClickedCallback([=] { select(true); });
	const auto hint = Ui::CreateChild<KeyHint>(toggle, keys);
	const auto gap = st::purpleMusicShortcutGap;
	group->setFixedHeight(std::max({ list->height(), grid->height(), hint->height() }));
	const auto place = [=] {
		const auto height = group->height();
		const auto width = list->width() + grid->width() + gap
			+ (hint->width() ? gap + hint->width() : 0);
		toggle->resize(width, height);
		toggle->moveToRight(0, 0);
		list->moveToLeft(0, (height - list->height()) / 2);
		grid->moveToLeft(list->width() + gap, (height - grid->height()) / 2);
		hint->moveToRight(0, (height - hint->height()) / 2);
		title->resizeToNaturalWidth(std::max(1, group->width() - width - gap));
		title->moveToLeft(0, (height - title->height()) / 2);
	};
	list->setWidthChangedCallback(place);
	grid->setWidthChangedCallback(place);
	rpl::combine(group->widthValue(), hint->widthValue())
		| rpl::on_next(place, group->lifetime());
	std::move(tiles) | rpl::on_next([=](bool isTiles) {
		const auto brush = std::optional<QBrush>(st::windowBgRipple->b);
		list->setBrushOverride(isTiles ? std::nullopt : brush);
		grid->setBrushOverride(isTiles ? brush : std::nullopt);
	}, group->lifetime());
	return result;
}

void PaintMusicText(QPainter &p, const QRect &rect, const QString &text, int lines) {
	if (text.isEmpty() || rect.isEmpty()) return;
	const auto rtl = text.isRightToLeft();
	const auto direction = rtl ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight;
	const auto align = (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignAbsolute | Qt::AlignTop;
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
