#include "purple/purple_music_widgets.h"

#include "lang/lang_keys.h"
#include "ui/controls/keys_legend.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"

#include "styles/style_keys_legend.h"
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
	KeyHint(QWidget *parent, const QStringList &keys);

protected:
	void paintEvent(QPaintEvent *) override;

private:
	std::vector<Ui::KeysLegend::Cap> _caps;

};

KeyHint::KeyHint(QWidget *parent, const QStringList &keys)
: Ui::RpWidget(parent) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto style = CapStyle();
	auto width = 0;
	for (const auto &key : keys) {
		if (width) width += style.capSkip;
		_caps.push_back(Ui::KeysLegend::MakeCap(key, style));
		width += _caps.back().width;
	}
	resize(width, Ui::KeysLegend::CapsMaxHeight(_caps, style.capHeight));
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

object_ptr<Ui::RpWidget> MakeMusicKeyHint(QWidget *parent, const QStringList &keys) {
	return object_ptr<KeyHint>(parent, keys);
}

object_ptr<Ui::RpWidget> MakeMusicActionButton(
		QWidget *parent,
		rpl::producer<QString> label,
		const QStringList &keys,
		Fn<void()> callback) {
	auto action = object_ptr<Ui::RpWidget>(parent);
	const auto group = action.data();
	const auto button = Ui::CreateChild<Ui::RoundButton>(
		group, std::move(label), st::defaultLightButton);
	button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	button->setClickedCallback(std::move(callback));
	const auto hint = Ui::CreateChild<KeyHint>(group, keys);
	button->setAccessibleDescription(keys.join(u' '));
	const auto place = [=] {
		const auto height = std::max(button->height(), hint->height());
		const auto hintLeft = button->width() + (hint->width() ? st::purpleMusicShortcutGap : 0);
		group->resize(hintLeft + hint->width(), height);
		button->moveToLeft(0, (height - button->height()) / 2);
		hint->moveToLeft(hintLeft, (height - hint->height()) / 2);
	};
	button->setWidthChangedCallback(place);
	place();
	return action;
}

object_ptr<Ui::RpWidget> MakeMusicHeader(
		QWidget *parent,
		const QStringList &playlistKeys,
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
		const auto centerWidth = std::max(1, header->width() - 2 * (menu->width()
			+ playlist->width() + 2 * gap));
		title->resizeToNaturalWidth(centerWidth);
		title->move((header->width() - title->width()) / 2, centerY - title->height() / 2);
	};
	header->widthValue() | rpl::on_next(place, header->lifetime());
	playlist->widthValue() | rpl::on_next(place, header->lifetime());
	return result;
}

object_ptr<Ui::InputField> MakeMusicFilter(QWidget *parent, const QStringList &keys) {
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
	field->sizeValue() | rpl::on_next([=](QSize size) {
		icon->resize(st::purpleMusicSearchIconWidth, size.height());
		icon->moveToLeft(0, 0, size.width());
		hint->moveToRight(st::purpleMusicShortcutGap, (size.height() - hint->height()) / 2);
	}, field->lifetime());
	return result;
}

object_ptr<Ui::RpWidget> MakeMusicViewControl(
		QWidget *parent,
		rpl::producer<bool> tiles,
		Fn<void(bool)> select,
		const QStringList &keys) {
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
	group->widthValue() | rpl::on_next(place, group->lifetime());
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
