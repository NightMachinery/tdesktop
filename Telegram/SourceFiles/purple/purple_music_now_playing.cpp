#include "purple/purple_music_now_playing.h"

#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_button.h"
#include "media/player/media_player_instance.h"
#include "purple/purple_playlist_song_list.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_purple_music.h"

namespace Purple {
namespace {

using Type = AudioMsgId::Type;

class NowPlaying final : public Ui::RpWidget {
public:
	NowPlaying(QWidget *parent, not_null<Main::Session*> session, bool transport);
	void setVisibility(Fn<void(bool)> show);

protected:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *event) override;

private:
	void sync();
	void refreshCover(not_null<DocumentData*> document);
	[[nodiscard]] bool compact(int width) const;
	[[nodiscard]] int coverSide(int width) const;

	const not_null<Main::Session*> _session;
	const not_null<Ui::MediaSliderWheelless*> _progress;
	const not_null<Ui::FlatLabel*> _time;
	Ui::IconButton *_previous = nullptr;
	Ui::IconButton *_next = nullptr;
	Media::Player::PlayButton *_play = nullptr;
	Fn<void(bool)> _show;
	AudioMsgId _current;
	QString _title;
	QString _artist;
	QString _timeText;
	QImage _cover;
	QColor _tint;
	qint64 _artRevision = 0;
	int _artPixels = 0;
	bool _active = false;

};

NowPlaying::NowPlaying(
		QWidget *parent,
		not_null<Main::Session*> session,
		bool transport)
: Ui::RpWidget(parent)
, _session(session)
, _progress(Ui::CreateChild<Ui::MediaSliderWheelless>(
	this, st::mediaPlayerPanelPlayback))
, _time(Ui::CreateChild<Ui::FlatLabel>(this, QString(), st::boxLabel)) {
	_progress->setAccessibleName(u"Playback position"_q);
	const auto player = Media::Player::instance();
	_progress->setChangeProgressCallback([=](float64) {
		if (_active && !player->isSeeking(Type::Song)) player->startSeeking(Type::Song);
	});
	_progress->setChangeFinishedCallback([=](float64 progress) {
		if (_active) player->finishSeeking(Type::Song, progress);
	});
	if (transport) {
		_previous = Ui::CreateChild<Ui::IconButton>(this, st::mediaPlayerPreviousButton);
		_play = Ui::CreateChild<Media::Player::PlayButton>(this, st::mediaPlayerPlayButton);
		_next = Ui::CreateChild<Ui::IconButton>(this, st::mediaPlayerNextButton);
		_previous->setAccessibleName(tr::lng_shortcuts_media_previous(tr::now));
		_play->setAccessibleName(tr::lng_keys_legend_media_play(tr::now));
		_next->setAccessibleName(tr::lng_shortcuts_media_next(tr::now));
		_previous->setClickedCallback([=] { player->previous(Type::Song); });
		_play->setClickedCallback([=] { player->playPauseCancelClicked(Type::Song); });
		_next->setClickedCallback([=] { player->next(Type::Song); });
	}
	player->updatedNotifier() | rpl::on_next([=](const Media::Player::TrackState &state) {
		if (state.id.type() == Type::Song) sync();
	}, lifetime());
	rpl::merge(player->stops(Type::Song), player->playlistChanges(Type::Song),
		session->downloaderTaskFinished(),
		Core::App().passcodeLockChanges() | rpl::to_empty
	) | rpl::on_next([=] { sync(); }, lifetime());
}

void NowPlaying::setVisibility(Fn<void(bool)> show) {
	_show = std::move(show);
	sync();
	_show(_active);
}

bool NowPlaying::compact(int width) const {
	return width < st::purpleChatMusicWidth / 2;
}

int NowPlaying::coverSide(int width) const {
	return compact(width) ? st::purpleMusicNowCoverCompact : st::purpleMusicNowCover;
}

int NowPlaying::resizeGetHeight(int width) {
	const auto padding = st::purpleMusicNowPadding;
	const auto side = coverSide(width);
	const auto left = 2 * padding + side;
	const auto progressTop = (compact(width) ? padding + side + padding : side);
	const auto progressHeight = _time->height() + padding;
	_time->moveToRight(padding, progressTop, width);
	_progress->setGeometryToLeft(compact(width) ? padding : left, progressTop,
		std::max(1, width - (compact(width) ? padding : left) - _time->width() - 2 * padding),
		progressHeight);
	if (_play) {
		const auto y = compact(width) ? padding + st::purpleMusicNowTitle->height
			+ st::normalFont->height : padding;
		_next->moveToRight(padding, y, width);
		_play->moveToRight(padding + _next->width(), y, width);
		_previous->moveToRight(padding + _next->width() + _play->width(), y, width);
	}
	return std::max(side + 2 * padding, progressTop + progressHeight + padding);
}

void NowPlaying::sync() {
	const auto player = Media::Player::instance();
	const auto state = player->getState(Type::Song);
	const auto document = state.id.audio();
	const auto active = document && &document->session() == _session.get()
		&& !Core::App().passcodeLocked() && !Media::Player::IsStoppedOrStopping(state.state);
	if (_active != active) {
		_active = active;
		if (_show) _show(active);
	}
	if (!active) {
		_current = {};
		_cover = QImage();
		_tint = QColor();
		_artRevision = 0;
		return;
	}
	if (_current != state.id) {
		_current = state.id;
		const auto song = document->song();
		_title = song && !song->title.trimmed().isEmpty()
			? song->title : document->filename();
		_artist = song ? song->performer : QString();
		_cover = QImage();
		_tint = QColor();
		_artRevision = 0;
		setAccessibleName(_title);
		update();
	}
	refreshCover(document);
	if (_play) {
		using Button = Media::Player::PlayButton;
		_play->setState(state.waitingForData ? Button::State::Cancel
			: Media::Player::ShowPauseIcon(state.state) ? Button::State::Pause
			: Button::State::Play);
		_previous->setDisabled(!player->previousAvailable(Type::Song));
		_next->setDisabled(!player->nextAvailable(Type::Song));
	}
	_progress->setDisabled(state.length <= 0 || state.waitingForData);
	if (player->isSeeking(Type::Song)) return;
	_progress->setValue(state.length > 0
		? std::clamp(double(state.position) / state.length, 0., 1.) : 0.);
	const auto frequency = std::max(state.frequency, 1);
	const auto time = Ui::FormatDurationText(state.position / frequency)
		+ u" / "_q + Ui::FormatDurationText(state.length / frequency);
	if (_timeText != time) {
		_timeText = time;
		_time->setText(time);
		_time->resizeToNaturalWidth(width());
		resizeToWidth(width());
	}
}

void NowPlaying::refreshCover(not_null<DocumentData*> document) {
	const auto media = document->activeMediaView();
	const auto image = media ? (media->goodThumbnail()
		? media->goodThumbnail() : media->thumbnail()) : nullptr;
	if (!image) return;
	const auto original = image->original();
	const auto pixels = int(st::purpleMusicNowCover * devicePixelRatioF());
	if (original.isNull() || (original.cacheKey() == _artRevision && _artPixels == pixels)) return;
	_artRevision = original.cacheKey();
	_artPixels = pixels;
	const auto size = std::min(original.width(), original.height());
	_cover = original.copy((original.width() - size) / 2,
		(original.height() - size) / 2, size, size).scaled(
			pixels,
			pixels,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	const auto sample = _cover.scaled(
		1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
	_tint = QColor(sample.red() & 0xe0, sample.green() & 0xe0, sample.blue() & 0xe0, 26);
	update();
}

void NowPlaying::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto quality = PainterHighQualityEnabler(p);
	const auto radius = st::purpleMusicCoverRadius;
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(rect(), radius, radius);
	if (_tint.isValid()) {
		auto tint = QLinearGradient(rect().topLeft(), rect().bottomRight());
		tint.setColorAt(0., _tint);
		tint.setColorAt(1., Qt::transparent);
		p.setBrush(tint);
		p.drawRoundedRect(rect(), radius, radius);
	}
	const auto padding = st::purpleMusicNowPadding;
	const auto side = coverSide(width());
	PaintMusicSleeve(p, style::rtlrect(padding, padding, side, side, width()),
		_title, _artist, _cover);
	const auto left = 2 * padding + side;
	const auto controls = _play && !compact(width())
		? _previous->width() + _play->width() + _next->width() + padding
		: 0;
	const auto available = std::max(0, width() - left - padding - controls);
	p.setPen(st::windowFg);
	p.setFont(st::purpleMusicNowTitle);
	p.drawTextLeft(left, padding, width(), st::purpleMusicNowTitle->elided(_title, available));
	p.setFont(st::normalFont);
	p.drawTextLeft(left, padding + st::purpleMusicNowTitle->height,
		width(), st::normalFont->elided(_artist, available));
}

} // namespace

object_ptr<Ui::RpWidget> MakeMusicNowPlaying(
		QWidget *parent,
		not_null<Main::Session*> session,
		bool transport) {
	auto result = object_ptr<Ui::SlideWrap<NowPlaying>>(
		parent, object_ptr<NowPlaying>(parent, session, transport));
	const auto wrap = result.data();
	wrap->hide(anim::type::instant);
	wrap->entity()->setVisibility([=](bool show) {
		wrap->toggle(show, anim::type::instant);
	});
	return result;
}

} // namespace Purple
