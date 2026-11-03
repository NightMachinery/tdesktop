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
	std::deque<SyncMagicReply<std::vector<MagicChatCandidate>>> allReplies;
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
	void allChats(std::function<void(SyncMagicReply<std::vector<MagicChatCandidate>>)> done) override {
		sent.push_back(u"allChats"_q);
		auto reply = allReplies.empty()
			? SyncMagicReply<std::vector<MagicChatCandidate>>::Ok(std::vector<MagicChatCandidate>())
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
		port->allReplies.push_back(SyncMagicReply<std::vector<MagicChatCandidate>>::Failed(u"FLOOD_WAIT_120"_q, 420));
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
		port->allReplies.push_back(SyncMagicReply<std::vector<MagicChatCandidate>>::Failed(u"FLOOD_WAIT_10"_q, 420));
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
		port->unreadable = true;
		auto chat = SyncMagicChat(port);
		const auto result = Run(chat, *port, SyncMagicTrigger::Startup);
		expect(result.kind == Kind::Unavailable && port->sent.isEmpty(), "unreadable: nothing is sent");
		expect(port->logged(u"reason=binding_unreadable"_q), "unreadable: logged");
	}
	{
		auto port = std::make_shared<Fake>();
		port->allReplies.push_back(SyncMagicReply<std::vector<MagicChatCandidate>>::Failed(QString(), -1));
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

	std::cout << "sync-magic-chat checks=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}
