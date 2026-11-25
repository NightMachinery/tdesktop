#include "purple/purple_playlist_player.h"

#include "purple/hooks/playlist_player.h"
#include "purple/hooks/media_playback.h"

#include "purple/purple_config.h"
#include "purple/purple_playlists_menu.h"
#include "purple/purple_playlists_ops.h"
#include "purple/purple_playlists_policy.h"
#include "purple/purple_playlists_resolver.h"
#include "purple/purple_playlists_service.h"
#include "purple/purple_user_control.h"
#include "base/random.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/media_common.h"
#include "media/player/media_player_instance.h"

#include <rpl/event_stream.h>
#include <rpl/rpl.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <numeric>

namespace Purple {
namespace {

using Type = AudioMsgId::Type;
constexpr auto kResolveWindow = std::size_t(4);
[[nodiscard]] bool SamePlaylistPlaybackSource(
		const AudioMsgId &a,
		const AudioMsgId &b) {
	return a.audio() == b.audio()
		&& a.contextId() == b.contextId()
		&& a.type() == b.type();
}

std::optional<std::size_t> PlaylistIndexForDocument(
		const std::vector<PlaylistResolveSong> &songs,
		uint64_t documentId) {
	const auto found = std::find_if(
		songs.begin(),
		songs.end(),
		[&](const auto &song) { return song.documentId == documentId; });
	return found != songs.end()
		? std::optional<std::size_t>(found - songs.begin())
		: std::nullopt;
}

std::size_t PlaylistStartIndex(
		std::size_t size,
		std::optional<std::size_t> selected,
		std::optional<std::size_t> current,
		Media::OrderMode mode,
		const Fn<std::size_t(std::size_t)> &random) {
	if (!size) return 0;
	if (selected) return *selected;
	if (mode == Media::OrderMode::Reverse) return size - 1;
	if (mode != Media::OrderMode::Shuffle) return 0;
	return current ? *current : random(size);
}

std::vector<std::size_t> PlaylistOrderIndices(
		std::size_t size,
		std::size_t selected,
		Media::OrderMode mode,
		const Fn<std::size_t(std::size_t)> &random) {
	auto result = std::vector<std::size_t>(size);
	std::iota(result.begin(), result.end(), std::size_t(0));
	if (mode == Media::OrderMode::Reverse) {
		std::reverse(result.begin(), result.end());
	} else if (mode == Media::OrderMode::Shuffle && size) {
		Expects(selected < size);
		std::swap(result.front(), result[selected]);
		for (auto remaining = size - 1; remaining > 1; --remaining) {
			const auto offset = random(remaining);
			Expects(offset < remaining);
			std::swap(result[remaining], result[1 + offset]);
		}
	}
	return result;
}

std::vector<std::size_t> ResolveIndices(
		std::size_t size,
		std::size_t first,
		int direction,
		std::size_t count,
		bool repeatAll = false) {
	Expects(direction == -1 || direction == 1);
	auto result = std::vector<std::size_t>();
	const auto limit = std::min({ count, kResolveWindow, size });
	for (auto offset = std::size_t(0); offset != limit; ++offset) {
		auto index = std::ptrdiff_t(first) + direction * std::ptrdiff_t(offset);
		if (repeatAll) {
			const auto total = std::ptrdiff_t(size);
			index = (index % total + total) % total;
		} else if (index < 0 || std::size_t(index) >= size) {
			break;
		}
		result.push_back(std::size_t(index));
	}
	return result;
}

enum class ResolveAction { Play, Skip, Retry };

ResolveAction ActionForResolution(PlaylistSongStatus status) {
	switch (status) {
	case PlaylistSongStatus::Ready:
	case PlaylistSongStatus::EarlierCopy:
		return ResolveAction::Play;
	case PlaylistSongStatus::Deleted:
	case PlaylistSongStatus::NoAccess:
	case PlaylistSongStatus::Changed:
	case PlaylistSongStatus::Restricted:
	case PlaylistSongStatus::InvalidInput:
		return ResolveAction::Skip;
	case PlaylistSongStatus::Resolving:
	case PlaylistSongStatus::Failed:
		return ResolveAction::Retry;
	}
	Unexpected("Playlist resolution status.");
}

class PlaylistCursor final {
public:
	PlaylistCursor() = default;
	PlaylistCursor(const PlaylistCursor&) = delete;
	PlaylistCursor &operator=(const PlaylistCursor&) = delete;

	void reset(std::size_t size, std::ptrdiff_t position);
	void setRepeatAll(bool repeatAll);
	[[nodiscard]] bool repeatAll() const;
	void select(std::ptrdiff_t position, int direction);
	void skip();
	void skipUnresolved(const Fn<bool(std::size_t)> &resolved);
	void cancel();
	void retire();
	[[nodiscard]] std::optional<std::size_t> index() const;
	[[nodiscard]] bool stepAvailable(int delta) const;
	[[nodiscard]] std::ptrdiff_t position() const;
	[[nodiscard]] int direction() const;
	[[nodiscard]] Fn<bool()> admission() const;

private:
	struct Authority {
		uint64_t generation = 0;
		bool active = false;
	};
	std::shared_ptr<Authority> _authority = std::make_shared<Authority>();
	std::size_t _size = 0;
	std::ptrdiff_t _position = 0;
	std::size_t _remaining = 0;
	int _direction = 1;
	bool _repeatAll = false;

};

void PlaylistCursor::reset(std::size_t size, std::ptrdiff_t position) {
	_size = size;
	_authority->active = true;
	select(position, 1);
}

void PlaylistCursor::setRepeatAll(bool repeatAll) {
	_repeatAll = repeatAll;
}

bool PlaylistCursor::repeatAll() const {
	return _repeatAll;
}

void PlaylistCursor::select(std::ptrdiff_t position, int direction) {
	Expects(direction == -1 || direction == 1);
	cancel();
	const auto boundary = std::clamp(
		position,
		std::ptrdiff_t(-1),
		std::ptrdiff_t(_size));
	_position = _repeatAll && _size
		? (boundary + std::ptrdiff_t(_size)) % std::ptrdiff_t(_size)
		: boundary;
	_direction = direction;
	_remaining = _size;
}

void PlaylistCursor::skip() {
	if (_repeatAll && _size && _remaining > 1) {
		--_remaining;
		_position = (_position + _direction + std::ptrdiff_t(_size)) % std::ptrdiff_t(_size);
	} else if (_repeatAll) {
		_remaining = 0;
		_position = _direction > 0 ? std::ptrdiff_t(_size) : -1;
	} else {
		_position = std::clamp(
			_position + _direction,
			std::ptrdiff_t(-1),
			std::ptrdiff_t(_size));
	}
}

void PlaylistCursor::skipUnresolved(const Fn<bool(std::size_t)> &resolved) {
	while (const auto current = index()) {
		if (resolved(*current)) return;
		skip();
	}
}

void PlaylistCursor::cancel() {
	++_authority->generation;
}

void PlaylistCursor::retire() {
	_authority->active = false;
	cancel();
}

std::optional<std::size_t> PlaylistCursor::index() const {
	return (_position >= 0 && std::size_t(_position) < _size)
		? std::optional(std::size_t(_position))
		: std::nullopt;
}

bool PlaylistCursor::stepAvailable(int delta) const {
	const auto next = _position + delta;
	return _authority->active && _size && (_repeatAll
		|| (next >= 0 && std::size_t(next) < _size));
}

std::ptrdiff_t PlaylistCursor::position() const {
	return _position;
}

int PlaylistCursor::direction() const {
	return _direction;
}

Fn<bool()> PlaylistCursor::admission() const {
	const auto weak = std::weak_ptr<Authority>(_authority);
	const auto generation = _authority->generation;
	return [weak, generation] {
		const auto authority = weak.lock();
		return authority && authority->active && authority->generation == generation;
	};
}

class PlaylistInternalCalls final {
public:
	struct Call {
		PlaylistPlayerIntent intent;
		bool available = true;
	};
	[[nodiscard]] std::shared_ptr<Call> enter(PlaylistPlayerIntent intent) {
		auto call = std::make_shared<Call>(Call{ intent });
		_calls.push_back(call);
		return call;
	}
	void leave(const std::shared_ptr<Call> &call) {
		Expects(!_calls.empty() && _calls.back() == call);
		_calls.pop_back();
	}
	[[nodiscard]] bool take(PlaylistPlayerIntent intent) {
		if (_calls.empty() || !_calls.back()->available
			|| _calls.back()->intent != intent) return false;
		_calls.back()->available = false;
		return true;
	}

private:
	std::vector<std::shared_ptr<Call>> _calls;

};

class PlaylistHandoffState final
	: public std::enable_shared_from_this<PlaylistHandoffState> {
public:
	struct Owner { uint64_t generation = 0; };
	struct Frame { uint64_t generation = 0; };
	[[nodiscard]] std::shared_ptr<Owner> enter();
	void leave(const std::shared_ptr<Owner> &owner);
	[[nodiscard]] std::shared_ptr<Frame> begin(bool force = false);
	void finish(const std::shared_ptr<Frame> &frame);
	void cancel(const std::shared_ptr<Owner> &owner);
	void supersede();
	[[nodiscard]] bool activeCall() const;
	void queue(Fn<void()> resume);
	[[nodiscard]] bool requestDispatch();
	[[nodiscard]] Fn<void()> take();
	[[nodiscard]] bool current(const std::shared_ptr<Owner> &owner) const;
	[[nodiscard]] Fn<bool()> guard(const std::shared_ptr<Frame> &frame) const;
	[[nodiscard]] Fn<bool()> callback() const;

private:
	uint64_t _generation = 0;
	std::vector<std::shared_ptr<Owner>> _owners;
	std::vector<std::shared_ptr<Frame>> _frames;
	Fn<void()> _pending;
	uint64_t _pendingGeneration = 0;
	bool _scheduled = false;

};

std::shared_ptr<PlaylistHandoffState::Owner> PlaylistHandoffState::enter() {
	auto owner = std::make_shared<Owner>();
	_owners.push_back(owner);
	return owner;
}

void PlaylistHandoffState::leave(const std::shared_ptr<Owner> &owner) {
	Expects(!_owners.empty() && _owners.back() == owner);
	_owners.pop_back();
}

std::shared_ptr<PlaylistHandoffState::Frame> PlaylistHandoffState::begin(bool force) {
	if (_owners.empty() && _frames.empty()) {
		if (_pending) supersede();
		if (!force) return nullptr;
	}
	auto frame = std::make_shared<Frame>();
	frame->generation = ++_generation;
	_frames.push_back(frame);
	if (!_owners.empty() && !_owners.back()->generation) {
		_owners.back()->generation = frame->generation;
	}
	return frame;
}

void PlaylistHandoffState::finish(const std::shared_ptr<Frame> &frame) {
	if (!frame) return;
	Expects(!_frames.empty() && _frames.back() == frame);
	_frames.pop_back();
}

void PlaylistHandoffState::cancel(const std::shared_ptr<Owner> &owner) {
	if (owner->generation && owner->generation == _generation) ++_generation;
}

void PlaylistHandoffState::supersede() {
	if (!_owners.empty() || !_frames.empty() || _pending) ++_generation;
	_pending = nullptr;
	_pendingGeneration = 0;
}

bool PlaylistHandoffState::activeCall() const {
	return !_frames.empty();
}

void PlaylistHandoffState::queue(Fn<void()> resume) {
	_pendingGeneration = ++_generation;
	_pending = std::move(resume);
}

bool PlaylistHandoffState::requestDispatch() {
	if (_scheduled || !_pending) return false;
	_scheduled = true;
	return true;
}

Fn<void()> PlaylistHandoffState::take() {
	_scheduled = false;
	if (!_frames.empty()) return nullptr;
	auto result = _pendingGeneration == _generation ? std::move(_pending) : Fn<void()>();
	_pending = nullptr;
	_pendingGeneration = 0;
	return result;
}

bool PlaylistHandoffState::current(const std::shared_ptr<Owner> &owner) const {
	return owner->generation && owner->generation == _generation;
}

Fn<bool()> PlaylistHandoffState::guard(const std::shared_ptr<Frame> &frame) const {
	if (!frame) return [] { return true; };
	const auto weak = weak_from_this();
	const auto generation = frame->generation;
	return [weak, generation] {
		const auto self = weak.lock();
		return self && self->_generation == generation;
	};
}

Fn<bool()> PlaylistHandoffState::callback() const {
	return guard(_frames.empty() ? nullptr : _frames.back());
}

class PlaylistPlayback final
	: public std::enable_shared_from_this<PlaylistPlayback> {
public:
	PlaylistPlayback(
		not_null<Main::Session*> session,
		QString id,
		std::optional<uint64_t> documentId);
	void start();
	void retry();
	void pauseForExpiry();
	[[nodiscard]] bool resumeAfterExpiry(const AudioMsgId &current, bool restart);
	[[nodiscard]] bool deferExpiryResume(Fn<void()> resume);
	[[nodiscard]] bool cancelExpiryResume();
	void retire(bool stopOwned, bool immediate = false);
	[[nodiscard]] bool belongsTo(Main::Session *session) const;
	[[nodiscard]] bool owns(const AudioMsgId &current);
	[[nodiscard]] bool claimsShuffle(const AudioMsgId &current);
	[[nodiscard]] bool claimsUi(const AudioMsgId &current) const;
	[[nodiscard]] bool move(int delta, bool autonext);
	[[nodiscard]] bool available(int delta) const;
	[[nodiscard]] PlaylistPlaybackStatus status() const;

private:
	[[nodiscard]] bool bound() const;
	[[nodiscard]] bool check(const Fn<bool()> &admission);
	[[nodiscard]] bool matchesCurrent(const AudioMsgId &current) const;
	void observe();
	void updateModes();
	[[nodiscard]] std::optional<PlaylistSongResolution> resolutionForCurrent(
		const AudioMsgId &current) const;
	void reorder(Media::OrderMode mode, uint64_t selected);
	void trimCache();
	[[nodiscard]] std::optional<std::size_t> savedIndex() const;
	void load();
	void loaded(PlaylistAccountResult result);
	void resolve();
	void requestResolution(
		std::size_t first,
		std::size_t count,
		bool warm);
	void prefetch();
	void resolved(
		uint64_t request,
		std::vector<std::size_t> indices,
		bool warm,
		std::vector<PlaylistSongResolution> results);
	void consume();
	void play(const PlaylistSongResolution &resolution);
	void retireResolver();
	void cancel(bool keepCache = false);
	void publish(
		PlaylistPlaybackPhase phase,
		PlaylistPlaybackProblem problem = PlaylistPlaybackProblem::None,
		std::optional<PlaylistSongStatus> sourceStatus = std::nullopt);

	base::weak_ptr<Main::Session> _session;
	AccountKey _account;
	std::optional<AccountRoutingSnapshot> _route;
	QString _id;
	std::optional<uint64_t> _selected;
	std::vector<PlaylistResolveSong> _savedSongs;
	std::vector<PlaylistResolveSong> _songs;
	Media::OrderMode _orderMode = Media::OrderMode::Default;
	PlaylistCursor _cursor;
	std::map<std::size_t, PlaylistSongResolution> _cache;
	std::unique_ptr<DesktopPlaylistResolver> _resolver;
	std::vector<std::shared_ptr<DesktopPlaylistResolver>> _retiringResolvers;
	AudioMsgId _expected;
	AudioMsgId _transitionFrom;
	PlaylistPlaybackStatus _status;
	uint64_t _request = 0;
	Fn<void()> _cancelHandoff;
	bool _loaded = false;
	bool _automatic = false;
	bool _retired = false;
	bool _ownsCurrent = false;
	bool _switching = false;
	bool _pausedForExpiry = false;
	bool _expiryResumePending = false;
	rpl::lifetime _lifetime;

};

std::shared_ptr<PlaylistPlayback> Active;
rpl::event_stream<> Changes;

std::map<Main::Session*, std::deque<PlaylistMessage>> MusicQueues;

bool PlayQueuedMusic(not_null<Main::Session*> session) {
	if (Core::App().passcodeLocked() || session->account().loggingOut()) return false;
	const auto found = MusicQueues.find(session.get());
	if (found == MusicQueues.end()) return false;
	auto &queue = found->second;
	while (!queue.empty()) {
		const auto message = queue.front();
		queue.pop_front();
		if (!CapturePlaylistSong(session, message)) continue;
		const auto item = session->data().message(message.id);
		if (!item || !item->media() || !item->media()->document()) continue;
		Media::Player::instance()->play(
			AudioMsgId(item->media()->document(), message.id),
			Media::Player::PlaylistContext{ item->topicRootId() });
		return true;
	}
	return false;
}
bool PlayerFinished = false;
uint64_t StartGeneration = 0;
const auto Handoffs = std::make_shared<PlaylistHandoffState>();
std::shared_ptr<int> SongStream;
const auto InternalCalls = std::make_shared<PlaylistInternalCalls>();

void ScheduleHandoffDispatch(const std::shared_ptr<PlaylistHandoffState> &state) {
	if (!state->requestDispatch()) return;
	crl::on_main([state] {
		if (auto resume = state->take()) resume();
	});
}

void RequireMainThread() {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
}

PeerId SourcePeer(const PlaylistSourceRef &source, uint64_t selfUserId) {
	switch (source.peer.kind) {
	case PeerTokenKind::Self: return peerFromUser(UserId(selfUserId));
	case PeerTokenKind::User: return peerFromUser(UserId(source.peer.id));
	case PeerTokenKind::Chat: return peerFromChat(ChatId(source.peer.id));
	case PeerTokenKind::Channel: return peerFromChannel(ChannelId(source.peer.id));
	}
	return PeerId();
}

DocumentData *ResolvedDocument(
		not_null<Main::Session*> session,
		const PlaylistSongResolution &resolution,
		HistoryItem *&item) {
	const auto id = FullMsgId(
		SourcePeer(resolution.source, SessionAccountKey(session).userId),
		MsgId(resolution.source.msg));
	item = session->data().message(id);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!item || item->isEmpty() || item->fullId() != id
		|| item->id.bare <= 0 || item->id.bare > kMaximumPlaylistMessageId
		|| !document || document->id != resolution.documentId
		|| &item->history()->session() != session.get()
		|| &document->session() != session.get()
		|| !item->computeUnavailableReason().isEmpty()) {
		return nullptr;
	}
	const auto candidate = PlaylistSongCandidate{
		.documentId = document->id,
		.messageId = int32_t(item->id.bare),
		.hasAudioAttribute = document->isSong(),
		.isVoiceMessage = document->isVoiceMessage(),
		.isRegularServerMessage = item->isRegular(),
		.isMainMessageMedia = dynamic_cast<Data::MediaFile*>(media) != nullptr,
		.isScheduled = item->isScheduled(),
		.isLocalUnsent = item->isLocal() || item->isSending(),
		.isSponsored = item->isSponsored(),
		.hasSelfDestructTimer = bool(media->ttlSeconds()) || bool(item->ttlDestroyAt()),
		.isProtectedContent = !item->allowsForward(),
	};
	return QualifySong(candidate, SaveRestrictionsDisabled()) ? document : nullptr;
}

PlaylistPlayback::PlaylistPlayback(
		not_null<Main::Session*> session,
		QString id,
		std::optional<uint64_t> documentId)
: _session(base::make_weak(session.get()))
, _account(SessionAccountKey(session))
, _route(CaptureAccountSyncRouting(session))
, _id(std::move(id))
, _selected(documentId)
, _expected(Media::Player::instance()->current(Type::Song)) {
	_status.playlistId = _id;
}

bool PlaylistPlayback::bound() const {
	const auto session = _session.get();
	return !_retired && !PlayerFinished && Active.get() == this && session && _route
		&& !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& SessionAccountKey(session) == _account
		&& AccountSyncRoutingCurrent(session, *_route);
}

bool PlaylistPlayback::check(const Fn<bool()> &admission) {
	if (_retired || !admission()) {
		return false;
	} else if (!bound()) {
		retire(true);
		return false;
	}
	return true;
}

bool PlaylistPlayback::matchesCurrent(const AudioMsgId &current) const {
	return SamePlaylistPlaybackSource(current, _expected)
		|| (_switching && SamePlaylistPlaybackSource(current, _transitionFrom));
}

bool PlaylistPlayback::belongsTo(Main::Session *session) const {
	return session && _session.get() == session;
}

std::optional<std::size_t> PlaylistPlayback::savedIndex() const {
	const auto index = _cursor.index();
	if (!index) return std::nullopt;
	const auto id = _songs[*index].documentId;
	const auto saved = std::find_if(
		_savedSongs.begin(),
		_savedSongs.end(),
		[&](const auto &song) { return song.documentId == id; });
	return saved != _savedSongs.end()
		? std::optional<std::size_t>(saved - _savedSongs.begin())
		: std::nullopt;
}

void PlaylistPlayback::publish(
		PlaylistPlaybackPhase phase,
		PlaylistPlaybackProblem problem,
		std::optional<PlaylistSongStatus> sourceStatus) {
	_status = {
		.playlistId = _id,
		.documentId = _cursor.index() ? _songs[*_cursor.index()].documentId : 0,
		.index = savedIndex(),
		.total = _songs.size(),
		.phase = phase,
		.problem = problem,
		.sourceStatus = sourceStatus,
	};
	Changes.fire({});
}

void PlaylistPlayback::retireResolver() {
	if (!_resolver) return;
	// WHY: callbacks can retire a resolver while it is still on the stack;
	// destruction belongs on the next main-loop turn.
	const auto raw = _resolver.get();
	_retiringResolvers.emplace_back(std::move(_resolver));
	const auto self = shared_from_this();
	crl::on_main([self, raw] {
		std::erase_if(self->_retiringResolvers, [raw](const auto &resolver) {
			return resolver.get() == raw;
		});
	});
}

void PlaylistPlayback::cancel(bool keepCache) {
	_automatic = false;
	_expiryResumePending = false;
	if (_cancelHandoff) _cancelHandoff();
	_cursor.cancel();
	++_request;
	retireResolver();
	if (!keepCache) _cache.clear();
}

void PlaylistPlayback::pauseForExpiry() {
	if (_retired) return;
	_pausedForExpiry = true;
	const auto current = Media::Player::instance()->current(Type::Song);
	const auto owned = _ownsCurrent && matchesCurrent(current);
	const auto playing = owned && current.audio()
		? std::find_if(_songs.begin(), _songs.end(), [&](const auto &song) {
			return song.documentId == current.audio()->id;
		}) : _songs.end();
	cancel(true);
	if (playing != _songs.end()) {
		_cursor.select(playing - _songs.begin(), 1);
	}
	if (_status.phase == PlaylistPlaybackPhase::Loading
		|| _status.phase == PlaylistPlaybackPhase::Resolving) {
		_status.phase = playing != _songs.end()
			? PlaylistPlaybackPhase::Playing
			: PlaylistPlaybackPhase::RetryRequired;
		_status.problem = PlaylistPlaybackProblem::None;
		_status.sourceStatus = std::nullopt;
	}
	_status.index = savedIndex();
	_status.documentId = _cursor.index() ? _songs[*_cursor.index()].documentId : 0;
	const auto weak = weak_from_this();
	const auto admission = _cursor.admission();
	crl::on_main([weak, admission] {
		if (const auto self = weak.lock()) {
			if (self->check(admission)) Changes.fire({});
		}
	});
}

bool PlaylistPlayback::resumeAfterExpiry(const AudioMsgId &current, bool restart) {
	if (!_pausedForExpiry || !_loaded || !_ownsCurrent) return false;
	if (!owns(current)) return false;
	_pausedForExpiry = false;
	cancel(true);
	if (!restart) {
		Changes.fire({});
		return false;
	}
	_expiryResumePending = true;
	resolve();
	return true;
}

bool PlaylistPlayback::deferExpiryResume(Fn<void()> resume) {
	if (!Handoffs->activeCall()) return false;
	if ((!_pausedForExpiry && !_expiryResumePending) || !_loaded || !_ownsCurrent) {
		return false;
	}
	if (!owns(Media::Player::instance()->current(Type::Song))) return false;
	cancel(true);
	_pausedForExpiry = true;
	_expiryResumePending = true;
	const auto weak = weak_from_this();
	const auto admission = _cursor.admission();
	Handoffs->queue([weak, admission, resume = std::move(resume)] {
		if (const auto self = weak.lock()) {
			const auto player = Media::Player::instance();
			if (self->check(admission) && self->owns(player->current(Type::Song))) {
				self->_expiryResumePending = false;
				const auto internal = PlaylistPlayerInternalCallGuard(
					Type::Song, PlaylistPlayerIntent::Resume);
				resume();
			}
		}
	});
	ScheduleHandoffDispatch(Handoffs);
	return true;
}

bool PlaylistPlayback::cancelExpiryResume() {
	if (!_expiryResumePending) return false;
	cancel(true);
	_pausedForExpiry = true;
	if (_status.phase == PlaylistPlaybackPhase::Resolving) {
		_status.phase = PlaylistPlaybackPhase::Playing;
		_status.problem = PlaylistPlaybackProblem::None;
		_status.sourceStatus = std::nullopt;
	}
	return true;
}

void PlaylistPlayback::retire(bool stopOwned, bool immediate) {
	const auto keepAlive = shared_from_this();
	if (_retired) {
		if (immediate) _retiringResolvers.clear();
		return;
	}
	_retired = true;
	_cursor.retire();
	cancel();
	if (immediate) _retiringResolvers.clear();
	_lifetime.destroy();
	const auto player = Media::Player::instance();
	const auto stop = stopOwned && _ownsCurrent
		&& SamePlaylistPlaybackSource(player->current(Type::Song), _expected);
	_ownsCurrent = false;
	if (stop) {
		const auto internal = PlaylistPlayerInternalCallGuard(
			Type::Song, PlaylistPlayerIntent::Stop);
		player->stop(Type::Song);
	}
	publish(PlaylistPlaybackPhase::Retired);
}

void PlaylistPlayback::observe() {
	const auto keepAlive = shared_from_this();
	if (!bound()) {
		retire(true);
		return;
	}
	const auto player = Media::Player::instance();
	const auto current = player->current(Type::Song);
	if (!matchesCurrent(current)) {
		retire(false);
	} else {
		updateModes();
	}
}

void PlaylistPlayback::start() {
	const auto keepAlive = shared_from_this();
	_cursor.reset(0, 0);
	if (!bound()) {
		retire(false);
		return;
	}
	const auto weak = weak_from_this();
	const auto player = Media::Player::instance();
	player->trackChanged() | rpl::filter([](Type type) {
		return type == Type::Song;
	}) | rpl::on_next([weak] {
		if (const auto self = weak.lock()) self->observe();
	}, _lifetime);
	player->playlistChanges(Type::Song) | rpl::on_next([weak] {
		if (const auto self = weak.lock()) self->observe();
	}, _lifetime);
	rpl::merge(
		Core::App().settings().playerOrderModeChanges() | rpl::to_empty,
		Core::App().settings().playerRepeatModeChanges() | rpl::to_empty,
		SettingsChanges()
	) | rpl::on_next([weak] {
		if (const auto self = weak.lock()) self->observe();
	}, _lifetime);
	player->updatedNotifier() | rpl::on_next([weak](const Media::Player::TrackState &state) {
		const auto self = weak.lock();
		if (self && state.id.type() == Type::Song && self->bound()
			&& SamePlaylistPlaybackSource(state.id, self->_expected) && self->_ownsCurrent
			&& state.state == Media::Player::State::StoppedAtError) {
			self->cancel();
			self->publish(PlaylistPlaybackPhase::RetryRequired, PlaylistPlaybackProblem::PlaybackRejected);
		}
	}, _lifetime);
	_session.get()->data().sessionDataAboutToBeCleared() | rpl::on_next([weak] {
		if (const auto self = weak.lock()) self->retire(false, true);
	}, _lifetime);
	_session.get()->lifetime().add([weak] {
		if (const auto self = weak.lock()) self->retire(false, true);
	});
	load();
}

void PlaylistPlayback::load() {
	cancel();
	_savedSongs.clear();
	_songs.clear();
	_loaded = false;
	_cursor.reset(0, 0);
	const auto admission = _cursor.admission();
	publish(PlaylistPlaybackPhase::Loading);
	if (!check(admission)) {
		return;
	}
	if (!IsPlaylistId(_id)) {
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			PlaylistPlaybackProblem::PlaylistUnavailable);
		return;
	}
	const auto weak = weak_from_this();
	ReadAccountPlaylists(_session.get(), [weak, admission](PlaylistAccountResult result) {
		if (const auto self = weak.lock()) {
			if (self->check(admission)) self->loaded(std::move(result));
		}
	});
}

void PlaylistPlayback::loaded(PlaylistAccountResult result) {
	if (!result || !result.snapshot.library) {
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			PlaylistPlaybackProblem::LibraryUnavailable);
		return;
	}
	const auto &library = *result.snapshot.library;
	const auto found = library.playlists.find(_id);
	if (found == library.playlists.end() || found->second.isPartial()
		|| found->second.deleted->value || library.purged.contains(_id)) {
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			PlaylistPlaybackProblem::PlaylistUnavailable);
		return;
	}
	const auto &playlist = found->second;
	for (const auto id : PlaylistVisibleSongIds(playlist)) {
		const auto &song = playlist.songs->at(id).value.source;
		if (song) _savedSongs.push_back({ id, *song });
	}
	const auto selected = _selected
		? std::find_if(_savedSongs.begin(), _savedSongs.end(), [&](const auto &song) {
			return song.documentId == *_selected;
		}) : _savedSongs.begin();
	if (_selected && selected == _savedSongs.end()) {
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			PlaylistPlaybackProblem::SelectedSongMissing);
		return;
	}
	_loaded = true;
	const auto mode = Core::App().settings().playerOrderMode();
	const auto current = Media::Player::instance()->current(Type::Song).audio();
	const auto playing = std::find_if(
		_savedSongs.begin(),
		_savedSongs.end(),
		[&](const auto &song) { return current && song.documentId == current->id; });
	const auto first = PlaylistStartIndex(
		_savedSongs.size(),
		_selected ? std::optional<std::size_t>(selected - _savedSongs.begin()) : std::nullopt,
		playing != _savedSongs.end()
			? std::optional<std::size_t>(playing - _savedSongs.begin()) : std::nullopt,
		mode,
		[](std::size_t count) { return std::size_t(base::RandomIndex(int(count))); });
	reorder(mode, _savedSongs.empty() ? 0 : _savedSongs[first].documentId);
	resolve();
}

void PlaylistPlayback::reorder(Media::OrderMode mode, uint64_t selected) {
	const auto saved = std::find_if(_savedSongs.begin(), _savedSongs.end(), [&](const auto &song) {
		return song.documentId == selected;
	});
	const auto order = PlaylistOrderIndices(
		_savedSongs.size(),
		saved != _savedSongs.end() ? saved - _savedSongs.begin() : 0,
		mode,
		[](std::size_t count) { return std::size_t(base::RandomIndex(int(count))); });
	_songs.clear();
	_songs.reserve(order.size());
	for (const auto index : order) _songs.push_back(_savedSongs[index]);
	_orderMode = mode;
	const auto position = std::find_if(_songs.begin(), _songs.end(), [&](const auto &song) {
		return song.documentId == selected;
	});
	_cursor.setRepeatAll(Core::App().settings().playerRepeatMode() == Media::RepeatMode::All);
	_cursor.reset(
		_songs.size(),
		position != _songs.end() ? position - _songs.begin() : 0);
}

auto PlaylistPlayback::resolutionForCurrent(const AudioMsgId &current) const
-> std::optional<PlaylistSongResolution> {
	if (!bound() || !_ownsCurrent || !current.audio()) return std::nullopt;
	const auto saved = PlaylistIndexForDocument(_savedSongs, current.audio()->id);
	if (!saved) return std::nullopt;
	const auto &song = _savedSongs[*saved];
	auto sources = std::vector<PlaylistSourceRef>{
		{ song.source.peer, song.source.msg, song.source.username },
	};
	sources.insert(sources.end(), song.source.alt.begin(), song.source.alt.end());
	for (auto index = std::size_t(0); index != sources.size(); ++index) {
		const auto &source = sources[index];
		if (SourcePeer(source, _account.userId) != current.contextId().peer
			|| MsgId(source.msg) != current.contextId().msg) {
			continue;
		}
		auto result = PlaylistSongResolution();
		result.documentId = song.documentId;
		result.source = source;
		result.sourceIndex = index;
		result.status = index
			? PlaylistSongStatus::EarlierCopy : PlaylistSongStatus::Ready;
		auto item = static_cast<HistoryItem*>(nullptr);
		const auto document = ResolvedDocument(_session.get(), result, item);
		if (document == current.audio()
			&& item && item->fullId() == current.contextId()) {
			return result;
		}
	}
	return std::nullopt;
}

void PlaylistPlayback::updateModes() {
	_cursor.setRepeatAll(Core::App().settings().playerRepeatMode() == Media::RepeatMode::All);
	const auto mode = Core::App().settings().playerOrderMode();
	if (!_loaded || _orderMode == mode) return;
	const auto player = Media::Player::instance();
	const auto current = player->current(Type::Song);
	const auto owned = _ownsCurrent && current.audio() && matchesCurrent(current);
	const auto selected = owned ? current.audio()->id
		: _cursor.index() ? _songs[*_cursor.index()].documentId : _selected.value_or(0);
	const auto sourceStatus = _status.documentId == selected
		? _status.sourceStatus : std::nullopt;
	const auto retry = _status.phase == PlaylistPlaybackPhase::RetryRequired
		&& (!owned || _status.documentId == selected);
	const auto problem = _status.problem;
	const auto expiry = _pausedForExpiry || _expiryResumePending;
	cancel();
	_pausedForExpiry = expiry;
	reorder(mode, selected);
	const auto admission = _cursor.admission();
	if (owned && current == player->current(Type::Song)) {
		const auto resolution = resolutionForCurrent(current);
		const auto index = PlaylistIndexForDocument(_songs, selected);
		if (resolution && index && check(admission)
			&& current == player->current(Type::Song)) {
			_cache.emplace(*index, *resolution);
		}
	}
	if (!check(admission)) return;
	if (retry || (!owned && expiry)) {
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			retry ? problem : PlaylistPlaybackProblem::None,
			sourceStatus);
	} else if (owned) {
		publish(
			PlaylistPlaybackPhase::Playing,
			PlaylistPlaybackProblem::None,
			sourceStatus);
		if (check(admission) && !_pausedForExpiry) prefetch();
	} else {
		resolve();
	}
}

void PlaylistPlayback::trimCache() {
	const auto index = _cursor.index();
	if (!index) return;
	const auto window = ResolveIndices(
		_songs.size(),
		*index,
		_cursor.direction(),
		kResolveWindow,
		_cursor.repeatAll());
	std::erase_if(_cache, [&](const auto &entry) {
		return std::find(window.begin(), window.end(), entry.first) == window.end();
	});
}

void PlaylistPlayback::resolve() {
	const auto keepAlive = shared_from_this();
	const auto admission = _cursor.admission();
	if (!check(admission)) {
		return;
	}
	if (_automatic) {
		_cursor.skipUnresolved([&](std::size_t index) { return _cache.contains(index); });
	}
	const auto index = _cursor.index();
	if (!index) {
		publish(PlaylistPlaybackPhase::Exhausted);
		return;
	}
	trimCache();
	if (_cache.contains(*index)) {
		consume();
		return;
	}
	requestResolution(*index, 1, false);
}

void PlaylistPlayback::requestResolution(
		std::size_t first,
		std::size_t count,
		bool warm) {
	retireResolver();
	const auto admission = _cursor.admission();
	const auto indices = ResolveIndices(
		_songs.size(),
		first,
		_cursor.direction(),
		count,
		_cursor.repeatAll());
	auto batch = std::vector<PlaylistResolveSong>();
	for (const auto index : indices) batch.push_back(_songs[index]);
	if (batch.empty()) return;
	const auto request = ++_request;
	const auto weak = weak_from_this();
	if (!warm) publish(PlaylistPlaybackPhase::Resolving);
	if (!check(admission)) return;
	_resolver = std::make_unique<DesktopPlaylistResolver>(
		_session.get(),
		std::move(batch),
		[weak, request, indices, warm, admission](auto results) {
			crl::on_main([weak, request, indices, warm, admission, results = std::move(results)]() mutable {
				if (const auto self = weak.lock()) {
					if (self->check(admission)) self->resolved(request, indices, warm, std::move(results));
				}
			});
		});
	_resolver->start();
}

void PlaylistPlayback::prefetch() {
	const auto index = _cursor.index();
	if (!index || _status.phase != PlaylistPlaybackPhase::Playing) return;
	trimCache();
	const auto window = ResolveIndices(
		_songs.size(),
		*index,
		_cursor.direction(),
		kResolveWindow,
		_cursor.repeatAll());
	for (auto offset = std::size_t(1); offset < window.size(); ++offset) {
		if (!_cache.contains(window[offset])) {
			requestResolution(window[offset], window.size() - offset, true);
			break;
		}
	}
}

void PlaylistPlayback::resolved(
		uint64_t request,
		std::vector<std::size_t> indices,
		bool warm,
		std::vector<PlaylistSongResolution> results) {
	if (request != _request) return;
	retireResolver();
	if (results.empty() || results.size() != indices.size() || results.size() > kResolveWindow) {
		if (!warm) publish(PlaylistPlaybackPhase::RetryRequired, PlaylistPlaybackProblem::TransientFailure);
		return;
	}
	for (auto offset = std::size_t(0); offset != results.size(); ++offset) {
		const auto index = indices[offset];
		if (index >= _songs.size() || results[offset].documentId != _songs[index].documentId) {
			_cache.clear();
			if (!warm) publish(PlaylistPlaybackPhase::RetryRequired, PlaylistPlaybackProblem::TransientFailure);
			return;
		}
		_cache.insert_or_assign(index, std::move(results[offset]));
	}
	if (!warm) consume();
}

void PlaylistPlayback::consume() {
	const auto index = _cursor.index();
	if (!index || !_cache.contains(*index)) {
		resolve();
		return;
	}
	const auto result = _cache.at(*index);
	switch (ActionForResolution(result.status)) {
	case ResolveAction::Play:
		play(result);
		break;
	case ResolveAction::Retry:
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			PlaylistPlaybackProblem::TransientFailure,
			result.status);
		break;
	case ResolveAction::Skip: {
		const auto weak = weak_from_this();
		const auto admission = _cursor.admission();
		publish(
			PlaylistPlaybackPhase::Resolving,
			PlaylistPlaybackProblem::SourceUnavailable,
			result.status);
		crl::on_main([weak, admission] {
			if (const auto self = weak.lock()) {
				if (!self->check(admission)) return;
				self->_cursor.skip();
				self->resolve();
			}
		});
	} break;
	}
}

void PlaylistPlayback::play(const PlaylistSongResolution &resolution) {
	const auto admission = _cursor.admission();
	if (!check(admission)) {
		return;
	}
	if (Handoffs->activeCall()) {
		const auto weak = weak_from_this();
		Handoffs->queue([weak, admission, resolution] {
			if (const auto self = weak.lock()) {
				if (self->check(admission)) self->play(resolution);
			}
		});
		ScheduleHandoffDispatch(Handoffs);
		return;
	}
	auto item = static_cast<HistoryItem*>(nullptr);
	const auto document = ResolvedDocument(_session.get(), resolution, item);
	if (!document) {
		_cache[*_cursor.index()].status = PlaylistSongStatus::Changed;
		consume();
		return;
	}
	const auto audio = AudioMsgId(document, item->fullId());
	const auto player = Media::Player::instance();
	const auto outgoing = player->current(Type::Song);
	const auto ownedOutgoing = _ownsCurrent;
	_transitionFrom = outgoing;
	_expected = audio;
	_ownsCurrent = true;
	_switching = true;
	const auto handoff = Handoffs->enter();
	const auto finish = gsl::finally([handoff] { Handoffs->leave(handoff); });
	_cancelHandoff = [handoff] { Handoffs->cancel(handoff); };
	const auto internal = PlaylistPlayerInternalCallGuard(
		Type::Song, PlaylistPlayerIntent::Play);
	player->play(audio);
	_cancelHandoff = nullptr;
	_switching = false;
	_transitionFrom = AudioMsgId();
	if (!Handoffs->current(handoff)) {
		if (!_retired && !admission() && bound()) {
			_expected = player->current(Type::Song);
			_ownsCurrent = SamePlaylistPlaybackSource(_expected, audio)
				|| (ownedOutgoing && SamePlaylistPlaybackSource(_expected, outgoing));
		} else if (!_retired) {
			retire(false);
		}
		return;
	}
	if (!check(admission)) return;
	if (!SamePlaylistPlaybackSource(player->current(Type::Song), audio)) {
		_expected = player->current(Type::Song);
		_ownsCurrent = false;
		publish(
			PlaylistPlaybackPhase::RetryRequired,
			PlaylistPlaybackProblem::PlaybackRejected);
	} else {
		_expiryResumePending = false;
		publish(
			PlaylistPlaybackPhase::Playing,
			PlaylistPlaybackProblem::None,
			resolution.status);
		if (!check(admission)) return;
		const auto weak = weak_from_this();
		crl::on_main([weak, admission] {
			if (const auto self = weak.lock()) {
				if (self->check(admission)) self->prefetch();
			}
		});
	}
}

bool PlaylistPlayback::owns(const AudioMsgId &current) {
	if (_retired) {
		return false;
	} else if (!bound()) {
		retire(true);
		return false;
	} else if (!matchesCurrent(current)) {
		retire(false);
		return false;
	}
	return true;
}

bool PlaylistPlayback::claimsShuffle(const AudioMsgId &current) {
	return _ownsCurrent && owns(current);
}

bool PlaylistPlayback::claimsUi(const AudioMsgId &current) const {
	return _ownsCurrent && bound() && matchesCurrent(current);
}

bool PlaylistPlayback::move(int delta, bool autonext) {
	if (!_loaded || delta == 0) {
		return false;
	} else if (autonext && (!_ownsCurrent || _status.phase != PlaylistPlaybackPhase::Playing)) {
		return false;
	}
	_pausedForExpiry = false;
	cancel(true);
	_automatic = autonext;
	_cursor.select(_cursor.position() + (delta > 0 ? 1 : -1), delta > 0 ? 1 : -1);
	if (_automatic) {
		_cursor.skipUnresolved([&](std::size_t index) { return _cache.contains(index); });
	}
	if (!_cursor.index()) {
		publish(PlaylistPlaybackPhase::Exhausted);
		return false;
	}
	const auto weak = weak_from_this();
	const auto admission = _cursor.admission();
	publish(PlaylistPlaybackPhase::Resolving);
	crl::on_main([weak, admission] {
		if (const auto self = weak.lock()) {
			if (self->check(admission)) self->resolve();
		}
	});
	return true;
}

bool PlaylistPlayback::available(int delta) const {
	return !_retired && _loaded && _cursor.stepAvailable(delta);
}

void PlaylistPlayback::retry() {
	const auto keepAlive = shared_from_this();
	if (!bound()) {
		observe();
		return;
	} else if (_status.phase != PlaylistPlaybackPhase::RetryRequired) {
		return;
	}
	_pausedForExpiry = false;
	cancel();
	if (!_loaded) load();
	else resolve();
}

PlaylistPlaybackStatus PlaylistPlayback::status() const {
	return _status;
}

PlaylistPlaybackStatus StatusFor(Main::Session *session) {
	return Active && Active->belongsTo(session)
		? Active->status()
		: PlaylistPlaybackStatus();
}

} // namespace

PlaylistPlayerInternalCallGuard::PlaylistPlayerInternalCallGuard(
		AudioMsgId::Type type,
		PlaylistPlayerIntent intent) {
	if (type != Type::Song) return;
	const auto calls = InternalCalls;
	const auto call = calls->enter(intent);
	_finish = [calls, call] { calls->leave(call); };
}

PlaylistPlayerInternalCallGuard::~PlaylistPlayerInternalCallGuard() {
	if (_finish) _finish();
}

bool AdmitPlaylistPlayerIntent(
		AudioMsgId::Type type,
		PlaylistPlayerIntent intent) {
	if (type == Type::Voice && !VoicePlaybackAvailable()) return false;
	if (type == Type::Voice && intent == PlaylistPlayerIntent::Stop) {
		StopVoicePlaybackContinuation();
	}
	if (type != Type::Song || InternalCalls->take(intent)) return true;
	if (PlayerFinished) return false;
	ClearVoiceMusicPause();
	const auto generation = ++StartGeneration;
	Handoffs->supersede();
	const auto active = Active;
	if (active) active->retire(false);
	return generation == StartGeneration && !PlayerFinished;
}

PlaylistPlayerCallGuard::PlaylistPlayerCallGuard(AudioMsgId::Type type) {
	if (type != AudioMsgId::Type::Song) {
		_current = (type == Type::Voice)
			? VoicePlaybackContinuationGuard()
			: Fn<bool()>([] { return true; });
		return;
	}
	const auto state = Handoffs;
	const auto frame = state->begin(true);
	_current = state->guard(frame);
	_finish = [state, frame] {
		state->finish(frame);
		ScheduleHandoffDispatch(state);
	};
}

PlaylistPlayerCallGuard::~PlaylistPlayerCallGuard() {
	if (_finish) _finish();
}

bool PlaylistPlayerCallGuard::current() const {
	return _current();
}

Fn<bool()> PlaylistPlayerCallbackGuard(
		AudioMsgId::Type type,
		bool acquire) {
	if (type == Type::Voice) return VoicePlaybackContinuationGuard();
	if (type != Type::Song) return [] { return true; };
	const auto state = Handoffs;
	if (state->activeCall()) return state->callback();
	if (!acquire) return [] { return true; };
	const auto frame = state->begin(true);
	const auto current = state->guard(frame);
	const auto lease = std::shared_ptr<int>(new int(0), [state, frame](int *value) {
		delete value;
		state->finish(frame);
		ScheduleHandoffDispatch(state);
	});
	return [current, lease] { return lease && current(); };
}

bool DeferPlaylistResumeAfterSleepTimer(
		AudioMsgId::Type type,
		Fn<void()> resume,
		bool play) {
	if (type == Type::Voice && play) NoteVoicePlaybackInteraction();
	if (type != Type::Song || InternalCalls->take(PlaylistPlayerIntent::Resume)) return false;
	const auto active = Active;
	return play && active && active->deferExpiryResume(std::move(resume));
}

Fn<bool()> PlaylistPlayerPauseGuard(
		AudioMsgId::Type type,
		bool pause,
		bool acquire) {
	if (type == Type::Voice && pause) NoteVoicePlaybackInteraction();
	if (pause && type == Type::Song && !InternalCalls->take(PlaylistPlayerIntent::Pause)) {
		const auto active = Active;
		if (active && active->cancelExpiryResume()) {
			Handoffs->supersede();
			const auto lease = std::make_shared<PlaylistPlayerCallGuard>(type);
			Changes.fire({});
			return [lease] { return lease->current(); };
		}
	}
	return PlaylistPlayerCallbackGuard(type, acquire);
}

Fn<bool()> NewPlaylistPlayerStreamGuard(AudioMsgId::Type type) {
	if (type == Type::Voice) return NewVoicePlaybackStreamGuard();
	if (type != Type::Song) return [] { return true; };
	SongStream = std::make_shared<int>(0);
	const auto weak = std::weak_ptr<int>(SongStream);
	return [weak] { return !weak.expired(); };
}

void RetirePlaylistPlayerStream(AudioMsgId::Type type) {
	if (type == Type::Song) SongStream.reset();
	else if (type == Type::Voice) RetireVoicePlaybackStream();
}

bool SkipAccountPlaylistShuffle(
		AudioMsgId::Type type,
		const AudioMsgId &current,
		Fn<void()> clear) {
	const auto active = Active;
	if (type != Type::Song || !active || !active->claimsShuffle(current)) return false;
	clear();
	return true;
}

bool DeferPlaylistPlayerPlay(const AudioMsgId &audioId, Fn<void()> resume) {
	if (audioId.type() != Type::Song || !Handoffs->activeCall()) return false;
	const auto document = audioId.audio();
	if (!document || PlayerFinished) return true;
	const auto session = &document->session();
	const auto weak = base::make_weak(session);
	const auto account = SessionAccountKey(session);
	const auto generation = StartGeneration;
	Handoffs->queue([weak, account, generation, resume = std::move(resume)] {
		const auto session = weak.get();
		if (!PlayerFinished && generation == StartGeneration
			&& session && !session->account().loggingOut()
			&& session->account().maybeSession() == session
			&& SessionAccountKey(session) == account) {
			const auto internal = PlaylistPlayerInternalCallGuard(
				Type::Song, PlaylistPlayerIntent::Play);
			resume();
		}
	});
	ScheduleHandoffDispatch(Handoffs);
	return true;
}

std::optional<PlaylistPlayerUiTarget> CurrentAccountPlaylistTarget(
		not_null<Main::Session*> session,
		const AudioMsgId &current) {
	RequireMainThread();
	const auto active = Active;
	if (current.type() != Type::Song || !current.audio()
		|| !active || !active->belongsTo(session)
		|| !active->claimsUi(current) || Active != active) {
		return std::nullopt;
	}
	const auto status = active->status();
	if (status.playlistId.isEmpty()) return std::nullopt;
	return PlaylistPlayerUiTarget{ status.playlistId, current.audio()->id };
}

void PlayAccountPlaylist(
		not_null<Main::Session*> session,
		const QString &playlistId,
		std::optional<uint64_t> documentId) {
	RequireMainThread();
	if (PlayerFinished) return;
	const auto generation = ++StartGeneration;
	Handoffs->supersede();
	const auto previous = Active;
	if (previous) previous->retire(false);
	if (generation != StartGeneration || PlayerFinished) return;
	const auto next = std::make_shared<PlaylistPlayback>(session, playlistId, documentId);
	Active = next;
	next->start();
}

void RetryAccountPlaylistPlayback(not_null<Main::Session*> session) {
	RequireMainThread();
	const auto active = Active;
	if (active && active->belongsTo(session)) active->retry();
}

void StopAccountPlaylistPlayback(not_null<Main::Session*> session) {
	RequireMainThread();
	const auto active = Active;
	if (active && active->belongsTo(session)) {
		++StartGeneration;
		active->retire(true);
	}
}

bool ResumeAccountPlaylistAfterSleepTimer(
		AudioMsgId::Type type,
		const AudioMsgId &current,
		bool restart) {
	RequireMainThread();
	const auto active = Active;
	return type == Type::Song && active && active->resumeAfterExpiry(current, restart);
}

void CancelPlaylistStartsForSleepTimer() {
	RequireMainThread();
	++StartGeneration;
	Handoffs->supersede();
	const auto active = Active;
	if (active) active->pauseForExpiry();
}

void InvalidatePlaylistPlayback() {
	RequireMainThread();
	++StartGeneration;
	const auto active = Active;
	if (active) active->retire(true);
}

void FinishPlaylistPlayback() {
	RequireMainThread();
	PlayerFinished = true;
	FinishVoiceQueue();
	SongStream.reset();
	Handoffs->supersede();
	++StartGeneration;
	const auto active = Active;
	if (active) active->retire(true, true);
	Active.reset();
}

rpl::producer<PlaylistPlaybackStatus> AccountPlaylistPlaybackValue(
		not_null<Main::Session*> session) {
	RequireMainThread();
	const auto weak = base::make_weak(session.get());
	const auto account = SessionAccountKey(session);
	return rpl::single(rpl::empty) | rpl::then(Changes.events()) | rpl::map([weak, account] {
		const auto session = weak.get();
		return session && SessionAccountKey(session) == account
			? StatusFor(session)
			: PlaylistPlaybackStatus{ .phase = PlaylistPlaybackPhase::Retired };
	});
}

std::optional<bool> MoveInAccountPlaylist(
		AudioMsgId::Type type,
		const AudioMsgId &current,
		int delta,
		bool autonext) {
	RequireMainThread();
	if (type == Type::Song && delta > 0 && current.audio()
		&& PlayQueuedMusic(&current.audio()->session())) {
		return true;
	}
	const auto active = Active;
	if (type != Type::Song || !active || !active->owns(current)) return std::nullopt;
	return active->move(delta, autonext);
}

std::optional<bool> AccountPlaylistStepAvailable(
		AudioMsgId::Type type,
		const AudioMsgId &current,
		int delta) {
	RequireMainThread();
	if (type == Type::Song && delta > 0 && current.audio()) {
		const auto found = MusicQueues.find(&current.audio()->session());
		if (found != MusicQueues.end() && !found->second.empty()) return true;
	}
	const auto active = Active;
	if (type != Type::Song || !active || !active->owns(current)) return std::nullopt;
	return active->available(delta);
}

bool AppendAccountMusicQueue(
		not_null<Main::Session*> session,
		const std::vector<PlaylistMessage> &messages) {
	RequireMainThread();
	if (messages.empty() || Core::App().passcodeLocked()
		|| session->account().loggingOut()
		|| !std::ranges::all_of(messages, [=](const PlaylistMessage &message) {
			return bool(CapturePlaylistSong(session, message));
		})) {
		return false;
	}
	const auto [found, created] = MusicQueues.try_emplace(session.get());
	if (created) {
		session->lifetime().add([key = session.get()] { MusicQueues.erase(key); });
	}
	found->second.insert(found->second.end(), messages.begin(), messages.end());
	const auto state = Media::Player::instance()->getState(Type::Song);
	if (Media::Player::IsStoppedOrStopping(state.state)) {
		return PlayQueuedMusic(session);
	}
	Changes.fire({});
	return true;
}

} // namespace Purple
