#include "purple/purple_sync_merge.h"
#include "purple/purple_types.h"

#include <cstdlib>
#include <iostream>

namespace {

using namespace Purple;
using Choice = SyncMergeChoice;
using Kind = SyncMergeDifference::Kind;
auto checks = 0;

template <typename T>
using not_null = T;

enum class SyncStoreStatus { Ready, IoError };
enum class SyncChatBindingMode { Magic, Manual };
enum class SyncChatProblem { None, Unavailable };

struct Binding {
	SyncChatBindingMode mode = SyncChatBindingMode::Magic;
	std::optional<int> chat = 1;
	SyncChatProblem problem = SyncChatProblem::None;
	int verifiedAt = 1;
};
struct Chat {
	std::optional<Binding> value = Binding();
	const std::optional<Binding> &binding() const { return value; }
};
struct Snapshot {
	SyncStoreStatus status = SyncStoreStatus::Ready;
	struct {
		bool statePresent = false;
		bool pendingPresent = false;
	} store;
};

namespace Main {
struct Session {
	Chat chat;
	std::optional<int> bound = 1;
	bool routable = true;
	QByteArray token;
	Snapshot snapshot;
	Session &account() { return *this; }
};
} // namespace Main

struct InspectionTarget {
	Main::Session *files;
};
Chat *SyncMagicChatOf(Main::Session *session) { return &session->chat; }
std::optional<int> SyncMagicBound(Main::Session *session) { return session->bound; }
std::optional<InspectionTarget> AccountSyncInspectionTarget(Main::Session *session) {
	return session->routable ? std::make_optional(InspectionTarget{ session }) : std::nullopt;
}
QByteArray AccountSyncBindingToken(Main::Session &session) { return session.token; }
int SessionAccountKey(Main::Session *) { return 1; }
Snapshot ReadSyncRecoveryStore(Main::Session *session, int) { return session->snapshot; }

#include "sync-settings-link.inc"

void Check(bool passed, const char *description) {
	++checks;
	if (!passed) {
		std::cerr << description << '\n';
		std::exit(1);
	}
}

void MergeTablesAndLists() {
	const auto local = QByteArray(
		"version = 2\n"
		"[lists.team]\n"
		"members = [12, 34, 34, 99]\n"
		"tags = ['shared', 'local']\n"
		"local_only = true\n"
		"label = 'Local team'\n"
		"[lists.unchanged]\n"
		"members = ['@common']\n");
	const auto remote = QByteArray(
		"version = 2\n"
		"[lists.team]\n"
		"members = [34, 12, 56]\n"
		"tags = ['shared', 'remote']\n"
		"label = 'Remote team'\n"
		"[lists.unchanged]\n"
		"members = ['@common']\n"
		"[lists.remote_list]\n"
		"members = ['@other']\n");
	const auto review = ReviewSyncMerge(local, remote);
	Check(bool(review), "review related tables without writing");
	Check(review->differences.size() == 9, "enumerate all leaves, added members and order");
	auto choices = std::vector<Choice>();
	for (const auto &row : review->differences) {
		if (row.kind == Kind::Order || row.kind == Kind::Conflict) {
			choices.push_back(Choice::Remote);
		} else if (row.path == QStringLiteral("lists.team.local_only")
			|| row.local == QStringLiteral("99")) {
			choices.push_back(Choice::Drop);
		} else {
			choices.push_back(Choice::Keep);
		}
	}
	const auto result = BuildSyncMerge(*review, choices);
	Check(result && result->contains("members = [ 34, 12, 56, 34 ]"), "remote order with retained local duplicate");
	Check(result->contains("[lists.remote_list]"), "union adds the remote table");
	Check(result->contains("@common"), "shared table survives");
	Check(result->contains("Remote team") && !result->contains("Local team"), "select a conflicting scalar");
	Check(result->contains("shared") && result->contains("local") && result->contains("remote"), "union of string items");
	Check(!result->contains("local_only") && !result->contains("99"), "drop a local key and member explicitly");
	choices.pop_back();
	Check(!BuildSyncMerge(*review, choices), "refuse missing choices");
}

void MergeConflicts() {
	const auto review = ReviewSyncMerge(
		QByteArray("value = [true, false]\nother = 1\n"),
		QByteArray("value = [false]\n[other]\nlabel = 'conflict'\n"));
	Check(review && review->differences.size() == 2, "show other arrays and incompatible types as whole values");
	Check(!BuildSyncMerge(*review, { Choice::Unset, Choice::Local }), "conflict requires a decision");
	const auto result = BuildSyncMerge(*review, { Choice::Remote, Choice::Drop });
	Check(result && result->contains("[other]") && !result->contains("value"), "select table and drop array");
	Check(!ReviewSyncMerge("version = 1\na = 1", "version = 2\na = 2"), "refuse mixed schema merge");
	Check(!ReviewSyncMerge("version = true\na = 1", "a = 2"), "refuse invalid schema metadata");
	Check(!ReviewSyncMerge("a = [", "a = 1"), "refuse invalid TOML");
	Check(!ReviewSyncMerge("a = 1", "# comment\na = 1"), "identical settings need no merge");
	const auto quoted = ReviewSyncMerge("[lists.\"Alpha.[]\"]\n", "[lists.\"Alpha.[]\"]\nlabel = 'remote'");
	Check(quoted && quoted->differences.front().path.startsWith(QStringLiteral("lists["))
		&& quoted->differences.front().path.contains(QStringLiteral("Alpha.[]"))
		&& quoted->differences.front().path.endsWith(QStringLiteral("].label")),
		"unambiguous path for a quoted table key");
	auto tooMany = QByteArray();
	for (auto i = 0; i != 513; ++i) {
		tooMany += "key" + QByteArray::number(i) + " = true\n";
	}
	Check(!ReviewSyncMerge("a = 1", tooMany), "refuse reviews exceeding the complete diff limit");
}

void LinkAdmission() {
	auto fresh = Main::Session();
	Check(SyncMagicSettingsUnlinked(&fresh), "fresh Magic cache without state needs a Join action");
	fresh.snapshot.store.statePresent = true;
	Check(!SyncMagicSettingsUnlinked(&fresh), "existing state needs the usual sync status");
	fresh.snapshot.store.statePresent = false;
	fresh.token = "old binding";
	Check(!SyncMagicSettingsUnlinked(&fresh), "existing token needs recovery");
	fresh.token.clear();
	fresh.snapshot.store.pendingPresent = true;
	Check(!SyncMagicSettingsUnlinked(&fresh), "orphan pending record needs recovery");
	fresh.snapshot.store.pendingPresent = false;
	fresh.snapshot.status = SyncStoreStatus::IoError;
	Check(!SyncMagicSettingsUnlinked(&fresh), "unreadable store is not positively absent");
	fresh.snapshot.status = SyncStoreStatus::Ready;
	fresh.routable = false;
	Check(!SyncMagicSettingsUnlinked(&fresh), "no settings route means no Join action");
	fresh.routable = true;
	fresh.bound = std::nullopt;
	Check(SyncMagicSettingsUnlinked(&fresh), "verified cached choice stays visible while offline");
	fresh.chat.value->verifiedAt = 0;
	Check(!SyncMagicSettingsUnlinked(&fresh), "unverified chat cache means no Join action");
	fresh.bound = 2;
	Check(!SyncMagicSettingsUnlinked(&fresh), "cached chat must match the resolved route");
	fresh.bound = 1;
	fresh.chat.value->problem = SyncChatProblem::Unavailable;
	Check(!SyncMagicSettingsUnlinked(&fresh), "a chat problem retains its own status");
	fresh.chat.value->problem = SyncChatProblem::None;
	fresh.chat.value->mode = SyncChatBindingMode::Manual;
	Check(!SyncMagicSettingsUnlinked(&fresh), "the warning belongs to discovered Magic chats");
}

} // namespace

int main() {
	MergeTablesAndLists();
	MergeConflicts();
	LinkAdmission();
	std::cout << checks << " checks, 0 failures\n";
}
