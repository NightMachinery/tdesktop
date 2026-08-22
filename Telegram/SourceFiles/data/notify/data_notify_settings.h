/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/notify/data_peer_notify_settings.h"

#include "base/timer.h"

class PeerData;
class ChannelData;

namespace Data {

class DocumentMedia;
class Session;
class Thread;
class Forum;
class ForumTopic;

[[nodiscard]] DefaultNotify DefaultNotifyType(
	not_null<const PeerData*> peer);

[[nodiscard]] MTPInputNotifyPeer DefaultNotifyToMTP(DefaultNotify type);

class NotifySettings final {
public:
	NotifySettings(not_null<Session*> owner);

	void request(not_null<PeerData*> peer);
	void request(not_null<Thread*> thread);

	void apply(
		const MTPNotifyPeer &notifyPeer,
		const MTPPeerNotifySettings &settings);
	void apply(
		const MTPInputNotifyPeer &notifyPeer,
		const MTPPeerNotifySettings &settings);
	void apply(DefaultNotify type, const MTPPeerNotifySettings &settings);
	void apply(PeerId peerId, const MTPPeerNotifySettings &settings);
	void apply(
		not_null<PeerData*> peer,
		const MTPPeerNotifySettings &settings);
	void apply(
		PeerId peerId,
		MsgId topicRootId,
		const MTPPeerNotifySettings &settings);
	void apply(
		not_null<ForumTopic*> topic,
		const MTPPeerNotifySettings &settings);

	void update(
		not_null<Thread*> thread,
		MuteValue muteForSeconds,
		std::optional<bool> silentPosts = std::nullopt,
		std::optional<NotifySound> sound = std::nullopt,
		std::optional<bool> storiesMuted = std::nullopt);
	void resetToDefault(not_null<Thread*> thread);
	void update(
		not_null<PeerData*> peer,
		MuteValue muteForSeconds,
		std::optional<bool> silentPosts = std::nullopt,
		std::optional<NotifySound> sound = std::nullopt,
		std::optional<bool> storiesMuted = std::nullopt);
	void resetToDefault(not_null<PeerData*> peer);

	void forumParentMuteUpdated(not_null<Forum*> forum);
	void communityParentMuteUpdated(not_null<ChannelData*> community);

	void cacheSound(DocumentId id);
	void cacheSound(not_null<DocumentData*> document);
	[[nodiscard]] std::shared_ptr<DocumentMedia> lookupRingtone(
		DocumentId id) const;

	[[nodiscard]] rpl::producer<> defaultUpdates(DefaultNotify type) const;
	[[nodiscard]] rpl::producer<> defaultUpdates(
		not_null<const PeerData*> peer) const;

	[[nodiscard]] const PeerNotifySettings &defaultSettings(
		DefaultNotify type) const;
	[[nodiscard]] bool isMuted(DefaultNotify type) const;

	void defaultUpdate(
		DefaultNotify type,
		MuteValue muteForSeconds,
		std::optional<bool> silentPosts = std::nullopt,
		std::optional<NotifySound> sound = std::nullopt,
		std::optional<bool> storiesMuted = std::nullopt);

	[[nodiscard]] bool isMuted(not_null<const Thread*> thread) const;
	[[nodiscard]] NotifySound sound(not_null<const Thread*> thread) const;
	[[nodiscard]] bool muteUnknown(not_null<const Thread*> thread) const;
	[[nodiscard]] bool soundUnknown(not_null<const Thread*> thread) const;

	[[nodiscard]] bool isMuted(not_null<const PeerData*> peer) const;
	[[nodiscard]] bool silentPosts(not_null<const PeerData*> peer) const;
	[[nodiscard]] NotifySound sound(not_null<const PeerData*> peer) const;
	[[nodiscard]] bool muteUnknown(not_null<const PeerData*> peer) const;
	[[nodiscard]] bool silentPostsUnknown(
		not_null<const PeerData*> peer) const;
	[[nodiscard]] bool soundUnknown(not_null<const PeerData*> peer) const;

	// Purple: re-evaluates the cached mute after a work preset moved this chat
	// between lists. Nothing about the peer itself changed, so none of the
	// upstream update paths reach it. See docs/purple/work_mode.md.
	void purpleRefreshMute(not_null<PeerData*> peer);

	// Purple: what isMuted() would say if no work preset were imposing one.
	// The UI needs both answers - one to know the chat is silent, the other to
	// know whether offering Unmute would lift anything - and this has to be
	// the real accessor rather than a peek at muteUntil, because a chat can be
	// muted by the account-wide default for its type with nothing set on it.
	[[nodiscard]] bool purpleMutedWithoutPreset(
		not_null<const Thread*> thread) const;
	[[nodiscard]] bool purpleMutedWithoutPreset(
		not_null<const PeerData*> peer) const;

	// Purple: whether the active preset silences this chat by naming a folder
	// it is in, rather than by naming a list. False the moment no preset asks,
	// which is what keeps a folder walk out of every other mute query.
	[[nodiscard]] bool purpleSilencedByFolder(
		not_null<const PeerData*> peer) const;

	void loadExceptions();
	[[nodiscard]] rpl::producer<DefaultNotify> exceptionsUpdates() const;
	[[nodiscard]] auto exceptionsUpdatesRealtime() const
		-> rpl::producer<DefaultNotify>;
	[[nodiscard]] const base::flat_set<not_null<PeerData*>> &exceptions(
		DefaultNotify type) const;
	void clearExceptions(DefaultNotify type);

private:
	static constexpr auto kDefaultNotifyTypes = 3;

	struct DefaultValue {
		PeerNotifySettings settings;
		rpl::event_stream<> updates;
	};

	void cacheSound(const std::optional<NotifySound> &sound);

	[[nodiscard]] bool isMuted(
		not_null<const Thread*> thread,
		crl::time *changesIn) const;
	[[nodiscard]] bool isMuted(
		not_null<const PeerData*> peer,
		crl::time *changesIn) const;

	// The body of isMuted() below the Purple gate, split out so both can be
	// asked separately. See purpleMutedWithoutPreset above.
	[[nodiscard]] bool purpleMutedWithoutPreset(
		not_null<const PeerData*> peer,
		crl::time *changesIn) const;

	[[nodiscard]] DefaultValue &defaultValue(DefaultNotify type);
	[[nodiscard]] const DefaultValue &defaultValue(DefaultNotify type) const;
	[[nodiscard]] const PeerNotifySettings &defaultSettings(
		not_null<const PeerData*> peer) const;
	[[nodiscard]] bool settingsUnknown(not_null<const PeerData*> peer) const;
	[[nodiscard]] bool settingsUnknown(
		not_null<const Thread*> thread) const;

	void unmuteByFinished();
	void unmuteByFinishedDelayed(crl::time delay);
	void updateLocal(not_null<Thread*> thread);
	void updateLocal(not_null<PeerData*> peer);
	void updateLocal(DefaultNotify type);

	void updateException(not_null<PeerData*> peer);
	void exceptionsUpdated(DefaultNotify type);

	const not_null<Session*> _owner;

	DefaultValue _defaultValues[3];
	std::unordered_set<not_null<const PeerData*>> _mutedPeers;
	std::unordered_map<not_null<ForumTopic*>, rpl::lifetime> _mutedTopics;
	base::Timer _unmuteByFinishedTimer;

	struct {
		base::flat_map<
			DocumentId,
			std::shared_ptr<DocumentMedia>> views;
		std::vector<DocumentId> pendingIds;
		rpl::lifetime pendingLifetime;
	} _ringtones;

	rpl::event_stream<DefaultNotify> _exceptionsUpdates;
	rpl::event_stream<DefaultNotify> _exceptionsUpdatesRealtime;
	std::array<
		base::flat_set<not_null<PeerData*>>,
		kDefaultNotifyTypes> _exceptions;
	std::array<mtpRequestId, kDefaultNotifyTypes> _exceptionsRequestId = {};
	std::array<bool, kDefaultNotifyTypes> _exceptionsUpdatesScheduled = {};

};

} // namespace Data
