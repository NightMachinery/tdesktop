#include "purple/purple_control_media.h"

#include "purple/purple_control.h"
#include "purple/purple_control_commands.h"
#include "purple/purple_gate.h"
#include "purple/purple_music_bookmark.h"
#include "purple/purple_playlists_menu.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h"
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

namespace Purple {
namespace {

constexpr auto kMusicLinkVersion = quint8(1);
AudioMsgId LastMusic;
qint64 LastMusicPlay = 0;
bool MusicPlaying = false;

QByteArray PackMusicLink(const PlaylistMessage &source) {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream << kMusicLinkVersion << quint64(source.id.peer.value)
		<< qint64(source.id.msg.bare) << quint64(source.documentId);
	return bytes;
}

std::optional<PlaylistMessage> UnpackMusicLink(const QByteArray &bytes) {
	auto stream = QDataStream(bytes);
	auto version = quint8();
	auto peer = quint64();
	auto message = qint64();
	auto document = quint64();
	stream >> version >> peer >> message >> document;
	if (stream.status() != QDataStream::Ok || !stream.atEnd()
		|| version != kMusicLinkVersion || !peer || !document
		|| message <= 0 || message > kMaximumPlaylistMessageId) {
		return std::nullopt;
	}
	return PlaylistMessage{ FullMsgId(PeerId(peer), MsgId(message)), document };
}

std::string MusicLinkKey(const QString &token) {
	return "purple.music.link." + token.toStdString();
}

bool LiveMusicLinkSession(Main::Session *session) {
	return session && !session->account().loggingOut()
		&& session->account().maybeSession() == session;
}

ControlResult MusicLinkDenied() {
	return ControlError(u"not-found"_q, u"Music source unavailable."_q);
}

ControlResult CurrentMusicLink() {
	auto error = AuthorizeControlCommand(u"media.music-link"_q, CurrentControlPolicy(),
		ControlMediaKind::Music, true);
	if (!error.code.isEmpty()) return error;
	using namespace Media::Player;
	const auto player = instance();
	const auto type = AudioMsgId::Type::Song;
	const auto current = player->current(type);
	const auto state = player->getState(type);
	const auto document = current.audio();
	if (!document || state.id.audio() != document
		|| state.id.contextId() != current.contextId()
		|| IsStoppedOrStopping(state.state)) {
		return { QJsonObject{ { u"state"_q, u"stopped"_q } } };
	}
	const auto session = &document->session();
	const auto source = PlaylistMessage{ current.contextId(), document->id };
	const auto bookmark = CaptureMusicBookmark(session, source);
	if (!bookmark) return MusicLinkDenied();
	return { QJsonObject{
		{ u"token"_q, bookmark->token },
		{ u"title"_q, bookmark->title },
		{ u"performer"_q, bookmark->performer },
		{ u"state"_q, IsPausedOrPausing(state.state) ? u"paused"_q : u"playing"_q },
		{ u"last_played_ms"_q, state.id == LastMusic && LastMusicPlay
			? QJsonValue(LastMusicPlay) : QJsonValue(QJsonValue::Null) },
	} };
}

void PlayMusicLink(QString token, std::function<bool()> active, ControlReply reply) {
	if (active && !active()) return;
	const auto &domain = Core::App().domain();
	for (const auto &entry : domain.accounts()) {
		const auto session = entry.account->maybeSession();
		if (!LiveMusicLinkSession(session)) continue;
		const auto source = UnpackMusicLink(
			session->local().readPref<QByteArray>(MusicLinkKey(token)));
		if (!source) continue;
		const auto weak = base::make_weak(session);
		const auto finish = [=] {
			if (active && !active()) return;
			auto error = AuthorizeControlCommand(u"media.play-music-link"_q,
				CurrentControlPolicy());
			if (!error.code.isEmpty()) {
				reply(std::move(error));
				return;
			}
			const auto live = weak.get();
			if (!LiveMusicLinkSession(live)
				|| !CapturePlaylistSong(live, *source)) {
				reply(MusicLinkDenied());
				return;
			}
			const auto item = live->data().message(source->id);
			const auto audio = AudioMsgId(item->media()->document(), source->id);
			const auto player = Media::Player::instance();
			player->play(audio);
			reply({ QJsonObject{ { u"accepted"_q, true } } });
		};
		if (session->data().message(source->id)) {
			finish();
			return;
		}
		const auto peer = session->data().peerLoaded(source->id.peer);
		if (peer || !peerIsChannel(source->id.peer)) {
			session->api().requestMessageData(peer, source->id.msg, finish);
			return;
		}
		break;
	}
	reply(MusicLinkDenied());
}

AudioMsgId::Type PlayerType(std::optional<ControlMediaType> type) {
	return !type ? Media::Player::instance()->getActiveType()
		: *type == ControlMediaType::Music ? AudioMsgId::Type::Song : AudioMsgId::Type::Voice;
}

ControlMediaSnapshot MediaSnapshot(std::optional<ControlMediaType> requested) {
	using namespace Media::Player;
	const auto player = instance();
	const auto type = PlayerType(requested);
	const auto current = player->current(type);
	const auto document = current.audio();
	auto result = ControlMediaSnapshot();
	if (!document) return result;
	result.type = type == AudioMsgId::Type::Song ? ControlMediaType::Music : ControlMediaType::Voice;
	const auto state = player->getState(type);
	result.state = IsStoppedOrStopping(state.state) ? ControlMediaState::Stopped
		: IsPausedOrPausing(state.state) ? ControlMediaState::Paused : ControlMediaState::Playing;
	result.position = ControlMediaMilliseconds(state.position, state.frequency);
	result.duration = ControlMediaMilliseconds(state.length, state.frequency);
	result.name = document->filename();
	result.path = document->filepath();
	result.mime = document->mimeString();
	const auto item = current.contextId() ? document->owner().message(current.contextId()) : nullptr;
	result.hidden = (result.type != ControlMediaType::Music)
		&& (item ? item->history()->purpleHiddenFromView() : Filtering(&document->session()));
	if (result.hidden) return result;
	if (const auto song = document->song()) {
		result.title = song->title;
		result.performer = song->performer;
	}
	if (item && result.type == ControlMediaType::Voice) {
		result.sender = item->from()->name();
		result.chat = item->history()->peer->name();
		result.date = item->date();
	}
	return result;
}

ControlResult ControlPlayback(ControlMediaAction action, std::optional<ControlMediaType> requested) {
	const auto player = Media::Player::instance();
	const auto type = PlayerType(requested);
	if (!player->current(type)) {
		return ControlError(u"not-found"_q, u"No current media item of this type."_q);
	}
	switch (action) {
	case ControlMediaAction::Toggle: player->playPause(type); break;
	case ControlMediaAction::Play: player->play(type); break;
	case ControlMediaAction::Pause: player->pause(type); break;
	}
	const auto state = player->getState(type).state;
	const auto text = Media::Player::IsStoppedOrStopping(state) ? u"stopped"_q
		: Media::Player::IsPausedOrPausing(state) ? u"paused"_q : u"playing"_q;
	return { QJsonObject{
		{ u"type"_q, type == AudioMsgId::Type::Song ? u"music"_q : u"voice"_q },
		{ u"state"_q, text },
	} };
}

void ReadMediaInfo(std::optional<ControlMediaType> type,
		std::function<bool()> active, ControlReply reply) {
	if (active && !active()) return;
	const auto snapshot = MediaSnapshot(type);
	const auto kind = !snapshot.type ? ControlMediaKind::Empty
		: *snapshot.type == ControlMediaType::Music ? ControlMediaKind::Music : ControlMediaKind::Voice;
	if (auto error = AuthorizeControlCommand(u"media.now"_q, CurrentControlPolicy(), kind, true)
		; !error.code.isEmpty()) {
		reply(std::move(error));
		return;
	}
	const auto candidate = snapshot.path;
	crl::async([=] {
		if (active && !active()) return;
		const auto path = ExistingControlMediaPath(candidate);
		crl::on_main([=] {
			if (active && !active()) return;
			auto result = ControlMediaInfo(MediaSnapshot(type), path);
			auto error = AuthorizeControlCommand(u"media.now"_q,
				CurrentControlPolicy(), result.mediaKind, true);
			reply(error.code.isEmpty() ? std::move(result) : std::move(error));
		});
	});
}

ControlResult LockApplication() {
	return ControlLock(Core::App().domain().local().hasLocalPasscode(), [] {
		if (!Core::App().passcodeLocked()) Core::App().lockByPasscode();
		return Core::App().passcodeLocked();
	});
}

}

std::optional<MusicBookmark> CaptureMusicBookmark(
		not_null<Main::Session*> session,
		const PlaylistMessage &source) {
	if (Core::App().passcodeLocked() || !LiveMusicLinkSession(session)) {
		return std::nullopt;
	}
	const auto input = CapturePlaylistSong(session, source);
	if (!input) return std::nullopt;
	auto &local = session->local();
	const auto reverseKey = "purple.music.document." + std::to_string(source.documentId);
	auto token = local.readPref<QString>(reverseKey);
	const auto recorded = IsControlMusicLinkToken(token)
		? UnpackMusicLink(local.readPref<QByteArray>(MusicLinkKey(token)))
		: std::nullopt;
	if (!recorded || recorded->documentId != source.documentId) {
		auto bytes = QByteArray(16, Qt::Uninitialized);
		base::RandomFill(bytes.data(), bytes.size());
		token = QString::fromLatin1(bytes.toHex());
	}
	local.writePref<QByteArray>(MusicLinkKey(token), PackMusicLink(source));
	local.writePref<QString>(reverseKey, token);
	return MusicBookmark{ std::move(token), input->meta.title, input->meta.performer };
}

void ObserveControlMusicPlayback(const Media::Player::TrackState &state) {
	using namespace Media::Player;
	const auto type = AudioMsgId::Type::Song;
	if (state.id.type() != type || instance()->isSeeking(type)) return;
	const auto changed = state.id != LastMusic;
	if (changed) {
		LastMusic = state.id;
		LastMusicPlay = 0;
	}
	const auto playing = !IsStoppedOrStopping(state.state)
		&& !IsPausedOrPausing(state.state);
	if (playing && (changed || !MusicPlaying)) {
		LastMusicPlay = QDateTime::currentMSecsSinceEpoch();
	}
	MusicPlaying = playing;
}

ControlMediaAdapter AppControlMediaAdapter() {
	return { ControlPlayback, ReadMediaInfo, LockApplication, CurrentMusicLink, PlayMusicLink };
}

}
