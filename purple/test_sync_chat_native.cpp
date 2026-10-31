#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "purple/purple_sync_chat_native.h"
#include "test_sync_chat_carry.cpp"
#include <deque>
#include "base/unixtime.h"

namespace {

class NativeWorker final {
public:
	NativeWorker();
	~NativeWorker();
	template <typename F> auto call(F task);

private:
	void loop();
	std::mutex _mutex;
	std::condition_variable _ready;
	std::deque<Fn<void()>> _queue;
	bool _stop = false;
	std::thread _thread;

};

NativeWorker::NativeWorker()
: _thread([this] { loop(); }) {}

NativeWorker::~NativeWorker() {
	{
		const auto lock = std::lock_guard(_mutex);
		_stop = true;
	}
	_ready.notify_all();
	_thread.join();
}

void NativeWorker::loop() {
	while (true) {
		auto task = Fn<void()>();
		{
			auto lock = std::unique_lock(_mutex);
			_ready.wait(lock, [&] { return _stop || !_queue.empty(); });
			if (_queue.empty() && _stop) {
				return;
			}
			task = std::move(_queue.front());
			_queue.pop_front();
		}
		task();
	}
}

template <typename F> auto NativeWorker::call(F task) {
	using Result = decltype(task());
	const auto work
	    = std::make_shared<std::packaged_task<Result()>>(std::move(task));
	auto result = work->get_future();
	{
		const auto lock = std::lock_guard(_mutex);
		_queue.push_back([work] { (*work)(); });
	}
	_ready.notify_one();
	return result.get();
}

std::unique_ptr<NativeWorker> TestWorker;

template <typename F> auto Worker(F task) {
	return TestWorker->call(std::move(task));
}

MTPMessage NativeMark(const DesktopSyncChatIo &route, int id, int date) {
	auto message = NativeMessage();
	message.flags
	    = MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_out);
	message.id = MTP_int(id);
	message.date = MTP_int(date);
	message.peer_id = Target(route.peer.chat);
	message.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
	message.message = MTP_string(DesktopSyncChatMarkText());
	return message.Build();
}

struct NativeHarness {
	std::unique_ptr<ExecutorFiles> files;
	DesktopSyncChatSetupSnapshot opening;
	DesktopSyncNativeIo io;
	DesktopSyncNativeRenewal renewal;
	std::unique_ptr<DesktopSyncChatNative> adapter;
	bool live = true;
	bool partial = false;
	bool wrongPeer = false;
	bool unknown = false;
	bool hold = false;
	bool holdEffect = false;
	bool duplicate = false;
	bool removeOld = false;
	bool malformedExact = false;
	bool hideTargetFromGlobal = false;
	bool missingTargetMark = false;
	int peerMark = 0;
	int targetSearches = 0;
	int targetHistories = 0;
	std::vector<int> exactIds;
	int markDeletes = 0;
	int cleanupDeletes = 0;
	std::set<int> deleted;
	std::vector<SyncCarryJournalAction> actions;
	int requests = 0;
	int effects = 0;
	int applies = 0;
	int uploads = 0;
	int renewals = 0;
	int cancels = 0;
	Fn<void(DesktopSyncNativeReply)> pending;
	NativeHarness(bool own = false, bool pendingStage = false) {
		files = Worker([=] {
			return std::make_unique<ExecutorFiles>(own,
			    SyncCarryMode::Switch,
			    pendingStage);
		});
		files->fresh.source.peer.facts->protectedContent = own;
		files->fresh.resolutionInput.cache
		    = SyncChatCache{files->initial.chat, 9, 100};
		files->start.resolution
		    = ResolveSyncChat(files->fresh.resolutionInput);
		opening.scope.origin = Origin;
		opening.scope.loginBinding = files->scope.loginAuthority;
		opening.scope.binding = files->scope.binding;
		opening.scope.root = files->temporary.path();
		opening.scope.settingsBound = true;
		opening.scope.settingsChat = files->initial.chat;
		opening.scope.space = files->initial.space;
		opening.scope.mark = files->fresh.resolutionInput.cache;
		auto cache = DesktopSyncChatCacheRecord();
		cache.account = Origin.account;
		cache.loginBinding = files->scope.loginAuthority;
		cache.mark = *opening.scope.mark;
		cache.readers
		    = DesktopSyncChatEligibility(files->fresh.source.peer)->readers;
		cache.readerConsent = true;
		cache.disclosedStreams = {u"config"_q};
		opening.cache = cache;
		io.current = [&] { return live; };
		io.onMain = [](Fn<void()> task) {
			if (!SyncStoreWorkerThread()) {
				task();
			} else {
				QMetaObject::invokeMethod(QCoreApplication::instance(),
				    std::move(task),
				    Qt::QueuedConnection);
			}
		};
		io.input
		    = [&](auto chat) { return std::optional(Route(chat).input); };
		io.peer = [&](auto chat, auto done) {
			CHECK(!SyncStoreWorkerThread());
			auto route = chat == files->fresh.source.peer.chat
			    ? files->fresh.source
			    : chat == files->fresh.target.peer.chat ? files->fresh.target
			                                            : Route(chat);
			if (wrongPeer) {
				route.peer.origin.login += u'x';
			}
			done(route);
		};
		io.inventory = [&](auto route, auto done) {
			CHECK(!SyncStoreWorkerThread());
			auto inventory = route.peer.chat == files->fresh.target.peer.chat
			    ? InventoryRows(route, files->cloud)
			    : InventoryRows(route,
			          {Receipt(route, files->original, 10, 100)});
			if (files->pending
			    && route.peer.chat == files->fresh.source.peer.chat) {
				inventory = InventoryRows(route,
				    {Receipt(route, files->original, 10, 100),
				        Receipt(route, RecordBytes(2), 11, 100)});
			}
			if (partial) {
				inventory.inventory.scan.status
				    = SyncHistoryScanStatus::InvalidResponse;
			}
			done(std::move(inventory));
		};
		io.request = [&](auto request, auto gate, auto done) {
			CHECK(!SyncStoreWorkerThread());
			++requests;
			if (hold) {
				pending = std::move(done);
				return;
			}
			auto wire = mtpBuffer();
			std::visit([&](const auto &value) { value.write(wire); },
			    request);
			auto cursor = wire.constData();
			const auto end = cursor + wire.size();
			const auto type = Read<MTPint>(cursor, end).v;
			auto result = DesktopSyncNativeReply();
			if (type == int(mtpc_messages_forwardMessages)
			    || type == int(mtpc_messages_sendMessage)) {
				Must(gate);
				auto allowed = false;
				gate([&](bool value) { allowed = value; });
				if (!allowed || unknown) {
					done(result);
					return;
				}
				++effects;
				if (holdEffect) {
					pending = std::move(done);
					return;
				}
				const auto flags = Read<MTPint>(cursor, end).v;
				auto random = uint64_t(0);
				auto id = 90;
				if (type == int(mtpc_messages_forwardMessages)) {
					CHECK((flags & (1 << 5)) && (flags & (1 << 11))
					    && !(flags & (1 << 12)));
					Read<MTPInputPeer>(cursor, end);
					const auto ids = Read<MTPVector<MTPint>>(cursor, end);
					const auto randoms
					    = Read<MTPVector<MTPlong>>(cursor, end);
					CHECK(ids.v.size() == 1
					    && (ids.v.front().v == 10 || ids.v.front().v == 11)
					    && randoms.v.size() == 1);
					random = randoms.v.front().v;
					id = 30 + int(files->cloud.size());
					files->cloud.push_back(Receipt(files->fresh.target,
					    ids.v.front().v == 11 ? RecordBytes(2)
					                          : files->original,
					    id,
					    300));
				} else {
					CHECK(flags & (1 << 5));
					Read<MTPInputPeer>(cursor, end);
					CHECK(qs(Read<MTPstring>(cursor, end))
					    == DesktopSyncChatMarkText());
					random = Read<MTPlong>(cursor, end).v;
					++files->markWrites;
				}
				result.updates = Receipt(files->fresh.target, random, id);
			} else if (type == int(mtpc_messages_searchGlobal)) {
				auto rows = QVector<MTPMessage>{
				    NativeMark(files->fresh.source, 9, 100)};
				if (files->markWrites && !hideTargetFromGlobal && !missingTargetMark) {
					rows.push_back(NativeMark(files->fresh.target, 90, 500));
				}
				if (peerMark && !hideTargetFromGlobal) { rows.push_back(NativeMark(files->fresh.target, peerMark, 700)); }
				result.messages = Response(Route({}), rows);
			} else if (type == int(mtpc_messages_search)) {
				Read<MTPint>(cursor, end);
				const auto input = Read<MTPInputPeer>(cursor, end);
				const auto query = qs(Read<MTPstring>(cursor, end));
				const auto route = input.type() == mtpc_inputPeerSelf
				    ? Route({})
				    : input.type() == mtpc_inputPeerChannel
					&& input.c_inputPeerChannel().vchannel_id().v == files->fresh.target.peer.chat.id
					? files->fresh.target : files->fresh.source;
				auto rows = QVector<MTPMessage>();
				if (query == u"#purplesync"_q) {
					rows.push_back(CarryDocument(route, files->original));
				} else if (route.peer.chat
				    == files->fresh.source.peer.chat) {
					rows.push_back(NativeMark(route, 9, 100));
				} else if (route.peer.chat == files->fresh.target.peer.chat) {
					++targetSearches;
					if (files->markWrites && !missingTargetMark) { rows.push_back(NativeMark(route, 90, 500)); }
					if (peerMark) { rows.push_back(NativeMark(route, peerMark, 700)); }
				}
				result.messages = Response(route, rows);
			} else if (type == int(mtpc_messages_getHistory)) {
				const auto input = Read<MTPInputPeer>(cursor, end);
				if (input.type() == mtpc_inputPeerChannel
					&& input.c_inputPeerChannel().vchannel_id().v == files->fresh.target.peer.chat.id) { ++targetHistories; }
				result.messages
				    = Response(input.type() == mtpc_inputPeerChannel
				            ? files->fresh.target
				            : files->fresh.source,
				        {});
			} else {
				auto route = files->fresh.source;
				if (type == int(mtpc_channels_getMessages)) {
					Read<MTPInputChannel>(cursor, end);
					route = files->fresh.target;
				}
				const auto ids
				    = Read<MTPVector<MTPInputMessage>>(cursor, end);
				Must(ids.v.size() == 1);
				const auto id = ids.v.front().c_inputMessageID().vid().v;
				exactIds.push_back(id);
				if (missingTargetMark && id == 90) {
					result.messages = Response(route, {MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(id), MTPPeer())});
				} else if (deleted.contains(id)) {
					result.messages = Response(route,
					    {MTP_messageEmpty(
					        MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),
					        MTP_int(id),
					        Target(route.peer.chat))});
				} else if (id == 10) {
					result.messages
					    = Receipt(route, files->original, 10, 100).response;
				} else {
					result.messages = Response(route,
					    {NativeMark(route, id, id == 9 ? 100 : id == peerMark ? 700 : 500)});
				}
				if (malformedExact) {
					result.messages = Response(route, {});
				}
			}
			const auto again = duplicate && result.updates.has_value();
			done(result);
			if (again) {
				done(result);
			}
		};
		io.upload = [&](auto permit, auto renew, auto done) {
			CHECK(!SyncStoreWorkerThread());
			auto next = std::optional<DesktopSyncCarryPermit>();
			renew([&](auto value) { next = std::move(value); });
			Must(next);
			CHECK(next->uploadMatches(next->uploadContent()));
			++uploads;
			if (unknown) {
				done({});
				return;
			}
			const auto id = 30 + int(files->cloud.size());
			files->cloud.push_back(Receipt(files->fresh.target,
			    next->uploadContent(),
			    id,
			    300));
			++applies;
			done(Receipt(next->target(), next->randomId(), id));
		};
		io.removeMark = [&](auto permit, auto renew, auto done) {
			CHECK(!SyncStoreWorkerThread());
			const auto request = permit.request();
			Must(request);
			CHECK(permit.row().location.markId == 9);
			++markDeletes;
			if (unknown) {
				done({});
				return;
			}
			deleted.emplace(9);
			done(MTP_messages_affectedMessages(MTP_int(8), MTP_int(1)));
		};
		io.cleanup = [&](auto permit, auto renew, auto done) {
			CHECK(!SyncStoreWorkerThread());
			const auto request = permit.request();
			Must(request);
			++cleanupDeletes;
			if (unknown) {
				done({});
				return;
			}
			deleted.emplace(permit.item().reference.messageId);
			done(MTP_messages_affectedMessages(MTP_int(8), MTP_int(1)));
		};
		io.localText = [&](auto done) { done(files->dirty); };
		io.apply = [&](const auto &, auto) {
			CHECK(!SyncStoreWorkerThread());
			++applies;
		};
		io.affected = [&](const auto &, const auto &) {
			CHECK(!SyncStoreWorkerThread());
			++applies;
			return true;
		};
		io.cancel = [&] { ++cancels; };
		renewal.effect
		    = [&](auto command, auto permit, auto fresh, auto done) {
			      CHECK(!SyncStoreWorkerThread());
			      ++renewals;
			      done(Worker([&] {
				      return files->engine->renew(command, permit, fresh);
			      }));
		      };
		renewal.beginCleanup = [&](auto command, auto permit, auto done) {
			done(Worker([&] {
				return files->engine->beginCleanupRenew(command, permit);
			}));
		};
		renewal.cleanup = [&](auto command,
		                      auto permit,
		                      auto read,
		                      auto reply,
		                      auto done) {
			done(Worker([&] {
				return files->engine->renewCleanup(command,
				    permit,
				    read,
				    reply);
			}));
		};
		adapter
		    = std::make_unique<DesktopSyncChatNative>(opening, io, renewal);
	}
	~NativeHarness() {
		adapter.reset();
		Worker([&] { files.reset(); });
	}
	DesktopSyncCarryReply send(DesktopSyncCarryCommand command) {
		auto reply = std::optional<DesktopSyncCarryReply>();
		adapter->transport().send(std::move(command),
		    [&](auto value) { reply = std::move(value); });
		Must(reply);
		return *reply;
	}
	void open(bool cleanup = false) {
		auto resolved = std::optional<DesktopSyncChatSetupSnapshot>();
		ResolveDesktopSyncNative(opening, io, [&](auto value) {
			resolved = std::move(value);
		});
		Must(resolved);
		files->start.resolution = ResolveSyncChat(resolved->input);
		files->start.oldRead = resolved->input.cacheRead;
		Worker([&] {
			const auto state
			    = CaptureSyncPrivateFile(files->root + u"/state.json"_q,
			        kSyncCarryJournalStorageLimit)
			          .bytes;
			files->engine->open(files->start,
			    files->fresh.target.peer,
			    u"config"_q,
			    DesktopSyncCarryOpening{state, {}, cleanup});
		});
		if (!files->progress.command) {
			std::printf("OPEN reason=%s store=%d journal=%d\n",
			    qPrintable(files->progress.reason),
			    int(files->progress.storeStatus),
			    int(files->progress.journalStatus));
		}
	}
	void run() {
		for (auto n = 0; files->progress.command && n != 80; ++n) {
			const auto command = *files->progress.command;
			actions.push_back(command.action);
			const auto reply = send(command);
			CHECK(reply.command.ticket == command.ticket
			    && reply.command.run == command.run
			    && reply.command.operation == command.operation
			    && reply.command.revision == command.revision);
			if (reply.outcome != SyncCarryOutcome::Success) {
				std::printf("ADAPTER UNKNOWN action=%d dispatch=%d\n",
				    int(command.action),
				    command.dispatch);
			}
			Worker([&] { files->engine->complete(reply); });
		}
	}
};

void NativeFlows() {
	CHECK(!SyncStoreWorkerThread());
	CHECK(!DesktopSyncChatCleanupActivated()
	    && !DesktopSyncChatMarkRemovalActivated());
	for (const auto own : {false, true}) {
		auto h = NativeHarness(own);
		h.open();
		h.run();
		std::printf("FLOW own=%d reason=%s status=%d phase=%d requests=%d "
		            "renew=%d\n",
		    own,
		    qPrintable(h.files->progress.reason),
		    int(h.files->progress.journalStatus),
		    h.files->progress.journal ? int(h.files->progress.journal->phase)
		                              : -1,
		    h.requests,
		    h.renewals);
		CHECK(h.files->progress.terminal
		    && h.files->progress.journal->phase == SyncCarryPhase::Done);
		CHECK(h.renewals > 0 && h.effects > 0 && h.applies > 0);
		CHECK(h.uploads == int(own));
		for (const auto action : {SyncCarryJournalAction::ResolveAgain,
		         SyncCarryJournalAction::Plan,
		         SyncCarryJournalAction::Deliver,
		         SyncCarryJournalAction::VerifyDestination,
		         SyncCarryJournalAction::PostMark,
		         SyncCarryJournalAction::ReadMark}) {
			CHECK(std::find(h.actions.begin(), h.actions.end(), action)
			    != h.actions.end());
		}
	}
	for (auto mutation = 0; mutation != 4; ++mutation) {
		auto h = NativeHarness();
		h.open();
		if (mutation == 0) {
			h.partial = true;
		}
		if (mutation == 1) {
			h.wrongPeer = true;
		}
		if (mutation == 2) {
			h.unknown = true;
		}
		if (mutation == 3) {
			h.files->fresh.target.peer.facts->protectedContent = true;
		}
		if (mutation == 2) {
			for (auto n = 0; h.files->progress.command
			    && !h.files->progress.command->dispatch && n != 30;
			    ++n) {
				const auto reply = h.send(*h.files->progress.command);
				Worker([&] { h.files->engine->complete(reply); });
			}
		}
		Must(h.files->progress.command);
		auto command = *h.files->progress.command;
		const auto reply = h.send(command);
		CHECK(reply.outcome == SyncCarryOutcome::Unknown && h.effects == 0);
	}
	{
		auto h = NativeHarness();
		h.open();
		h.hold = true;
		auto delivered = 0;
		h.adapter->transport().send(*h.files->progress.command,
		    [&](auto) { ++delivered; });
		Must(h.pending);
		const auto old = h.pending;
		h.adapter->cancel();
		h.live = false;
		old({});
		CHECK(delivered == 0 && h.cancels == 1);
	}
	{
		auto h = NativeHarness();
		auto command = DesktopSyncCarryCommand();
		command.origin = Origin;
		CHECK(h.send(command).outcome == SyncCarryOutcome::Unknown
		    && h.requests == 0);
	}
	{
		auto h = NativeHarness();
		h.open(true);
		h.run();
		CHECK(h.files->progress.terminal && h.cleanupDeletes == 1
		    && h.deleted.contains(10));
	}
}

void NativeCommandRefusals() {
	const auto reads = {SyncCarryJournalAction::ResolvePending,
	    SyncCarryJournalAction::ResolveAgain,
	    SyncCarryJournalAction::Plan,
	    SyncCarryJournalAction::Deliver,
	    SyncCarryJournalAction::PostMark,
	    SyncCarryJournalAction::VerifyDestination,
	    SyncCarryJournalAction::ReadMark,
	    SyncCarryJournalAction::PrepareCache,
	    SyncCarryJournalAction::CompleteCache,
	    SyncCarryJournalAction::Cleanup};
	{
		auto h = NativeHarness();
		h.open();
		h.partial = true;
		for (const auto action : reads) {
			auto command = *h.files->progress.command;
			command.action = action;
			const auto reply = h.send(command);
			CHECK(reply.outcome == SyncCarryOutcome::Unknown
			    && h.effects == 0);
		}
	}
	{
		auto h = NativeHarness(false, true);
		h.open();
		h.run();
		CHECK(h.files->progress.terminal);
		CHECK(std::find(h.actions.begin(),
		          h.actions.end(),
		          SyncCarryJournalAction::ResolvePending)
		    != h.actions.end());
	}
	{
		auto h = NativeHarness();
		h.duplicate = true;
		h.open();
		h.run();
		CHECK(h.files->progress.terminal && h.effects == h.applies);
	}
	{
		auto h = NativeHarness();
		h.open();
		for (auto n = 0; h.files->progress.command
		    && !h.files->progress.command->dispatch && n != 30;
		    ++n) {
			const auto reply = h.send(*h.files->progress.command);
			Worker([&] { h.files->engine->complete(reply); });
		}
		Must(h.files->progress.command
		    && !h.files->progress.command->effects.empty());
		const auto command = *h.files->progress.command;
		h.holdEffect = true;
		auto delivered = 0;
		h.adapter->transport().send(command, [&](auto) { ++delivered; });
		Must(h.pending);
		h.live = false;
		auto reply = DesktopSyncNativeReply();
		reply.updates = Receipt(command.effects.front().target(),
		    command.effects.front().randomId(),
		    77);
		h.pending(reply);
		CHECK(delivered == 0 && h.applies == 1);
	}
}

void NativeResolverLoss() {
	for (auto shape = 0; shape != 3; ++shape) {
		auto h = NativeHarness();
		const auto lost = PeerToken{PeerTokenKind::Channel, 17};
		auto opening = h.opening;
		opening.scope.settingsChat = lost;
		opening.cache->mark.chat = lost;
		opening.scope.mark = opening.cache->mark;
		opening.scope.carry = true;
		opening.cache->carry = QJsonObject{{u"synthetic"_q, true}};
		h.io.peer = [&](auto chat, auto done) {
			done(chat == lost ? std::nullopt : std::optional(Route(chat)));
		};
		h.io.request = [&](auto request, auto gate, auto done) {
			auto result = DesktopSyncNativeReply();
			std::visit(
			    [&](const auto &value) {
				    const auto type = value.type();
				    CHECK(type != mtpc_messages_sendMessage
				        && type != mtpc_messages_forwardMessages && !gate);
				    if (type == mtpc_channels_getMessages) {
					    result.error = u"CHANNEL_PRIVATE"_q;
				    } else if (type == mtpc_messages_getHistory) {
					    result.error = shape == 0 ? u"CHANNEL_PRIVATE"_q
					                              : u"NETWORK_UNKNOWN"_q;
				    } else {
					    result.messages = Response(Route({}), {});
				    }
				    if (shape == 2 && type == mtpc_channels_getMessages) {
					    result.error = u"NETWORK_UNKNOWN"_q;
				    }
			    },
			    request);
			done(result);
		};
		auto resolved = std::optional<DesktopSyncChatSetupSnapshot>();
		ResolveDesktopSyncNative(opening, h.io, [&](auto value) {
			resolved = std::move(value);
		});
		CHECK(bool(resolved) == (shape == 0));
		CHECK(opening.cache->carry.has_value() && opening.scope.carry);
		if (shape == 0) {
			CHECK(resolved->input.cacheRead == SyncChatDirectRead::ChatGone
			    && resolved->cache->carry == opening.cache->carry);
			h.adapter = std::make_unique<DesktopSyncChatNative>(opening,
			    h.io,
			    h.renewal);
			auto command = DesktopSyncCarryCommand();
			command.ticket = command.run = 1;
			command.operation = u"synthetic"_q;
			command.origin = Origin;
			command.source = lost;
			command.target = h.files->fresh.target.peer.chat;
			command.action = SyncCarryJournalAction::Plan;
			const auto reply = h.send(command);
			CHECK(reply.outcome == SyncCarryOutcome::Success
			    && reply.access == SyncCarrySourceAccess::ChatGone
			    && reply.fresh && !reply.fresh->source.peer.facts
			    && reply.destination);
		}
	}
}

void NativeLiveLosingMark() {
	for (const auto sameChat : {false, true}) {
		auto h = NativeHarness();
		const auto rival = sameChat ? h.files->fresh.target.peer.chat
			: PeerToken{PeerTokenKind::Channel, 999};
		const auto ordinaryRequest = h.io.request;
		const auto ordinaryInventory = h.io.inventory;
		h.io.inventory = [&](auto route, auto done) {
			if (!sameChat && route.peer.chat == rival) {
				done(InventoryRows(route, {}));
			} else {
				ordinaryInventory(route, done);
			}
		};
		h.io.request = [&](DesktopSyncNativeRequest request, auto gate, auto done) {
			if (h.files->markWrites) {
				if (std::holds_alternative<MTPmessages_SearchGlobal>(request)) {
					auto result = DesktopSyncNativeReply();
					result.messages = Response(Route({}), {
						NativeMark(h.files->fresh.source, 9, 100),
						NativeMark(h.files->fresh.target, 90, 500),
						NativeMark(Route(rival), 91, 600)});
					done(result);
					return;
				}
				if (std::holds_alternative<MTPchannels_GetMessages>(request)) {
					auto wire = mtpBuffer();
					std::get<MTPchannels_GetMessages>(request).write(wire);
					auto cursor = wire.constData();
					const auto end = cursor + wire.size();
					Read<MTPint>(cursor, end);
					const auto channel = Read<MTPInputChannel>(cursor, end);
					const auto ids = Read<MTPVector<MTPInputMessage>>(cursor, end);
					if (channel.c_inputChannel().vchannel_id().v == rival.id
						&& ids.v.size() == 1 && ids.v.front().c_inputMessageID().vid().v == 91) {
						auto result = DesktopSyncNativeReply();
						result.messages = Response(Route(rival), {NativeMark(Route(rival), 91, 600)});
						done(result);
						return;
					}
				}
			}
			ordinaryRequest(std::move(request), std::move(gate), std::move(done));
		};
		h.adapter.reset();
		h.adapter = std::make_unique<DesktopSyncChatNative>(h.opening, h.io, h.renewal);
		h.open(true);
		const auto before = Worker([&] {
			return CaptureSyncPrivateFile(h.files->root + u"/state.json"_q,
				kSyncCarryJournalStorageLimit).bytes;
		});
		auto observed = false;
		for (auto n = 0; h.files->progress.command && n != 100; ++n) {
			const auto command = *h.files->progress.command;
			const auto reply = h.send(command);
			if (command.action == SyncCarryJournalAction::ReadMark) {
				Must(reply.fresh);
				const auto resolved = ResolveSyncChat(reply.fresh->resolutionInput);
				CHECK(resolved.winner == std::optional(SyncChatCache{rival, 91, 600}));
				CHECK(command.markId == (sameChat ? 91 : 90) && reply.outcome == SyncCarryOutcome::Success);
				observed = true;
			}
			Worker([&] { h.files->engine->complete(reply); });
		}
		CHECK(observed && h.files->progress.terminal && !h.files->progress.command);
		Must(h.files->progress.journal);
		CHECK(h.files->progress.journal->stateCommitted);
		CHECK(h.files->progress.journal->phase == (sameChat ? SyncCarryPhase::Done : SyncCarryPhase::FinishedForFollow));
		CHECK(h.cleanupDeletes == int(sameChat) && h.deleted.size() == size_t(sameChat));
		CHECK(Worker([&] { return ParseSyncLocalState(CaptureSyncPrivateFile(h.files->root + u"/state.json"_q,
			kSyncCarryJournalStorageLimit).bytes).state.chat; }) == h.files->fresh.target.peer.chat);
		CHECK(!before.isEmpty());
	}
}

void NativeRecoverySeedAndNamedRead() {
	for (const auto peerWinner : {false, true}) {
		auto h = NativeHarness(); h.open();
		for (auto budget = 0; h.files->progress.command && budget != 100; ++budget) {
			const auto command = *h.files->progress.command;
			if (command.action == SyncCarryJournalAction::PostMark && command.dispatch) { break; }
			const auto reply = h.send(command);
			Worker([&] { h.files->engine->complete(reply); });
		}
		Must(h.files->progress.command && h.files->progress.command->dispatch);
		if (peerWinner) { h.peerMark = 91; }
		else { static_cast<void>(h.send(*h.files->progress.command)); }
		h.hideTargetFromGlobal = true;
		const auto effects = h.effects;
		Worker([&] { h.files->newEngine(true); });
		h.run();
		Must(h.files->progress.journal);
		CHECK(h.files->progress.terminal && h.files->progress.journal->phase == SyncCarryPhase::Done);
		CHECK(h.targetSearches > 0 && h.effects == effects && h.files->markWrites == int(!peerWinner));
		CHECK(h.files->progress.journal->targetMark->markId == (peerWinner ? 91 : 90));
		CHECK(std::find(h.exactIds.begin(), h.exactIds.end(), peerWinner ? 91 : 90) != h.exactIds.end());
	}
}

void NativePeerlessEmptyNeverProvesAbsent() {
	auto h = NativeHarness(); h.files->start.removeOldMarkAuthorized = true; h.open(true);
	for (auto budget = 0; h.files->progress.command && budget != 100; ++budget) {
		const auto command = *h.files->progress.command;
		const auto reply = h.send(command);
		Worker([&] { h.files->engine->complete(reply); });
		if (command.action == SyncCarryJournalAction::PostMark && command.dispatch) { break; }
	}
	Must(h.files->markWrites == 1);
	const auto effects = h.effects;
	const auto request = h.files->progress.journal->markRequestId;
	h.missingTargetMark = true;
	auto uncertain = 0;
	const auto check = [&] {
		for (auto budget = 0; h.files->progress.command && budget != 100; ++budget) {
			const auto command = *h.files->progress.command;
			CHECK(!command.dispatch && command.action != SyncCarryJournalAction::PostMark
				&& command.action != SyncCarryJournalAction::Deliver && command.action != SyncCarryJournalAction::Cleanup);
			const auto reply = h.send(command);
			if (command.action == SyncCarryJournalAction::ReadMark) {
				Must(reply.fresh && reply.markResponse);
				CHECK(reply.outcome == SyncCarryOutcome::Success && reply.markId == 90);
				CHECK(!reply.neverLandedConfirmed && reply.neverLandedRequestId.isEmpty());
				const auto read = DesktopSyncChatCarryMarkReadback(reply.fresh->target, 90,
					*reply.markResponse, DesktopSyncChatMarkText());
				CHECK(!read);
				const auto &journal = *h.files->progress.journal;
				auto event = SyncCarryEvent(journal, SyncCarryEventKind::MarkRead);
				event.outcome = SyncCarryOutcome::Success; event.noRequestInFlight = true;
				event.mark = read.value_or(SyncCarryMarkReadback());
				CHECK(event.mark.outcome == SyncCarryMarkOutcome::Unknown && !event.neverLandedConfirmed);
				const auto held = AdvanceSyncCarryJournal(journal, event, DesktopSyncCarryLogin(h.files->scope.loginAuthority));
				CHECK(held.status == SyncCarryJournalStatus::Wait && held.journal.markAttempted);
				CHECK(NextSyncCarryJournalAction(held.journal) == SyncCarryJournalAction::ReadMark);
				event.mark.outcome = SyncCarryMarkOutcome::Absent;
				event.mark.account = journal.account; event.mark.chat = journal.target;
				const auto unproven = AdvanceSyncCarryJournal(journal, event, DesktopSyncCarryLogin(h.files->scope.loginAuthority));
				CHECK(unproven.status == SyncCarryJournalStatus::Wait && unproven.journal.markAttempted);
				CHECK(NextSyncCarryJournalAction(unproven.journal) == SyncCarryJournalAction::ReadMark);
				++uncertain;
			}
			Worker([&] { h.files->engine->complete(reply); });
		}
		CHECK(h.effects == effects && h.files->markWrites == 1 && h.cleanupDeletes == 0 && h.markDeletes == 0);
		CHECK(h.files->progress.journal->markAttempted && h.files->progress.journal->markRequestId == request);
	};
	check();
	for (const auto delay : {30000, 120000, 600000}) {
		Must(h.files->progress.markRecoveryAfter);
		CHECK(*h.files->progress.markRecoveryAfter == delay && !h.files->progress.terminal);
		Worker([&] { h.files->clock += delay; h.files->newEngine(true); });
		check();
	}
	Must(h.files->progress.terminal && h.files->progress.journal);
	const auto &journal = *h.files->progress.journal;
	CHECK(journal.phase == SyncCarryPhase::FinishedForFollow && journal.removeOldMarkAuthorized);
	CHECK(!journal.attemptedMark && !journal.targetMark && !journal.abandonedCleanup.empty());
	CHECK(uncertain == 4 && std::count(h.exactIds.begin(), h.exactIds.end(), 90) == 4);
}

void NativeNoWinnerRecoveryCadence() {
	for (const auto backwards : {false, true}) {
		auto h = NativeHarness();
		h.files->start.removeOldMarkAuthorized = true;
		Worker([&] { MarkEffect(*h.files); });
		h.deleted.insert(9);
		auto delays = 0;
		h.io.delay = [&](int, Fn<void()>) { ++delays; };
		const auto request = h.io.request;
		h.io.request = [&](auto value, auto gate, auto done) {
			if (std::holds_alternative<MTPmessages_SearchGlobal>(value)
				|| std::holds_alternative<MTPmessages_Search>(value)) {
				CHECK(!gate);
				auto reply = DesktopSyncNativeReply();
				reply.messages = Response(Route({}), {});
				done(reply);
			} else {
				request(std::move(value), std::move(gate), std::move(done));
			}
		};
		const auto restart = [&] {
			h.adapter.reset();
			h.adapter = std::make_unique<DesktopSyncChatNative>(h.opening,
				h.io, h.renewal);
			Worker([&] { h.files->newEngine(true); });
		};
		const auto image = [&] {
			return Worker([&] {
				auto result = MarkRecoveryImage(*h.files);
				if (result.isEmpty()) {
					for (const auto &bytes : h.files->progress.retainedImages) {
						result = QJsonDocument::fromJson(bytes).object()
							.value(u"transport"_q).toObject()
							.value(u"mark_recovery"_q).toObject();
						if (!result.isEmpty()) { break; }
					}
				}
				return result;
			});
		};
		auto advances = 0;
		const auto check = [&] {
			for (auto budget = 0; h.files->progress.command && budget != 80; ++budget) {
				const auto command = *h.files->progress.command;
				CHECK(command.markRecovery && !command.dispatch);
				CHECK(command.action == SyncCarryJournalAction::VerifyDestination
					|| command.action == SyncCarryJournalAction::ResolveAgain);
				const auto before = image().value(u"checks"_q).toInt();
				auto reply = std::optional<DesktopSyncCarryReply>();
				h.adapter->transport().send(command,
					[&](auto value) { reply = std::move(value); });
				CHECK(reply.has_value() && delays == 0);
				if (!reply) { return false; }
				if (command.action == SyncCarryJournalAction::ResolveAgain) {
					Must(reply->fresh);
					CHECK(reply->fresh->fullResolutionComplete);
					CHECK(ResolveSyncChat(reply->fresh->resolutionInput).action
						== SyncChatAction::Searching);
				}
				Worker([&] { h.files->engine->complete(*reply); });
				const auto after = image().value(u"checks"_q).toInt();
				CHECK(after >= before && after <= before + 1);
				CHECK(command.action == SyncCarryJournalAction::ResolveAgain || after == before);
				advances += after - before;
			}
			CHECK(!h.files->progress.command);
			CHECK(delays == 0 && h.effects == 0 && h.files->markWrites == 0
				&& h.cleanupDeletes == 0 && h.markDeletes == 0);
			CHECK(h.deleted == std::set<int>{9} && h.files->cloud.size() == 1);
			return true;
		};
		restart();
		if (!check()) { return; }
		CHECK(advances == 1 && image().value(u"checks"_q).toInt() == 1);
		CHECK(image().value(u"next_at"_q).toString().toLongLong() == h.files->clock + 30000);
		const auto retained = image();
		Worker([&] { h.files->clock += 1000; });
		restart();
		if (!check()) { return; }
		CHECK(image() == retained && h.files->progress.markRecoveryAfter == 29000);
		const auto wall = base::unixtime::now();
		if (backwards) {
			base::unixtime::update(wall - 86400, true);
			Worker([&] { h.files->clock = 10; });
			restart();
			if (!check()) { return; }
			CHECK(image().value(u"checks"_q).toInt() == 1);
			CHECK(image().value(u"next_at"_q).toString() == u"30010"_q);
			CHECK(h.files->progress.markRecoveryAfter == 30000);
		}
		for (const auto delay : {30000, 120000, 600000}) {
			Must(h.files->progress.markRecoveryAfter);
			CHECK(*h.files->progress.markRecoveryAfter
				== (delay == 30000 && !backwards ? 29000 : delay));
			Worker([&] { h.files->clock += *h.files->progress.markRecoveryAfter; });
			const auto before = image().value(u"checks"_q).toInt();
			const auto priorAdvances = advances;
			restart();
			if (!check()) { return; }
			CHECK(advances == priorAdvances + 1 && image().value(u"checks"_q).toInt() == before + 1);
			CHECK(image().value(u"next_at"_q).toString().toLongLong()
				== h.files->clock + (before == 1 ? 120000 : before == 2 ? 600000 : 0));
			if (backwards) { CHECK(base::unixtime::now() < wall - 86000); }
		}
		if (backwards) { base::unixtime::update(wall, true); }
		Must(h.files->progress.journal);
		CHECK(h.files->progress.terminal
			&& h.files->progress.journal->phase == SyncCarryPhase::FinishedForFollow);
		CHECK(image().value(u"checks"_q).toInt() == 4 && delays == 0);
		CHECK(!h.files->progress.retainedImages.empty());
	}
}

void NativeOrdinarySearchingCadence() {
	auto h = NativeHarness();
	auto opening = h.opening;
	opening.scope.settingsChat = h.files->fresh.target.peer.chat;
	opening.cache->mark = {opening.scope.settingsChat, 0, 0};
	opening.scope.mark = opening.cache->mark;
	h.io.request = [&](auto request, auto gate, auto done) {
		CHECK(!gate);
		CHECK(std::holds_alternative<MTPmessages_SearchGlobal>(request)
			|| std::holds_alternative<MTPmessages_Search>(request));
		auto reply = DesktopSyncNativeReply();
		reply.messages = Response(Route({}), {});
		done(reply);
	};
	auto timer = Fn<void()>();
	auto milliseconds = 0;
	auto timers = 0;
	h.io.delay = [&](int delay, Fn<void()> task) {
		++timers;
		milliseconds = delay;
		timer = std::move(task);
	};
	auto resolved = std::optional<DesktopSyncChatSetupSnapshot>();
	const auto wall = base::unixtime::now();
	ResolveDesktopSyncNative(opening, h.io,
		[&](auto value) { resolved = std::move(value); });
	CHECK(!resolved && timer && timers == 1
		&& milliseconds >= 29000 && milliseconds <= 30000);
	for (const auto seconds : {30, 120, 600}) {
		Must(timer);
		base::unixtime::update(base::unixtime::now() + seconds, true);
		auto task = std::move(timer);
		task();
		if (seconds == 600) {
			Must(resolved);
			CHECK(!timer && resolved->status.complete
				&& resolved->status.phase == DesktopSyncChatPhase::Lost);
		} else {
			CHECK(!resolved && timer && milliseconds == 60000);
		}
	}
	CHECK(timers == 3);
	base::unixtime::update(wall, true);
}

void NativeUnmarkedBinding() {
	auto h = NativeHarness();
	auto opening = h.opening;
	opening.scope.settingsChat = h.files->fresh.target.peer.chat;
	opening.cache->mark = {opening.scope.settingsChat, 0, 0};
	opening.scope.mark = opening.cache->mark;
	auto calls = 0;
	h.io.request = [&](auto request, auto gate, auto done) {
		CHECK(!gate);
		CHECK(std::holds_alternative<MTPmessages_SearchGlobal>(request) || std::holds_alternative<MTPmessages_Search>(request));
		++calls;
		auto reply = DesktopSyncNativeReply(); reply.messages = Response(Route({}), {});
		done(reply);
	};
	auto resolved = std::optional<DesktopSyncChatSetupSnapshot>();
	ResolveDesktopSyncNative(opening, h.io, [&](auto value) { resolved = std::move(value); });
	Must(resolved);
	CHECK(resolved->input.cacheRead == SyncChatDirectRead::MarkGone);
	CHECK(resolved->status.resolution.action == SyncChatAction::Searching);
	CHECK(calls == 2 && h.exactIds.empty() && h.targetHistories == 0);
}

void NativeMarkPort() {
	for (const auto malformed : {false, true}) {
		auto h = NativeHarness();
		Worker([&] { h.files->engine.reset(); });
		auto snapshot = h.opening;
		snapshot.scope.settingsChat = h.files->fresh.target.peer.chat;
		snapshot.scope.mark
		    = SyncChatCache{snapshot.scope.settingsChat, 90, 500};
		snapshot.cache->mark = *snapshot.scope.mark;
		snapshot.cache->readers
		    = DesktopSyncChatEligibility(h.files->fresh.target.peer)
		          ->readers;
		h.files->markWrites = 1;
		h.files->cloud.push_back(
		    Receipt(h.files->fresh.target, h.files->original, 30, 300));
		h.adapter = std::make_unique<DesktopSyncChatNative>(snapshot,
		    h.io,
		    h.renewal);
		const auto row
		    = CaptureDesktopSyncChatStaleMark(h.files->fresh.source.peer,
		        h.files->scope.loginAuthority,
		        NativeMark(h.files->fresh.source, 9, 100));
		Must(row);
		auto terminal = std::atomic<bool>(false);
		auto absent = std::atomic<bool>(false);
		auto context = DesktopSyncChatMarkPort();
		context.origin = [] { return Origin; };
		context.login = [&] { return h.files->scope.loginAuthority; };
		context.window = [] { return u"native-window"_q; };
		context.current = [&] { return h.live; };
		context.foreground = [] { return true; };
		context.worker = [](Fn<void()> task) {
			if (SyncStoreWorkerThread()) {
				task();
			} else {
				Worker(std::move(task));
			}
		};
		context.status = [&](auto progress) {
			terminal.store(progress.terminal);
			absent.store(progress.absent);
		};
		auto port = h.adapter->markPort(std::move(context));
		if (malformed) {
			h.malformedExact = true;
		}
		auto flow = std::make_unique<DesktopSyncChatMarkActions>(*row,
		    u"native-window"_q,
		    h.files->root,
		    h.files->scope,
		    std::move(port));
		flow->start();
		for (auto n = 0; !terminal.load() && n != 1000; ++n) {
			QCoreApplication::processEvents();
			Worker([] {});
		}
		CHECK(terminal.load() && absent.load() == !malformed
		    && h.markDeletes == int(!malformed));
		CHECK(h.applies == int(!malformed));
		flow.reset();
		Worker([] {});
	}
}

}

int main(int argc, char** argv) {
	auto app = QCoreApplication(argc, argv);
	std::setbuf(stdout, nullptr);
	TestWorker = std::make_unique<NativeWorker>();
	NativeFlows();
	NativeMarkPort();
	NativeCommandRefusals();
	NativeResolverLoss();
	NativeLiveLosingMark();
	NativeRecoverySeedAndNamedRead();
	NativePeerlessEmptyNeverProvesAbsent();
	NativeUnmarkedBinding();
	NativeNoWinnerRecoveryCadence();
	NativeOrdinarySearchingCadence();
	TestWorker.reset();
	std::printf("NATIVE ADAPTER %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
