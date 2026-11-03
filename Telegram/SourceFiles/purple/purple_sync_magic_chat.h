#pragma once

#include "purple/purple_sync_chat_magic.h"
#include "purple/purple_sync_diag.h"
#include "purple/purple_sync_directory.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace Purple {

enum class SyncMagicTrigger { Startup, Screen, Manual, Auto, Post, Retry };

template <typename T>
struct SyncMagicReply {
	std::optional<T> value;
	QString error;
	int code = 0;
	bool timeout = false;

	[[nodiscard]] bool ok() const {
		return error.isEmpty() && !timeout && code == 0;
	}
	[[nodiscard]] static SyncMagicReply Ok(std::optional<T> value) {
		return { std::move(value), QString(), 0, false };
	}
	[[nodiscard]] static SyncMagicReply Failed(QString error, int code) {
		return { std::nullopt, std::move(error), code, false };
	}
	[[nodiscard]] static SyncMagicReply TimedOut() {
		return { std::nullopt, QString(), -504, true };
	}
};

// One messages.getAllChats page: its megagroups, every chat id it named, and the total of a slice (-1 when complete).
struct SyncMagicChats {
	std::vector<MagicChatCandidate> candidates;
	std::vector<uint64_t> ids;
	int64_t count = -1;
};

struct SyncMagicLocal {
	std::vector<MagicChatCandidate> visible;
	SyncChatBoundState boundState = SyncChatBoundState::Unknown;
	MagicChatCandidate bound;
	bool mainLoaded = false;
	bool archiveLoaded = false;
};

class SyncMagicPort {
public:
	virtual ~SyncMagicPort() = default;

	[[nodiscard]] virtual bool current() = 0;
	[[nodiscard]] virtual int64_t now() = 0;
	virtual void post(std::function<void()> task) = 0;
	virtual void delay(std::function<void()> task, int64_t ms) = 0;
	// Nullopt when the file cannot be read, empty when there is none.
	[[nodiscard]] virtual std::optional<QByteArray> readBinding() = 0;
	[[nodiscard]] virtual bool writeBinding(
		const QByteArray &expected,
		const QByteArray &next) = 0;
	virtual void local(
		std::optional<PeerToken> bound,
		std::function<void(std::optional<SyncMagicLocal>)> done) = 0;
	// Loads the main and archive dialog lists if they are not loaded; done once they are or give up.
	virtual void loadLists(std::function<void()> done) = 0;
	virtual void allChats(
		std::vector<uint64_t> except,
		std::function<void(SyncMagicReply<SyncMagicChats>)> done) = 0;
	virtual void fullInfo(
		uint64_t channelId,
		std::function<void(SyncMagicReply<MagicChatCandidate>)> done) = 0;
	virtual void create(
		std::function<void(SyncMagicReply<MagicChatCandidate>)> done) = 0;
	virtual void archive(
		uint64_t channelId,
		std::function<void(SyncMagicReply<bool>)> done) = 0;
	virtual void mute(
		uint64_t channelId,
		std::function<void(SyncMagicReply<bool>)> done) = 0;
	virtual void diag(
		const QString &event,
		const std::vector<SyncDiagField> &fields) = 0;
};

struct SyncMagicResult {
	enum class Kind { Use, Wait, Problem, Unavailable };

	Kind kind = Kind::Unavailable;
	std::optional<PeerToken> chat;
	SyncChatProblem problem = SyncChatProblem::None;
	bool show = false;
	int64_t retryAt = 0;
	bool manual = false;
};

inline constexpr auto kSyncMagicShortFlood = int64_t(30);
inline constexpr auto kSyncMagicAllChatsPages = 5;
inline constexpr auto kSyncMagicPartialWait = int64_t(60 * 60);
inline constexpr auto kSyncMagicErrorWait = int64_t(10 * 60);
inline constexpr auto kSyncMagicFoldersMethod = "folders.editPeerFolders";
inline constexpr auto kSyncMagicNotifyMethod = "account.updateNotifySettings";

[[nodiscard]] std::optional<int64_t> SyncMagicFloodSeconds(const QString &error);
[[nodiscard]] QString SyncMagicTriggerName(SyncMagicTrigger trigger);
[[nodiscard]] bool SyncMagicSpaceMismatch(
	const SyncDirectory &directory,
	const QString &space);

struct SyncMagicFrame {
	enum class Line { Looking, Using, Waiting, Problem };

	Line line = Line::Looking;
	std::optional<PeerToken> chat;
	SyncChatProblem problem = SyncChatProblem::None;
	int64_t retryAt = 0;
	bool manual = false;
	bool listed = false;
	bool showProblem = false;

	[[nodiscard]] bool savedMessages() const;
	[[nodiscard]] bool canOpen() const;
	[[nodiscard]] bool canList() const;
	[[nodiscard]] bool canUseDefault() const;
	[[nodiscard]] bool canJoin() const;
};

[[nodiscard]] SyncMagicFrame SyncMagicFrameOf(
	const SyncMagicResult &result,
	const std::optional<SyncChatBinding> &binding);

enum class SyncMagicPick { Allowed, NotAlone, Unknown, Unsupported };

[[nodiscard]] SyncMagicPick SyncMagicPickOf(
	PeerToken chat,
	const std::optional<MagicChatCandidate> &local);

class SyncMagicChat final {
public:
	using Done = std::function<void(SyncMagicResult)>;

	explicit SyncMagicChat(std::shared_ptr<SyncMagicPort> port);
	~SyncMagicChat();

	[[nodiscard]] const SyncMagicResult &last() const;
	[[nodiscard]] const std::optional<SyncChatBinding> &binding() const;
	[[nodiscard]] std::optional<PeerToken> hidden() const;
	void setChanged(std::function<void()> changed);

	void lookup(SyncMagicTrigger trigger, bool fresh, Done done = nullptr);
	void choose(PeerToken chat, Done done);
	void useDefault(Done done);
	void joined(Done done);
	void list(bool listed, std::function<void(bool)> done);
	void shown();
	void raise(SyncChatProblem problem);
	void close();
	[[nodiscard]] int64_t waitUntil(const QString &method) const;
	void recordWait(const QString &method, int64_t retryAt);

private:
	using Change = std::function<bool(SyncChatBinding&)>;
	template <typename T>
	using Call = std::function<void(std::function<void(SyncMagicReply<T>)>)>;

	void later(std::function<void()> task);
	[[nodiscard]] bool live(uint64_t run) const;
	void userEdit(const QString &action, Change change, Done done);
	void step(uint64_t run);
	void planned(
		uint64_t run,
		const SyncChatBinding &binding,
		std::optional<SyncMagicLocal> local);
	void proveAbsent(uint64_t run, const SyncChatBinding &binding);
	void page(
		uint64_t run,
		const SyncChatBinding &binding,
		std::vector<uint64_t> seen,
		std::vector<MagicChatCandidate> found,
		int pages);
	void listPartial(uint64_t run);
	void backOff(uint64_t run, const QString &method);
	void verify(
		uint64_t run,
		const SyncChatBinding &binding,
		const SyncChatLookupPlan &plan,
		const SyncChatLookupInput &input);
	void create(uint64_t run, const SyncChatBinding &binding);
	void notCreated();
	void uncertain(uint64_t run, int64_t started);
	void cosmetic(
		uint64_t run,
		const QString &method,
		Call<bool> call,
		SyncChatCreateStep step,
		int64_t started,
		PeerToken chat,
		std::function<void()> next);
	void problem(
		uint64_t run,
		SyncChatProblem problem,
		bool show,
		const std::optional<SyncChatBinding> &parsed,
		const SyncChatLookupInput *input);
	void waiting(uint64_t run, int64_t retryAt, const SyncChatLookupInput &input);
	void done(
		uint64_t run,
		SyncMagicResult result,
		const QString &code,
		const SyncChatLookupInput &input);
	void unavailable(uint64_t run, const QString &why);
	void finish(uint64_t run, SyncMagicResult result);
	void schedule(int64_t retryAt);
	template <typename T>
	void gated(
		uint64_t run,
		const SyncChatBinding &binding,
		const QString &method,
		Call<T> call,
		std::function<void(std::optional<T>)> ok,
		std::function<void(SyncMagicReply<T>)> failed,
		std::function<void()> network = nullptr,
		std::function<void()> beforeFlood = nullptr);
	template <typename T>
	void gatedQuiet(
		uint64_t run,
		const SyncChatBinding &binding,
		const QString &method,
		Call<T> call,
		std::function<void(std::optional<T>)> ok,
		std::function<void()> skipped);
	void hold(
		uint64_t run,
		const QString &method,
		int64_t retryAt,
		std::function<void()> instead);
	std::optional<SyncChatBinding> edit(const Change &change);
	void changed();

	std::shared_ptr<SyncMagicPort> _port;
	std::shared_ptr<int> _guard;
	std::vector<Done> _waiters;
	std::set<QString> _retried;
	SyncMagicResult _last;
	std::optional<SyncChatBinding> _binding;
	std::optional<std::vector<MagicChatCandidate>> _allChats;
	std::function<void()> _changed;
	SyncMagicTrigger _trigger = SyncMagicTrigger::Startup;
	bool _running = false;
	bool _fresh = false;
	bool _closed = false;
	bool _created = false;
	bool _resumed = false;
	bool _partial = false;
	bool _listsWaited = false;
	uint64_t _generation = 0;
	uint64_t _wake = 0;
	int _failures = 0;
	int _errors = 0;

};

} // namespace Purple
