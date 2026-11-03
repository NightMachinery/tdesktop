#include "purple/purple_sync_magic_telegram.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_config_file.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <limits>

namespace Purple {
namespace {

constexpr auto kRequestTimeoutMs = 30 * 1000;
constexpr auto kListsTimeoutMs = int64_t(60 * 1000);
constexpr auto kAbout = "Purple Telegram keeps sync data for your devices here.";

int SlotOf(not_null<Main::Session*> session) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account.get() == &session->account()) {
			return index;
		}
	}
	return -1;
}

// Joined supergroups from the session, not from a chat list a Work Mode preset may have filtered.
void AddJoined(not_null<Data::Session*> data, std::vector<MagicChatCandidate> &result) {
	data->enumerateGroups([&](not_null<PeerData*> peer) {
		const auto channel = peer->asChannel();
		if (channel && channel->amIn() && !channel->isForbidden() && IsMagicSyncChatTitle(channel->name())) {
			result.push_back(SyncMagicLocalCandidate(channel));
		}
	});
}

class TelegramPort final
	: public SyncMagicPort
	, public std::enable_shared_from_this<TelegramPort> {
public:
	TelegramPort(
		not_null<Main::Session*> session,
		QString path,
		Fn<bool()> current)
	: _session(session)
	, _path(std::move(path))
	, _current(std::move(current))
	, _slot(SlotOf(session)) {
	}

	bool current() override {
		return _session && _current && _current();
	}
	int64_t now() override {
		return base::unixtime::now();
	}
	void post(std::function<void()> task) override {
		crl::on_main(std::move(task));
	}
	void delay(std::function<void()> task, int64_t ms) override {
		QTimer::singleShot(
			int(std::clamp(ms, int64_t(0), int64_t(std::numeric_limits<int>::max()))),
			std::move(task));
	}
	std::optional<QByteArray> readBinding() override {
		if (!current()) {
			return std::nullopt;
		}
		const auto input = ReadSettingsFileInput(_path);
		if (input.state == SettingsFileState::Missing) {
			return QByteArray();
		} else if (input.state != SettingsFileState::Readable
			|| input.bytes.size() > kSyncMagicBindingLimit) {
			return std::nullopt;
		}
		return input.bytes;
	}
	bool writeBinding(const QByteArray &expected, const QByteArray &next) override {
		if (!current() || next.size() > kSyncMagicBindingLimit) {
			return false;
		}
		const auto input = ReadSettingsFileInput(_path);
		const auto stored = (input.state == SettingsFileState::Readable)
			? std::optional(input.bytes)
			: (input.state == SettingsFileState::Missing)
			? std::optional(QByteArray())
			: std::nullopt;
		if (stored != expected || !QDir().mkpath(QFileInfo(_path).absolutePath())) {
			return false;
		}
		const auto weak = weak_from_this();
		return WriteGuardedSettingsFile(_path, input, next, [=] {
			const auto self = weak.lock();
			return self && self->current();
		}, true) == GuardedFileWrite::Written;
	}
	void local(
			std::optional<PeerToken> bound,
			std::function<void(std::optional<SyncMagicLocal>)> done) override {
		const auto session = _session.get();
		if (!session || !current()) {
			done(std::nullopt);
			return;
		}
		auto &data = session->data();
		auto result = SyncMagicLocal();
		AddJoined(&data, result.visible);
		const auto archive = data.folderLoaded(Data::Folder::kId);
		result.mainLoaded = data.chatsListLoaded(nullptr);
		result.archiveLoaded = archive ? data.chatsListLoaded(archive) : result.mainLoaded;
		if (bound && bound->kind == PeerTokenKind::Self) {
			result.boundState = SyncChatBoundState::Present;
			result.bound.facts.kind = SyncChatKind::Self;
			result.bound.facts.canSendFiles = true;
			result.bound.facts.otherMembers = 0;
			result.bound.facts.otherAdmins = 0;
		} else if (bound && bound->kind == PeerTokenKind::Channel) {
			if (const auto channel = data.channelLoaded(ChannelId(bound->id))) {
				if (channel->isForbidden()) {
					result.boundState = SyncChatBoundState::Gone;
				} else {
					result.boundState = SyncChatBoundState::Present;
					result.bound = SyncMagicLocalCandidate(channel);
				}
			}
		}
		done(std::move(result));
	}
	void loadLists(std::optional<PeerToken> bound, std::function<void()> done) override {
		const auto session = _session.get();
		if (!session || !current()) {
			done();
			return;
		}
		const auto finished = std::make_shared<bool>(false);
		const auto weak = base::make_weak(session);
		const auto check = [=] {
			const auto session = weak.get();
			if (*finished) {
				return;
			} else if (!session) {
				*finished = true;
				done();
				return;
			}
			auto &data = session->data();
			const auto archive = data.folderLoaded(Data::Folder::kId);
			const auto step = SyncMagicListsStepOf(
				bound && data.channelLoaded(ChannelId(bound->id)) != nullptr,
				data.chatsListLoaded(nullptr),
				!archive || data.chatsListLoaded(archive));
			if (step == SyncMagicListsStep::MoreArchive) {
				session->api().requestDialogs(archive);
				return;
			} else if (step == SyncMagicListsStep::Wait) {
				return;
			}
			*finished = true;
			done();
		};
		_lists.destroy();
		rpl::merge(
			session->data().chatsListChanges(),
			session->data().chatsListLoadedEvents()
		) | rpl::on_next([=](Data::Folder*) {
			check();
		}, _lists);
		delay([=] {
			if (!*finished) {
				*finished = true;
				done();
			}
		}, kListsTimeoutMs);
		check();
	}
	void fullInfo(
			uint64_t channelId,
			std::function<void(SyncMagicReply<MagicChatCandidate>)> done) override {
		using Reply = SyncMagicReply<MagicChatCandidate>;
		const auto session = _session.get();
		const auto channel = session
			? session->data().channelLoaded(ChannelId(channelId))
			: nullptr;
		if (!channel) {
			done(Reply::Failed(u"CHANNEL_INVALID"_q, 400));
			return;
		}
		const auto weak = base::make_weak(session);
		send<MTPchannels_GetFullChannel, MagicChatCandidate>(
			MTPchannels_GetFullChannel(channel->inputChannel()),
			std::move(done),
			[=](const MTPmessages_ChatFull &result) {
				const auto session = weak.get();
				const auto channel = session
					? session->data().channelLoaded(ChannelId(channelId))
					: nullptr;
				if (!channel) {
					return Reply::Failed(QString(), -1);
				}
				session->api().processFullPeer(channel, result);
				const auto &data = result.data();
				for (const auto &chat : data.vchats().v) {
					const auto candidate = SyncMagicCandidate(chat, &data.vfull_chat());
					if (candidate && candidate->channelId == channelId) {
						return Reply::Ok(*candidate);
					}
				}
				return Reply::Failed(u"CHAT_FULL_INVALID"_q, 400);
			});
	}
	void create(std::function<void(SyncMagicReply<MagicChatCandidate>)> done) override {
		using Reply = SyncMagicReply<MagicChatCandidate>;
		using Flag = MTPchannels_CreateChannel::Flag;
		const auto weak = base::make_weak(_session.get());
		send<MTPchannels_CreateChannel, MagicChatCandidate>(
			MTPchannels_CreateChannel(
				MTP_flags(Flag::f_megagroup),
				MTP_string(QString::fromLatin1(kMagicSyncChatTitle)),
				MTP_string(QString::fromLatin1(kAbout)),
				MTPInputGeoPoint(),
				MTPstring(),
				MTPint()),
			std::move(done),
			[=](const MTPUpdates &result) {
				const auto session = weak.get();
				if (!session) {
					return Reply::Failed(QString(), -1);
				}
				session->api().applyUpdates(result);
				const auto chats = result.match([](const MTPDupdates &data) {
					return data.vchats().v;
				}, [](const MTPDupdatesCombined &data) {
					return data.vchats().v;
				}, [](const auto &) {
					return QVector<MTPChat>();
				});
				for (const auto &chat : chats) {
					const auto candidate = SyncMagicCandidate(chat, nullptr);
					if (candidate
						&& candidate->megagroup
						&& candidate->facts.creator
						&& IsMagicSyncChatTitle(candidate->title)) {
						return Reply::Ok(*candidate);
					}
				}
				return Reply::Ok(std::nullopt);
			});
	}
	void archive(uint64_t channelId, std::function<void(SyncMagicReply<bool>)> done) override {
		using Reply = SyncMagicReply<bool>;
		const auto session = _session.get();
		const auto channel = session
			? session->data().channelLoaded(ChannelId(channelId))
			: nullptr;
		if (!channel) {
			done(Reply::Failed(u"PEER_ID_INVALID"_q, 400));
			return;
		}
		const auto weak = base::make_weak(session);
		send<MTPfolders_EditPeerFolders, bool>(
			MTPfolders_EditPeerFolders(MTP_vector<MTPInputFolderPeer>(1,
				MTP_inputFolderPeer(channel->input(), MTP_int(Data::Folder::kId)))),
			std::move(done),
			[=](const MTPUpdates &result) {
				if (const auto session = weak.get()) {
					session->api().applyUpdates(result);
				}
				return Reply::Ok(true);
			});
	}
	void mute(uint64_t channelId, std::function<void(SyncMagicReply<bool>)> done) override {
		using Reply = SyncMagicReply<bool>;
		using Flag = MTPDinputPeerNotifySettings::Flag;
		const auto session = _session.get();
		const auto channel = session
			? session->data().channelLoaded(ChannelId(channelId))
			: nullptr;
		if (!channel) {
			done(Reply::Failed(u"PEER_ID_INVALID"_q, 400));
			return;
		}
		const auto forever = std::numeric_limits<int32_t>::max();
		const auto weak = base::make_weak(session);
		send<MTPaccount_UpdateNotifySettings, bool>(
			MTPaccount_UpdateNotifySettings(
				MTP_inputNotifyPeer(channel->input()),
				MTP_inputPeerNotifySettings(
					MTP_flags(Flag::f_mute_until),
					MTPBool(),
					MTPBool(),
					MTP_int(forever),
					MTPNotificationSound(),
					MTPBool(),
					MTPBool(),
					MTPNotificationSound())),
			std::move(done),
			[=](const MTPBool &result) {
				const auto session = weak.get();
				const auto channel = session
					? session->data().channelLoaded(ChannelId(channelId))
					: nullptr;
				if (!mtpIsTrue(result)) {
					return Reply::Ok(false);
				} else if (channel) {
					session->data().notifySettings().apply(
						channel,
						MTP_peerNotifySettings(
							MTP_flags(MTPDpeerNotifySettings::Flag::f_mute_until),
							MTPBool(),
							MTPBool(),
							MTP_int(forever),
							MTPNotificationSound(),
							MTPNotificationSound(),
							MTPNotificationSound(),
							MTPBool(),
							MTPBool(),
							MTPNotificationSound(),
							MTPNotificationSound(),
							MTPNotificationSound()));
				}
				return Reply::Ok(true);
			});
	}
	void diag(const QString &event, const std::vector<SyncDiagField> &fields) override {
		auto all = std::vector<SyncDiagField>{ SyncDiagCount(u"slot"_q, _slot) };
		all.insert(end(all), begin(fields), end(fields));
		SyncDiag(u"sync_chat"_q, event, all);
	}

private:
	template <typename Request, typename Result>
	void send(
			Request &&request,
			std::function<void(SyncMagicReply<Result>)> done,
			std::function<SyncMagicReply<Result>(const typename Request::ResponseType &)> map) {
		const auto session = _session.get();
		if (!session || !current()) {
			done(SyncMagicReply<Result>::Failed(QString(), -1));
			return;
		}
		const auto answered = std::make_shared<bool>(false);
		const auto id = std::make_shared<mtpRequestId>(0);
		const auto weak = weak_from_this();
		const auto finish = [=](SyncMagicReply<Result> reply) {
			if (!std::exchange(*answered, true)) {
				done(std::move(reply));
			}
		};
		*id = session->api().request(std::forward<Request>(request)).done([=](
				const typename Request::ResponseType &result) {
			const auto self = weak.lock();
			finish((self && self->current())
				? map(result)
				: SyncMagicReply<Result>::Failed(QString(), -1));
		}).fail([=](const MTP::Error &error) {
			const auto type = error.type();
			const auto code = error.code();
			finish(SyncMagicReply<Result>::Failed(
				type,
				(code == 0 && type.isEmpty()) ? -1 : code));
		}).handleFloodErrors().send();
		const auto session_ = base::make_weak(session);
		QTimer::singleShot(kRequestTimeoutMs, [=] {
			if (*answered) {
				return;
			} else if (const auto session = session_.get()) {
				session->api().request(*id).cancel();
			}
			finish(SyncMagicReply<Result>::TimedOut());
		});
	}

	base::weak_ptr<Main::Session> _session;
	QString _path;
	Fn<bool()> _current;
	int _slot = -1;
	rpl::lifetime _lists;

};

} // namespace

MagicChatCandidate SyncMagicLocalCandidate(not_null<ChannelData*> channel) {
	auto result = MagicChatCandidate();
	result.channelId = peerToChannel(channel->id).bare;
	result.title = channel->name();
	result.member = channel->amIn();
	result.megagroup = channel->isMegagroup();
	result.gigagroup = channel->isGigagroup();
	auto &facts = result.facts;
	facts.kind = channel->isMonoforum()
		? SyncChatKind::ChannelDirectMessages
		: result.megagroup
		? SyncChatKind::Supergroup
		: SyncChatKind::Broadcast;
	facts.creator = channel->amCreator();
	facts.admin = channel->amCreator() || channel->hasAdminRights();
	facts.publicUsername = !channel->username().isEmpty()
		|| !channel->usernames().empty();
	facts.protectedContent = !channel->allowsForwarding();
	facts.forum = channel->isForum();
	facts.slowMode = (channel->flags() & ChannelDataFlag::SlowmodeEnabled)
		|| channel->slowmodeSeconds() > 0;
	facts.anonymousAdmin = (channel->adminRights() & ChatAdminRight::Anonymous);
	facts.paidMessages = (channel->starsPerMessage() != 0);
	facts.canSendFiles = result.member && channel->amCreator();
	return result;
}

std::optional<MagicChatCandidate> SyncMagicCandidate(
		const MTPChat &chat,
		const MTPChatFull *full) {
	auto result = MagicChatCandidate();
	const auto mapped = chat.match([&](const MTPDchannel &data) {
		if (data.is_min() || data.vid().v <= 0) {
			return false;
		}
		result.channelId = uint64_t(data.vid().v);
		result.title = qs(data.vtitle());
		result.member = !data.is_left();
		result.megagroup = data.is_megagroup();
		result.gigagroup = data.is_gigagroup();
		auto &facts = result.facts;
		facts.kind = data.is_monoforum()
			? SyncChatKind::ChannelDirectMessages
			: data.is_megagroup()
			? SyncChatKind::Supergroup
			: SyncChatKind::Broadcast;
		facts.creator = data.is_creator();
		const auto rights = data.vadmin_rights();
		facts.admin = data.is_creator() || rights;
		facts.publicUsername = data.vusername() && !qs(*data.vusername()).isEmpty();
		if (const auto usernames = data.vusernames()) {
			for (const auto &username : usernames->v) {
				facts.publicUsername = facts.publicUsername || username.data().is_active();
			}
		}
		facts.protectedContent = data.is_noforwards();
		facts.forum = data.is_forum();
		facts.slowMode = data.is_slowmode_enabled();
		facts.anonymousAdmin = rights
			&& (rights->data().vflags().v & MTPDchatAdminRights::Flag::f_anonymous);
		facts.paidMessages = data.vsend_paid_messages_stars().value_or_empty() != 0;
		facts.canSendFiles = result.member && data.is_creator();
		return true;
	}, [&](const MTPDchannelForbidden &data) {
		result.channelId = uint64_t(data.vid().v);
		result.title = qs(data.vtitle());
		result.member = false;
		return data.vid().v > 0;
	}, [](const auto &) {
		return false;
	});
	if (!mapped) {
		return std::nullopt;
	} else if (!full || chat.type() != mtpc_channel) {
		return result;
	}
	const auto channelFull = full->match([&](const MTPDchannelFull &data) {
		if (uint64_t(data.vid().v) != result.channelId) {
			return false;
		}
		auto &facts = result.facts;
		facts.linkedDiscussion = data.vlinked_chat_id().has_value();
		facts.autoDelete = data.vttl_period().value_or_empty() != 0;
		facts.paidMessages = facts.paidMessages
			|| data.vsend_paid_messages_stars().value_or_empty() != 0;
		facts.slowMode = facts.slowMode
			|| data.vslowmode_seconds().value_or_empty() != 0;
		if (const auto sendAs = data.vdefault_send_as()) {
			facts.postsAsOther = (sendAs->type() != mtpc_peerUser);
		}
		const auto admins = data.vadmins_count().value_or_empty();
		if (admins > 0) {
			facts.soleAdmin = (admins == 1);
			facts.otherAdmins = uint64_t(admins - (facts.admin ? 1 : 0));
		}
		const auto members = data.vparticipants_count().value_or_empty();
		if (result.member && members > 0) {
			facts.otherMembers = uint64_t(members - 1);
		}
		return true;
	}, [](const auto &) {
		return false;
	});
	return channelFull ? std::optional(result) : std::nullopt;
}

std::shared_ptr<SyncMagicPort> MakeSyncMagicTelegramPort(
		not_null<Main::Session*> session,
		QString bindingPath,
		Fn<bool()> current) {
	return std::make_shared<TelegramPort>(
		session,
		std::move(bindingPath),
		std::move(current));
}

} // namespace Purple
