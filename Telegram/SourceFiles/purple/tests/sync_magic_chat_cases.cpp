#include "purple/purple_sync_magic_chat.h"

#include <QtCore/QStringList>

#include <deque>
#include <iostream>
#include <map>

namespace {

using namespace Purple;

MagicChatCandidate Magic(uint64_t id, bool counted) {
	auto result = MagicChatCandidate();
	result.channelId = id;
	result.title = QString::fromLatin1(kMagicSyncChatTitle);
	result.megagroup = true;
	result.facts.kind = SyncChatKind::Supergroup;
	result.facts.creator = true;
	result.facts.admin = true;
	result.facts.canSendFiles = true;
	if (counted) {
		result.facts.soleAdmin = true;
		result.facts.otherMembers = 0;
		result.facts.otherAdmins = 0;
	}
	return result;
}

struct Fake final : SyncMagicPort {
	int64_t clock = 1'000'000;
	bool live = true;
	QByteArray file;
	bool unreadable = false;
	SyncMagicLocal lists;
	std::map<uint64_t, MagicChatCandidate> known;
	std::deque<std::function<void()>> tasks;
	std::multimap<int64_t, std::function<void()>> timers;
	QStringList sent;
	QStringList lines;
	std::deque<SyncMagicReply<SyncMagicChats>> allReplies;
	std::vector<std::vector<uint64_t>> excepts;
	int listLoads = 0;
	std::map<uint64_t, MagicChatCandidate> archived;
	std::deque<SyncMagicReply<MagicChatCandidate>> fullReplies;
	std::deque<SyncMagicReply<MagicChatCandidate>> createReplies;

	bool current() override { return live; }
	int64_t now() override { return clock; }
	void post(std::function<void()> task) override { tasks.push_back(std::move(task)); }
	void delay(std::function<void()> task, int64_t ms) override {
		timers.emplace(clock * 1000 + ms, std::move(task));
	}
	std::optional<QByteArray> readBinding() override {
		return unreadable ? std::nullopt : std::optional(file);
	}
	bool writeBinding(const QByteArray &expected, const QByteArray &next) override {
		if (expected != file) {
			return false;
		}
		file = next;
		return true;
	}
	void local(std::optional<PeerToken> bound, std::function<void(std::optional<SyncMagicLocal>)> done) override {
		auto result = lists;
		result.boundState = SyncChatBoundState::Unknown;
		if (bound && bound->kind == PeerTokenKind::Self) {
			result.boundState = SyncChatBoundState::Present;
			result.bound = MagicChatCandidate();
			result.bound.facts.kind = SyncChatKind::Self;
			result.bound.facts.canSendFiles = true;
			result.bound.facts.otherMembers = 0;
			result.bound.facts.otherAdmins = 0;
		} else if (bound && known.contains(bound->id)) {
			result.boundState = SyncChatBoundState::Present;
			result.bound = known[bound->id];
		}
		done(result);
	}
	void loadLists(std::function<void()> done) override {
		++listLoads;
		known.insert(archived.begin(), archived.end());
		lists.mainLoaded = lists.archiveLoaded = true;
		done();
	}
	void allChats(std::vector<uint64_t> except, std::function<void(SyncMagicReply<SyncMagicChats>)> done) override {
		sent.push_back(u"allChats"_q);
		excepts.push_back(std::move(except));
		auto reply = allReplies.empty()
			? SyncMagicReply<SyncMagicChats>::Ok(SyncMagicChats())
			: allReplies.front();
		if (!allReplies.empty()) {
			allReplies.pop_front();
		}
		done(reply);
	}
	void fullInfo(uint64_t channelId, std::function<void(SyncMagicReply<MagicChatCandidate>)> done) override {
		sent.push_back(u"fullInfo"_q);
		if (!fullReplies.empty()) {
			const auto reply = fullReplies.front();
			fullReplies.pop_front();
			done(reply);
			return;
		}
		done(SyncMagicReply<MagicChatCandidate>::Ok(Magic(channelId, true)));
	}
	void create(std::function<void(SyncMagicReply<MagicChatCandidate>)> done) override {
		sent.push_back(u"create"_q);
		auto reply = createReplies.empty()
			? SyncMagicReply<MagicChatCandidate>::Ok(Magic(900, false))
			: createReplies.front();
		if (!createReplies.empty()) {
			createReplies.pop_front();
		}
		if (reply.value) {
			known[reply.value->channelId] = *reply.value;
		}
		done(reply);
	}
	void archive(uint64_t, std::function<void(SyncMagicReply<bool>)> done) override {
		sent.push_back(u"archive"_q);
		done(SyncMagicReply<bool>::Ok(true));
	}
	void mute(uint64_t, std::function<void(SyncMagicReply<bool>)> done) override {
		sent.push_back(u"mute"_q);
		done(SyncMagicReply<bool>::Ok(true));
	}
	void diag(const QString &event, const std::vector<SyncDiagField> &fields) override {
		lines.push_back(SyncDiagLine(u"sync_chat"_q, event, fields));
	}

	void drain() {
		while (!tasks.empty()) {
			auto task = std::move(tasks.front());
			tasks.pop_front();
			task();
		}
	}
	void advance(int64_t seconds) {
		clock += seconds;
		while (!timers.empty() && timers.begin()->first <= clock * 1000) {
			auto task = std::move(timers.begin()->second);
			timers.erase(timers.begin());
			task();
			drain();
		}
	}
	[[nodiscard]] SyncChatBinding stored() const {
		return file.isEmpty() ? SyncChatBinding() : ParseSyncChatBinding(file).value_or(SyncChatBinding());
	}
	[[nodiscard]] bool logged(const QString &part) const {
		for (const auto &line : lines) {
			if (line.contains(part)) {
				return true;
			}
		}
		return false;
	}
};

SyncMagicReply<SyncMagicChats> Slice(int64_t count, std::vector<MagicChatCandidate> magic, std::vector<uint64_t> ids) {
	auto result = SyncMagicChats();
	result.count = count;
	result.ids = std::move(ids);
	for (const auto &chat : magic) {
		result.ids.push_back(chat.channelId);
	}
	result.candidates = std::move(magic);
	return SyncMagicReply<SyncMagicChats>::Ok(std::move(result));
}

SyncMagicResult Run(SyncMagicChat &chat, Fake &port, SyncMagicTrigger trigger, bool fresh = false) {
	auto result = SyncMagicResult();
	result.retryAt = -1;
	chat.lookup(trigger, fresh, [&](SyncMagicResult value) { result = value; });
	port.drain();
	return result;
}

} // namespace

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using Kind = SyncMagicResult::Kind;
	const auto channel = [](uint64_t id) { return PeerToken{ PeerTokenKind::Channel, id }; };

	{
		auto port = std::make_shared<Fake>();
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(port->sent == QStringList({ u"allChats"_q, u"create"_q, u"archive"_q, u"mute"_q, u"fullInfo"_q }),
			"absent: one absence proof, one create, archive, mute, then verify");
		expect(result.kind == Kind::Use && result.chat == channel(900), "absent: the created chat is used");
		const auto stored = port->stored();
		expect(stored.mode == SyncChatBindingMode::Magic && stored.chat == channel(900), "absent: the binding is magic");
		expect(stored.createStep == SyncChatCreateStep::None && stored.verifiedAt == port->clock, "absent: verified");
		expect(port->logged(u"lookup_result result=created"_q), "absent: logged as created");
		expect(port->logged(u"lookup_start trigger=startup"_q), "absent: logged the trigger");
		expect(chat.hidden() == channel(900), "absent: the magic chat is hidden");
		port->sent.clear();
		const auto again = Run(chat, *port, SyncMagicTrigger::Auto);
		expect(again.kind == Kind::Use && port->sent.isEmpty(), "bound: an automatic lookup sends nothing");
		port->advance(kSyncChatVerifySeconds);
		const auto post = Run(chat, *port, SyncMagicTrigger::Post, true);
		expect(post.kind == Kind::Use && port->sent == QStringList({ u"fullInfo"_q }), "bound: a post re-verifies after a day");
	}
	{
		auto port = std::make_shared<Fake>();
		port->lists.visible = { Magic(70, false), Magic(40, false) };
		port->known[40] = Magic(40, false);
		port->known[70] = Magic(70, false);
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Screen);
		expect(port->sent == QStringList({ u"fullInfo"_q }), "visible: no absence proof and no create");
		expect(result.kind == Kind::Use && result.chat == channel(40), "visible: the oldest chat is used");
		expect(port->logged(u"lookup_result result=found count=2"_q), "visible: logged as found");
	}
	{
		auto port = std::make_shared<Fake>();
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(60);
		binding.verifiedAt = port->clock;
		port->file = *SerializeSyncChatBinding(binding);
		port->known[60] = Magic(60, true);
		port->known[20] = Magic(20, false);
		port->lists.visible = { Magic(60, true), Magic(20, false) };
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Use && result.chat == channel(20), "converge: an older magic chat wins");
		expect(port->logged(u"lookup_result result=rebound"_q), "converge: logged as rebound");
	}
	{
		auto port = std::make_shared<Fake>();
		port->allReplies.push_back(SyncMagicReply<SyncMagicChats>::Failed(u"FLOOD_WAIT_120"_q, 420));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Wait && result.retryAt == port->clock + 121, "flood: a long flood waits");
		expect(port->stored().waits.contains(QString::fromLatin1(kSyncChatAllChatsMethod)), "flood: the wait is stored");
		auto restarted = SyncMagicChat(port);
		port->sent.clear();
		for (const auto trigger : { SyncMagicTrigger::Startup, SyncMagicTrigger::Screen, SyncMagicTrigger::Manual,
				SyncMagicTrigger::Auto, SyncMagicTrigger::Post }) {
			const auto held = Run(restarted, *port, trigger, true);
			expect(held.kind == Kind::Wait, "flood: every trigger waits after a restart");
		}
		expect(port->sent.isEmpty(), "flood: nothing is sent before the deadline");
		port->timers.clear();
		port->advance(122);
		const auto later = Run(restarted, *port, SyncMagicTrigger::Startup);
		expect(later.kind == Kind::Use && port->sent.front() == u"allChats"_q, "flood: the request goes after the deadline");
	}
	{
		auto port = std::make_shared<Fake>();
		port->allReplies.push_back(SyncMagicReply<SyncMagicChats>::Failed(u"FLOOD_WAIT_10"_q, 420));
		auto chat = SyncMagicChat(port);
		auto result = SyncMagicResult();
		chat.lookup(SyncMagicTrigger::Startup, false, [&](SyncMagicResult value) { result = value; });
		port->drain();
		expect(port->sent == QStringList({ u"allChats"_q }) && result.kind == Kind::Unavailable, "short flood: retried in place");
		port->advance(11);
		expect(port->sent.size() >= 2 && port->sent[1] == u"allChats"_q && result.kind == Kind::Use, "short flood: one retry");
		expect(!port->stored().waits.contains(QString::fromLatin1(kSyncChatAllChatsMethod)), "short flood: not stored");
	}
	{
		auto port = std::make_shared<Fake>();
		port->createReplies.push_back(SyncMagicReply<MagicChatCandidate>::TimedOut());
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Wait && result.retryAt == port->clock + kSyncChatCreateUncertainSeconds,
			"uncertain: a lost create waits a day");
		expect(port->stored().createStep == SyncChatCreateStep::Uncertain, "uncertain: recorded");
		port->timers.clear();
		port->sent.clear();
		port->advance(3600);
		const auto later = Run(chat, *port, SyncMagicTrigger::Manual, true);
		expect(later.kind == Kind::Wait && !port->sent.contains(u"create"_q), "uncertain: no second create");
		port->known[901] = Magic(901, false);
		port->lists.visible = { Magic(901, false) };
		const auto arrived = Run(chat, *port, SyncMagicTrigger::Screen);
		expect(arrived.kind == Kind::Use && arrived.chat == channel(901), "uncertain: the arriving chat binds");
	}
	{
		auto port = std::make_shared<Fake>();
		port->createReplies.push_back(SyncMagicReply<MagicChatCandidate>::Failed(u"CHANNELS_TOO_MUCH"_q, 400));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Problem && result.problem == SyncChatProblem::CreateLimit && result.show,
			"limit: a creation limit is a problem shown once");
		expect(port->stored().createStep == SyncChatCreateStep::None, "limit: nothing is pending");
		chat.shown();
		expect(!chat.last().show, "limit: a second view does not show it again");
		port->drain();
		expect(port->stored().problemShown, "limit: shown is stored");
		port->sent.clear();
		const auto again = Run(chat, *port, SyncMagicTrigger::Auto);
		expect(again.kind == Kind::Problem && !again.show && port->sent.isEmpty(), "limit: no request until the user acts");
		expect(port->logged(u"problem_shown reason=create_limit"_q), "limit: logged once shown");
		const auto manual = Run(chat, *port, SyncMagicTrigger::Manual, true);
		expect(port->logged(u"auto_resumed reason=user_check"_q) && port->sent.contains(u"create"_q),
			"limit: a manual check decides afresh");
		expect(manual.kind == Kind::Use, "limit: and recovers");
	}
	{
		auto port = std::make_shared<Fake>();
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(50);
		binding.verifiedAt = port->clock;
		port->file = *SerializeSyncChatBinding(binding);
		auto chat = SyncMagicChat(port);
		const auto gone = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(port->sent == QStringList({ u"allChats"_q }), "gone: an unknown bound chat is settled by one absence proof");
		expect(gone.kind == Kind::Problem && gone.problem == SyncChatProblem::ChatGone, "gone: a missing chat is a problem");
		auto extra = Magic(50, true);
		extra.facts.otherMembers = 2;
		port->known[50] = extra;
		port->file = *SerializeSyncChatBinding(binding);
		auto other = SyncMagicChat(port);
		const auto member = Run(other, *port, SyncMagicTrigger::Startup);
		expect(member.kind == Kind::Problem && member.problem == SyncChatProblem::ExtraMember, "member: another member is a problem");
	}
	{
		auto port = std::make_shared<Fake>();
		port->known[30] = Magic(30, true);
		port->lists.visible = { Magic(30, false) };
		auto chat = SyncMagicChat(port);
		expect(Run(chat, *port, SyncMagicTrigger::Startup).kind == Kind::Use, "space: bound");
		chat.raise(SyncChatProblem::SpaceMismatch);
		port->drain();
		expect(chat.last().kind == Kind::Problem && chat.last().show, "space: raised by sync, to show");
		expect(port->logged(u"auto_paused reason=space_mismatch"_q), "space: logged");
		auto joined = SyncMagicResult();
		chat.joined([&](SyncMagicResult value) { joined = value; });
		port->drain();
		expect(joined.kind == Kind::Use && port->stored().problem == SyncChatProblem::None, "space: joining clears it");
		auto listed = false;
		chat.list(true, [&](bool ok) { listed = ok; });
		port->drain();
		expect(listed && port->stored().listed && !chat.hidden(), "listed: a listed chat is not hidden");
		auto picked = SyncMagicResult();
		chat.choose(PeerToken(), [&](SyncMagicResult value) { picked = value; });
		port->drain();
		expect(picked.kind == Kind::Use && picked.manual && picked.chat == PeerToken(), "manual: Saved Messages can be picked");
		expect(!chat.hidden(), "manual: a picked chat is never hidden");
		auto reset = SyncMagicResult();
		chat.useDefault([&](SyncMagicResult value) { reset = value; });
		port->drain();
		expect(reset.kind == Kind::Use && reset.chat == channel(30) && !reset.manual, "default: back to the magic chat");
		expect(port->stored().listed, "default: the list choice is kept");
		expect(port->logged(u"user_action action=default"_q), "default: logged");
	}
	{
		auto port = std::make_shared<Fake>();
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(60);
		binding.verifiedAt = port->clock;
		port->file = *SerializeSyncChatBinding(binding);
		auto shared = Magic(20, true);
		shared.facts.otherMembers = 2;
		port->known[60] = Magic(60, true);
		port->known[20] = Magic(20, false);
		port->lists.visible = { Magic(60, true), Magic(20, false) };
		port->fullReplies.push_back(SyncMagicReply<MagicChatCandidate>::Ok(shared));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Use && result.chat == channel(60), "rejected: the bound chat stays in use");
		expect(port->stored().problem == SyncChatProblem::None && port->stored().rejected.contains(20),
			"rejected: no problem, the older chat is skipped");
		expect(port->logged(u"rejected reason=extra_member"_q), "rejected: logged");
		port->sent.clear();
		const auto again = Run(chat, *port, SyncMagicTrigger::Auto, true);
		expect(again.kind == Kind::Use && port->sent.isEmpty(), "rejected: the next lookup does not read it again");
		const auto manual = Run(chat, *port, SyncMagicTrigger::Manual, true);
		expect(manual.kind == Kind::Use && port->sent == QStringList({ u"fullInfo"_q }),
			"rejected: a manual check considers it again");
		expect(!port->logged(u"auto_resumed"_q), "rejected: nothing was paused");
	}
	{
		auto port = std::make_shared<Fake>();
		auto shared = Magic(20, true);
		shared.facts.otherMembers = 2;
		port->known[20] = Magic(20, false);
		port->known[40] = Magic(40, false);
		port->lists.visible = { Magic(20, false), Magic(40, false) };
		port->fullReplies.push_back(SyncMagicReply<MagicChatCandidate>::Ok(shared));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Use && result.chat == channel(40), "rejected: an unbound lookup tries the next chat");
		expect(port->sent == QStringList({ u"fullInfo"_q, u"fullInfo"_q }), "rejected: two reads, no create");
	}
	{
		auto port = std::make_shared<Fake>();
		port->allReplies.push_back(Slice(3, {}, { 11, 12 }));
		port->allReplies.push_back(Slice(3, {}, { 13 }));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(port->sent.mid(0, 3) == QStringList({ u"allChats"_q, u"allChats"_q, u"create"_q }),
			"slices: two pages, then create");
		expect(port->excepts.size() >= 2 && port->excepts[0].empty()
			&& port->excepts[1] == std::vector<uint64_t>({ 11, 12 }), "slices: the second page excepts the first");
		expect(result.kind == Kind::Use, "slices: the complete list proves absence");
	}
	{
		auto port = std::make_shared<Fake>();
		for (auto i = 0; i != kSyncMagicAllChatsPages; ++i) {
			port->allReplies.push_back(Slice(100, {}, { uint64_t(11 + i) }));
		}
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(port->sent.count(u"allChats"_q) == kSyncMagicAllChatsPages && !port->sent.contains(u"create"_q),
			"slices: an incomplete list never creates");
		expect(result.kind == Kind::Wait && result.retryAt == port->clock + kSyncMagicPartialWait,
			"slices: an incomplete list waits");
		expect(port->logged(u"reason=list_partial"_q), "slices: logged");
	}
	{
		auto port = std::make_shared<Fake>();
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(50);
		binding.verifiedAt = port->clock;
		port->file = *SerializeSyncChatBinding(binding);
		for (auto i = 0; i != kSyncMagicAllChatsPages; ++i) {
			port->allReplies.push_back(Slice(100, { Magic(uint64_t(70 + i), false) }, {}));
		}
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Wait && port->stored().problem == SyncChatProblem::None,
			"slices: a bound chat missing from an incomplete list is not gone");
	}
	{
		auto port = std::make_shared<Fake>();
		port->createReplies.push_back(SyncMagicReply<MagicChatCandidate>::Failed(u"PEER_FLOOD"_q, 400));
		port->createReplies.push_back(SyncMagicReply<MagicChatCandidate>::Failed(u"PEER_FLOOD"_q, 400));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Wait && result.retryAt == port->clock + kSyncMagicErrorWait,
			"refused: PEER_FLOOD is a wait");
		expect(port->stored().waits.contains(QString::fromLatin1(kSyncChatCreateMethod)), "refused: the wait is stored");
		auto restarted = SyncMagicChat(port);
		port->sent.clear();
		const auto held = Run(restarted, *port, SyncMagicTrigger::Auto, true);
		expect(held.kind == Kind::Wait && !port->sent.contains(u"create"_q), "refused: no create before the wait ends");
		port->timers.clear();
		port->advance(kSyncMagicErrorWait + 1);
		const auto second = Run(chat, *port, SyncMagicTrigger::Auto, true);
		expect(second.kind == Kind::Wait && second.retryAt == port->clock + 2 * kSyncMagicErrorWait,
			"refused: a second refusal waits twice as long");
	}
	{
		auto port = std::make_shared<Fake>();
		port->allReplies.push_back(SyncMagicReply<SyncMagicChats>::Failed(u"FLOOD_WAIT_600"_q, 420));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Manual, true);
		expect(result.kind == Kind::Wait, "retry: a manual check ends in a wait");
		port->advance(601);
		expect(port->logged(u"lookup_start trigger=retry"_q), "retry: the wake-up is not a user check");
	}
	{
		auto port = std::make_shared<Fake>();
		const auto history = u"messages.getHistory"_q;
		auto chat = SyncMagicChat(port);
		chat.recordWait(history, port->clock + 600);
		port->drain();
		expect(chat.waitUntil(history) == port->clock + 600, "kept: a sync request flood is kept");
		expect(chat.waitUntil(u"messages.sendMedia"_q) == 0, "kept: per method");
		expect(port->logged(u"wait_recorded method=messages.gethistory seconds=600"_q), "kept: logged");
		auto restarted = SyncMagicChat(port);
		expect(restarted.waitUntil(history) == port->clock + 600, "kept: across a restart");
		port->advance(601);
		expect(restarted.waitUntil(history) == 0, "kept: until it ends");
	}
	{
		auto port = std::make_shared<Fake>();
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(60);
		binding.verifiedAt = port->clock;
		port->file = *SerializeSyncChatBinding(binding);
		port->allReplies.push_back(SyncMagicReply<SyncMagicChats>::Failed(u"FLOOD_WAIT_600"_q, 420));
		auto chat = SyncMagicChat(port);
		expect(chat.hidden() == channel(60), "hidden: the bound chat is hidden before the first lookup");
		const auto waited = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(waited.kind == Kind::Wait && chat.hidden() == channel(60), "hidden: and while a lookup waits");
	}
	{
		auto port = std::make_shared<Fake>();
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(60);
		binding.verifiedAt = port->clock;
		port->file = *SerializeSyncChatBinding(binding);
		port->archived[60] = Magic(60, true);
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Use && result.chat == channel(60) && port->listLoads == 1,
			"cold start: the archive list finds the bound chat");
		expect(port->sent.isEmpty(), "cold start: no getAllChats");
	}
	{
		auto port = std::make_shared<Fake>();
		for (auto i = 0; i != kSyncMagicAllChatsPages; ++i) {
			auto shared = Magic(uint64_t(70 + i), true);
			shared.facts.otherMembers = 2;
			port->allReplies.push_back(Slice(100, { Magic(uint64_t(70 + i), false) }, {}));
			port->fullReplies.push_back(SyncMagicReply<MagicChatCandidate>::Ok(shared));
		}
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Wait && port->stored().problem == SyncChatProblem::None,
			"slices: ineligible chats on an incomplete list raise no problem");
	}
	{
		auto port = std::make_shared<Fake>();
		port->unreadable = true;
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Unavailable && port->sent.isEmpty(), "unreadable: nothing is sent");
		expect(port->logged(u"reason=binding_unreadable"_q), "unreadable: logged");
	}
	{
		auto port = std::make_shared<Fake>();
		port->allReplies.push_back(SyncMagicReply<SyncMagicChats>::Failed(QString(), -1));
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Wait && result.retryAt == port->clock + 5, "network: backs off five seconds");
		expect(port->timers.size() == 1, "network: one wake-up is armed");
		chat.close();
		port->drain();
		port->sent.clear();
		port->advance(10);
		expect(port->sent.isEmpty(), "close: a closed lookup never wakes");
	}
	{
		auto port = std::make_shared<Fake>();
		auto chat = SyncMagicChat(port);
		auto first = SyncMagicResult();
		auto second = SyncMagicResult();
		chat.lookup(SyncMagicTrigger::Startup, false, [&](SyncMagicResult value) { first = value; });
		chat.lookup(SyncMagicTrigger::Post, true, [&](SyncMagicResult value) { second = value; });
		port->drain();
		expect(port->sent.count(u"create"_q) == 1, "single flight: one create for two triggers");
		expect(first.kind == Kind::Use && second.kind == Kind::Use, "single flight: both get the result");
	}
	{
		auto port = std::make_shared<Fake>();
		auto chat = SyncMagicChat(port);
		(void)Run(chat, *port, SyncMagicTrigger::Startup);
		auto clean = true;
		for (const auto &line : port->lines) {
			clean = clean && !line.contains(u"invalid"_q);
		}
		expect(clean, "diag: every word is in the vocabulary");
		expect(SyncMagicFloodSeconds(u"FLOOD_WAIT_7"_q) == 7 && !SyncMagicFloodSeconds(u"FLOOD_WAIT_"_q)
			&& SyncMagicFloodSeconds(u"FLOOD_PREMIUM_WAIT_3"_q) == 3 && !SyncMagicFloodSeconds(u"PEER_FLOOD"_q),
			"flood seconds parse");
	}
	{
		const auto group = [](QString space) {
			auto result = SyncDirectoryGroup();
			result.space = std::move(space);
			return result;
		};
		auto directory = SyncDirectory();
		directory.complete = true;
		directory.groups = { group(u"other"_q) };
		expect(SyncMagicSpaceMismatch(directory, u"mine"_q), "space: only foreign records mismatch");
		directory.groups.push_back(group(u"mine"_q));
		expect(!SyncMagicSpaceMismatch(directory, u"mine"_q), "space: an own record matches");
		directory.groups = { group(u"other"_q) };
		directory.unreadableCandidate = true;
		expect(!SyncMagicSpaceMismatch(directory, u"mine"_q), "space: an unreadable record decides nothing");
		directory.unreadableCandidate = false;
		directory.complete = false;
		expect(!SyncMagicSpaceMismatch(directory, u"mine"_q), "space: an incomplete scan decides nothing");
		directory.complete = true;
		expect(!SyncMagicSpaceMismatch(directory, QString()), "space: no own space decides nothing");
		directory.groups.clear();
		expect(!SyncMagicSpaceMismatch(directory, u"mine"_q), "space: an empty chat matches");
	}
	{
		auto binding = SyncChatBinding();
		binding.mode = SyncChatBindingMode::Magic;
		binding.chat = channel(5);
		auto use = SyncMagicResult();
		use.kind = Kind::Use;
		use.chat = channel(5);
		auto frame = SyncMagicFrameOf(use, binding);
		expect(frame.line == SyncMagicFrame::Line::Using && frame.canOpen() && frame.canList()
			&& !frame.canUseDefault() && !frame.canJoin(), "frame: the magic chat opens and lists");
		binding.mode = SyncChatBindingMode::Manual;
		binding.listed = true;
		frame = SyncMagicFrameOf(use, binding);
		expect(frame.manual && frame.listed && !frame.canList() && frame.canUseDefault(),
			"frame: a picked chat is never hidden and offers the default");
		use.chat = PeerToken{};
		frame = SyncMagicFrameOf(use, binding);
		expect(frame.savedMessages() && !frame.canOpen(), "frame: Saved Messages is not opened from here");
		auto wait = SyncMagicResult();
		wait.kind = Kind::Wait;
		wait.retryAt = 1234;
		frame = SyncMagicFrameOf(wait, std::nullopt);
		expect(frame.line == SyncMagicFrame::Line::Waiting && frame.retryAt == 1234 && !frame.chat,
			"frame: a wait shows its end");
		auto problem = SyncMagicResult();
		problem.kind = Kind::Problem;
		problem.chat = channel(5);
		problem.problem = SyncChatProblem::SpaceMismatch;
		problem.show = true;
		frame = SyncMagicFrameOf(problem, std::nullopt);
		expect(frame.line == SyncMagicFrame::Line::Problem && frame.showProblem && frame.canJoin(),
			"frame: space mismatch offers Join once shown");
		problem.problem = SyncChatProblem::ExtraMember;
		expect(!SyncMagicFrameOf(problem, std::nullopt).canJoin(), "frame: other problems offer no Join");
		expect(SyncMagicFrameOf(SyncMagicResult(), std::nullopt).line == SyncMagicFrame::Line::Looking,
			"frame: no result yet is looking");

		auto local = Magic(7, true);
		expect(SyncMagicPickOf(PeerToken{}, std::nullopt) == SyncMagicPick::Allowed, "pick: Saved Messages");
		expect(SyncMagicPickOf(channel(7), local) == SyncMagicPick::Allowed, "pick: a supergroup alone");
		local.facts.otherMembers = 1;
		expect(SyncMagicPickOf(channel(7), local) == SyncMagicPick::NotAlone, "pick: a shared supergroup");
		local.facts.otherMembers = std::nullopt;
		expect(SyncMagicPickOf(channel(7), local) == SyncMagicPick::Unknown, "pick: unknown members");
		local = Magic(7, true);
		local.megagroup = false;
		expect(SyncMagicPickOf(channel(7), local) == SyncMagicPick::Unsupported, "pick: a broadcast channel");
		expect(SyncMagicPickOf(channel(8), Magic(7, true)) == SyncMagicPick::Unsupported, "pick: another chat's facts");
		expect(SyncMagicPickOf(PeerToken{ PeerTokenKind::User, 9 }, std::nullopt) == SyncMagicPick::Unsupported,
			"pick: a private chat");
	}

	std::cout << "sync-magic-chat checks=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}
