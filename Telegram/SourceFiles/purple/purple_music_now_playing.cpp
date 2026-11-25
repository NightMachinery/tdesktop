#include "purple/purple_music_now_playing.h"

#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_button.h"
#include "media/player/media_player_instance.h"
#include "purple/purple_music_widgets.h"
#include "purple/purple_playlist_song_list.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"

#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_purple_music.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QIcon>
#include <QtWidgets/QStyle>

#include <cmath>

namespace Purple {
namespace {

using Type = AudioMsgId::Type;

class SkipButton final : public Ui::IconButton {
public:
	SkipButton(QWidget *parent, QStyle::StandardPixmap glyph);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const QIcon _glyph;
};

SkipButton::SkipButton(QWidget *parent, QStyle::StandardPixmap glyph)
: Ui::IconButton(parent, st::purpleMusicSkipButton)
, _glyph(style()->standardIcon(glyph)) {
}

void SkipButton::paintEvent(QPaintEvent *e) {
	Ui::IconButton::paintEvent(e);
	const auto side = st::purpleMusicSkipGlyphSize;
	auto mask = _glyph.pixmap(QSize(side, side), devicePixelRatioF());
	if (mask.isNull()) return;
	{
		auto p = QPainter(&mask);
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(QRectF(QPointF(), mask.deviceIndependentSize()),
			isEnabled() ? st::windowActiveTextFg->c : st::windowSubTextFg->c);
	}
	auto p = QPainter(this);
	const auto size = mask.deviceIndependentSize();
	p.drawPixmap(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), mask);
}

class NowPlaying final : public Ui::RpWidget {
public:
	NowPlaying(
		QWidget *parent,
		MusicTransportActions actions,
		const MusicShortcutHints &keys);
	void setState(MusicNowPlayingState state);

protected:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *) override;

private:
	[[nodiscard]] bool compact(int width) const;
	[[nodiscard]] int coverSide(int width) const;
	[[nodiscard]] int transportWidth() const;
	[[nodiscard]] int transportHeight() const;
	void placeTransport(int left, int top, int width);
	void updateProgressContrast();

	const not_null<Ui::MediaSliderWheelless*> _progress;
	const not_null<Ui::FlatLabel*> _time;
	Ui::IconButton *_previous = nullptr;
	Ui::IconButton *_next = nullptr;
	Media::Player::PlayButton *_play = nullptr;
	std::array<Ui::RpWidget*, 3> _hints = {};
	MusicNowPlayingState _state;
	QString _timeText;
	QColor _tint;

};

NowPlaying::NowPlaying(
		QWidget *parent,
		MusicTransportActions actions,
		const MusicShortcutHints &keys)
: Ui::RpWidget(parent)
, _progress(Ui::CreateChild<Ui::MediaSliderWheelless>(
	this, st::purpleMusicProgress))
, _time(Ui::CreateChild<Ui::FlatLabel>(this, QString(), st::boxLabel)) {
	_progress->setAccessibleName(tr::lng_purple_music_playback_position(tr::now));
	_progress->setChangeProgressCallback([=, seek = std::move(actions.startSeeking)](double) {
		if (seek && _state.active && !_state.seeking) seek();
	});
	_progress->setChangeFinishedCallback([=, seek = std::move(actions.finishSeeking)](double progress) {
		if (seek && _state.active) seek(progress);
	});
	style::PaletteChanged() | rpl::on_next([=] { updateProgressContrast(); }, lifetime());
	updateProgressContrast();
	if (actions.toggle) {
		_previous = Ui::CreateChild<SkipButton>(this, QStyle::SP_MediaSkipBackward);
		_play = Ui::CreateChild<Media::Player::PlayButton>(this, st::purpleMusicPlayButton);
		_next = Ui::CreateChild<SkipButton>(this, QStyle::SP_MediaSkipForward);
		_previous->setAccessibleName(tr::lng_shortcuts_media_previous(tr::now));
		_play->setAccessibleName(tr::lng_keys_legend_media_play(tr::now));
		_next->setAccessibleName(tr::lng_shortcuts_media_next(tr::now));
		_previous->setClickedCallback(std::move(actions.previous));
		_play->setClickedCallback(std::move(actions.toggle));
		_next->setClickedCallback(std::move(actions.next));
		_hints = {
			MakeMusicKeyHint(this, keys.previous, _previous).release(),
			MakeMusicKeyHint(this, keys.togglePlayback, _play).release(),
			MakeMusicKeyHint(this, keys.next, _next).release(),
		};
		for (const auto hint : _hints) {
			hint->widthValue() | rpl::skip(1) | rpl::on_next([=] {
				resizeToWidth(width());
			}, lifetime());
		}
	}
}

bool NowPlaying::compact(int width) const {
	return width < st::purpleMusicNowCompactWidth;
}

int NowPlaying::coverSide(int width) const {
	return (width < 2 * st::purpleMusicNowCover + 4 * st::purpleMusicNowPadding)
		? st::purpleMusicNowCoverCompact : st::purpleMusicNowCover;
}

int NowPlaying::transportWidth() const {
	if (!_play) return 0;
	return std::max(_previous->width(), _hints[0]->width())
		+ std::max(_play->width(), _hints[1]->width())
		+ std::max(_next->width(), _hints[2]->width())
		+ 2 * st::purpleMusicActionGap;
}

int NowPlaying::transportHeight() const {
	if (!_play) return 0;
	auto hintHeight = 0;
	for (const auto hint : _hints) {
		if (hint->width()) hintHeight = std::max(hintHeight, hint->height());
	}
	return std::max({ _previous->height(), _play->height(), _next->height() })
		+ (hintHeight ? st::purpleMusicShortcutGap + hintHeight : 0);
}

void NowPlaying::placeTransport(int left, int top, int width) {
	const auto controls = std::array<Ui::RpWidget*, 3>{ _previous, _play, _next };
	const auto height = std::max({ _previous->height(), _play->height(), _next->height() });
	for (auto i = 0; i != int(controls.size()); ++i) {
		const auto button = controls[i];
		const auto hint = _hints[i];
		const auto cell = std::max(button->width(), hint->width());
		button->moveToLeft(left + (cell - button->width()) / 2,
			top + (height - button->height()) / 2, width);
		hint->moveToLeft(left + (cell - hint->width()) / 2,
			top + height + st::purpleMusicShortcutGap, width);
		left += cell + st::purpleMusicActionGap;
	}
}

int NowPlaying::resizeGetHeight(int width) {
	const auto padding = st::purpleMusicNowPadding;
	const auto side = coverSide(width);
	const auto left = 2 * padding + side;
	const auto compactMode = compact(width);
	const auto metadataHeight = st::purpleMusicNowEyebrow->height
		+ st::purpleMusicNowTitle->height + st::normalFont->height;
	const auto transportTop = metadataHeight + (_play ? st::purpleMusicNowTransportGap : 0);
	const auto progressTop = compactMode
		? 2 * padding + std::max(side, transportTop + transportHeight())
		: padding + side - st::normalFont->height;
	_time->resizeToNaturalWidth(width);
	const auto progressHeight = std::max(_time->height(), st::normalFont->height);
	_time->moveToRight(padding, progressTop, width);
	_progress->setGeometryToLeft(compactMode ? padding : left, progressTop,
		std::max(1, width - (compactMode ? padding : left) - _time->width() - 2 * padding),
		progressHeight);
	if (_play) {
		placeTransport(compactMode ? left : width - padding - transportWidth(),
			compactMode ? padding + transportTop : padding + st::purpleMusicActionGap, width);
	}
	return std::max(side + 2 * padding, progressTop + progressHeight + padding);
}

void NowPlaying::updateProgressContrast() {
	const auto surface = st::windowBgOver->c;
	auto tinted = surface;
	if (_tint.isValid()) {
		auto ink = _tint;
		ink.setAlpha(255);
		tinted = anim::color(surface, ink, _tint.alphaF());
	}
	const auto preferred = anim::color(st::windowSubTextFg, st::windowFg, .15);
	const auto base = MusicRelativeLuminance(surface);
	const auto lightSurface = base > .18;
	const auto rail = [&] {
		if (!lightSurface) {
			return MusicReadableColor(MusicReadableColor(preferred, surface, 3.), tinted, 3.);
		}
		const auto darkest = std::min(base, MusicRelativeLuminance(tinted));
		const auto luminance = (darkest + .05) / 3. - .05;
		const auto gray = 1.055 * std::pow(luminance, 1. / 2.4) - .055;
		const auto tone = int(gray * 255.);
		return QColor(tone, tone, tone);
	}();
	_progress->setColorOverrides({
		.activeFg = lightSurface
			? std::optional(anim::color(st::windowActiveTextFg, st::windowFg, .12))
			: std::nullopt,
		.inactiveFg = rail,
	});
}

void NowPlaying::setState(MusicNowPlayingState state) {
	const auto changed = _state.title != state.title
		|| _state.artist != state.artist || _state.album != state.album
		|| _state.cover.cacheKey() != state.cover.cacheKey();
	const auto transportChanged = _state.playing != state.playing || _state.loading != state.loading;
	if (_state.cover.cacheKey() != state.cover.cacheKey()) {
		_tint = {};
		if (!state.cover.isNull()) {
			const auto sample = state.cover.scaled(
				1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
			_tint = QColor(sample.red() & 0xe0, sample.green() & 0xe0, sample.blue() & 0xe0, 26);
		}
		updateProgressContrast();
	}
	_state = std::move(state);
	if (changed) {
		setAccessibleName(_state.title);
		update();
	}
	if (_play) {
		using Button = Media::Player::PlayButton;
		if (transportChanged) {
			_play->setState(_state.loading ? Button::State::Cancel
				: _state.playing ? Button::State::Pause : Button::State::Play);
			if (anim::Disabled()) _play->finishTransform();
		}
		_previous->setDisabled(!_state.previousAvailable);
		_next->setDisabled(!_state.nextAvailable);
	}
	_progress->setDisabled(_state.durationSeconds <= 0 || _state.loading);
	if (_state.seeking) return;
	_progress->setValue(_state.progress);
	const auto time = u'\u2066' + Ui::FormatDurationText(_state.elapsedSeconds)
		+ u" / "_q + Ui::FormatDurationText(_state.durationSeconds) + u'\u2069';
	if (_timeText != time) {
		_timeText = time;
		_time->setText(time);
		resizeToWidth(width());
	}
}

QString ArtistAndAlbum(const MusicNowPlayingState &state, const QFontMetrics &metrics, int width) {
	if (state.artist.isEmpty()) return state.album;
	if (state.album.isEmpty()) return state.artist;
	const auto separator = u" · "_q;
	const auto remaining = width - metrics.horizontalAdvance(state.artist + separator);
	if (remaining <= metrics.horizontalAdvance(u"…"_q)) return state.artist;
	const auto direction = state.album.isRightToLeft()
		? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight;
	const auto album = metrics.elidedText(state.album, Qt::ElideRight, remaining, direction);
	return state.artist + separator + u'\u2068' + album + u'\u2069';
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
	if (_play) {
		p.setBrush(st::activeButtonBg);
		p.drawEllipse(_play->geometry());
	}
	const auto padding = st::purpleMusicNowPadding;
	const auto side = coverSide(width());
	PaintMusicSleeve(p, style::rtlrect(padding, padding, side, side, width()),
		_state.title, _state.artist, _state.cover);
	const auto left = 2 * padding + side;
	const auto controls = compact(width()) ? 0 : transportWidth() + padding;
	const auto available = std::max(0, width() - left - padding - controls);
	auto y = padding;
	p.setPen(st::windowFg);
	p.setFont(st::purpleMusicNowEyebrow);
	PaintMusicText(p, style::rtlrect(left, y, available, st::purpleMusicNowEyebrow->height, width()),
		tr::lng_purple_music_now_playing(tr::now));
	y += st::purpleMusicNowEyebrow->height;
	p.setFont(st::purpleMusicNowTitle);
	PaintMusicText(p, style::rtlrect(left, y, available, st::purpleMusicNowTitle->height, width()),
		_state.title);
	y += st::purpleMusicNowTitle->height;
	p.setFont(st::normalFont);
	PaintMusicText(p, style::rtlrect(left, y, available, st::normalFont->height, width()),
		ArtistAndAlbum(_state, QFontMetrics(p.font()), available));
}

struct PlaybackPresentation {
	qint64 artRevision = 0;
	QImage cover;
};

MusicNowPlayingState PlaybackState(
		not_null<Main::Session*> session,
		PlaybackPresentation &cache) {
	const auto player = Media::Player::instance();
	const auto track = player->getState(Type::Song);
	const auto document = track.id.audio();
	const auto active = document && &document->session() == session.get()
		&& !Core::App().passcodeLocked() && !Media::Player::IsStoppedOrStopping(track.state);
	if (!active) {
		cache = {};
		return {};
	}
	const auto media = document->activeMediaView();
	const auto image = media ? (media->goodThumbnail()
		? media->goodThumbnail() : media->thumbnail()) : nullptr;
	const auto raw = image ? image->original() : QImage();
	if (cache.artRevision != raw.cacheKey()) {
		cache.artRevision = raw.cacheKey();
		const auto side = std::min(raw.width(), raw.height());
		cache.cover = raw.isNull() ? QImage() : raw.copy(
			(raw.width() - side) / 2, (raw.height() - side) / 2, side, side);
	}
	const auto song = document->song();
	const auto frequency = std::max(track.frequency, 1);
	return {
		.title = song && !song->title.trimmed().isEmpty() ? song->title : document->filename(),
		.artist = song ? song->performer : QString(),
		.cover = cache.cover,
		.elapsedSeconds = track.position / frequency,
		.durationSeconds = track.length / frequency,
		.progress = track.length > 0 ? std::clamp(double(track.position) / track.length, 0., 1.) : 0.,
		.active = true,
		.playing = Media::Player::ShowPauseIcon(track.state),
		.loading = track.waitingForData,
		.seeking = player->isSeeking(Type::Song),
		.previousAvailable = player->previousAvailable(Type::Song),
		.nextAvailable = player->nextAvailable(Type::Song),
	};
}

} // namespace

object_ptr<Ui::RpWidget> MakeMusicNowPlaying(
		QWidget *parent,
		rpl::producer<MusicNowPlayingState> state,
		MusicTransportActions actions,
		const MusicShortcutHints &keys) {
	auto result = object_ptr<Ui::SlideWrap<NowPlaying>>(
		parent, object_ptr<NowPlaying>(parent, std::move(actions), keys));
	const auto wrap = result.data();
	wrap->hide(anim::type::instant);
	std::move(state) | rpl::on_next([=](MusicNowPlayingState state) {
		const auto active = state.active;
		wrap->entity()->setState(std::move(state));
		wrap->toggle(active, anim::type::instant);
	}, wrap->lifetime());
	return result;
}

object_ptr<Ui::RpWidget> MakeMusicNowPlaying(
		QWidget *parent,
		not_null<Main::Session*> session,
		bool transport,
		const MusicShortcutHints &keys) {
	const auto player = Media::Player::instance();
	auto state = rpl::make_producer<MusicNowPlayingState>([=](const auto &consumer) {
		auto lifetime = rpl::lifetime();
		const auto cache = lifetime.make_state<PlaybackPresentation>();
		const auto update = [=] { consumer.put_next(PlaybackState(session, *cache)); };
		rpl::merge(
			rpl::single(rpl::empty),
			player->updatedNotifier() | rpl::filter([](const Media::Player::TrackState &state) {
				return state.id.type() == Type::Song;
			}) | rpl::to_empty,
			player->stops(Type::Song),
			player->playlistChanges(Type::Song),
			session->downloaderTaskFinished(),
			Core::App().passcodeLockChanges() | rpl::to_empty
		) | rpl::on_next(update, lifetime);
		return lifetime;
	});
	auto actions = MusicTransportActions();
	actions.startSeeking = [=] { player->startSeeking(Type::Song); };
	actions.finishSeeking = [=](double progress) { player->finishSeeking(Type::Song, progress); };
	if (transport) {
		actions.previous = [=] { player->previous(Type::Song); };
		actions.toggle = [=] { player->playPauseCancelClicked(Type::Song); };
		actions.next = [=] { player->next(Type::Song); };
	}
	return MakeMusicNowPlaying(parent, std::move(state), std::move(actions), keys);
}

} // namespace Purple
