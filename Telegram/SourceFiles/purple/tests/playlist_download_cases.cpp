#include "purple/purple_playlist_download_job.h"
#include "purple/purple_playlist_download_state.h"

#include <iostream>

int main() {
	using namespace Purple;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	const auto id = u"pl-aaaaaaaaaaaaaaaa"_q;
	auto local = PlaylistLocalState();
	local.preserved.insert(u"other_client"_q, QJsonObject{ { u"opaque"_q, 7 } });
	const auto untouched = local;
	auto read = ReadPlaylistDownloadState(local);
	expect(bool(read) && PlaylistKeepsDownloads(read.state, id, true), "absence inherits true default");
	expect(!PlaylistKeepsDownloads(read.state, id, false), "absence inherits false default");
	read.state.preferences[id] = PlaylistKeepDownloaded::Off;
	read.state.preserved.insert(u"future"_q, QJsonObject{ { u"a"_q, 1 } });
	expect(SetPlaylistDownloadState(local, read.state) == PlaylistDownloadStatus::Ready,
		"typed explicit off encodes");
	const auto confirmed = ReadPlaylistDownloadState(local);
	expect(bool(confirmed) && confirmed.state == read.state
		&& local.preserved.value(u"other_client"_q) == untouched.preserved.value(u"other_client"_q),
		"round trip preserves unrelated and nested unknown fields");
	for (const auto value : { QJsonValue(7), QJsonValue(QJsonObject{ { u"format"_q, 2 } }),
		QJsonValue(QJsonObject{ { u"format"_q, 1 }, { u"preferences"_q, QJsonObject() },
			{ u"files"_q, QJsonObject{ { u"1"_q, QJsonObject{ { u"size"_q, u"01"_q } } } } } }) }) {
		auto invalid = local;
		invalid.preserved.insert(u"desktop_playlist_downloads"_q, value);
		const auto before = invalid;
		expect(!ReadPlaylistDownloadState(invalid)
			&& SetPlaylistDownloadState(invalid, {}) != PlaylistDownloadStatus::Ready
			&& invalid == before, "invalid and newer recognized state refuses a rewrite");
	}

	{
		auto pending = confirmed.state;
		const auto token = QString(32, u'a');
		pending.pending[token] = { id, 1, u"mp3"_q, 3, "1:2", QByteArray(64, 'a'),
			QJsonObject{ { u"future_intent"_q, true } } };
		auto target = local;
		expect(SetPlaylistDownloadState(target, pending) == PlaylistDownloadStatus::Ready
			&& ReadPlaylistDownloadState(target).state == pending,
			"durable transfer intent round trip preserves unknown fields");
		expect(!PlaylistDownloadTokenSafe(u"../foreign"_q)
			&& PlaylistDownloadTokenSafe(token), "staging token cannot select arbitrary paths");
	}
	expect(!PlaylistDownloadExtensionSafe(u"../mp3"_q)
		&& !PlaylistDownloadExtensionSafe(u"a/b"_q)
		&& PlaylistDownloadExtensionSafe(u"m4a"_q), "extension never supplies a path");
	auto playlist = Playlist();
	playlist.songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 1, { PlaylistSongValue{ PlaylistSource() }, 1 } } };
	playlist.deleted = PlaylistDeletedRegister{ true, 1 };
	auto library = PlaylistLibrary();
	library.playlists[id] = playlist;
	auto inventory = PlaylistDownloadState();
	inventory.files[1] = { u"mp3"_q, 3, "1:2", QByteArray(64, 'a'), {} };
	expect(PlaylistDownloadRemovalCandidates(library, inventory, std::nullopt).empty(),
		"recently deleted unpurged reference protects file");
	const auto other = u"pl-bbbbbbbbbbbbbbbb"_q;
	library.playlists[other] = playlist;
	expect(PlaylistDownloadRemovalCandidates(library, inventory, id).empty(),
		"remove now cannot delete another unpurged playlist shared file");
	library.purged.insert(other);
	expect(PlaylistDownloadRemovalCandidates(library, inventory, id) == std::vector<uint64_t>{ 1 },
		"only target unpurged reference can be explicitly removed");
	library.purged.insert(id);
	expect(PlaylistDownloadRemovalCandidates(library, inventory, std::nullopt) == std::vector<uint64_t>{ 1 },
		"purge permits inventory pruning");
	auto policy = PlaylistDownloadJobPolicy(3);
	const auto first = policy.admit();
	expect(first.size() == 2 && policy.admit().empty(), "two admissions bound active work");
	expect(!policy.retry(), "retry cannot inherit an active generation");
	expect(policy.finish(first[0], PlaylistDownloadPhase::Busy)
		&& !policy.finish(first[0], PlaylistDownloadPhase::Downloaded), "duplicate completion cannot count");
	const auto third = policy.admit();
	expect(third.size() == 1, "free slot admits next row");
	expect(policy.finish(first[1], PlaylistDownloadPhase::Unavailable)
		&& policy.finish(third[0], PlaylistDownloadPhase::Downloaded), "terminal results retain meaning");
	expect(policy.retry(), "busy row permits explicit retry");
	const auto retried = policy.admit();
	expect(retried.size() == 1 && retried[0].generation != first[0].generation
		&& !policy.finish(first[0], PlaylistDownloadPhase::Downloaded), "retry rejects stale ticket");
	policy.cancel();
	expect(!policy.finish(retried[0], PlaylistDownloadPhase::Downloaded) && policy.admit().empty(),
		"retirement prevents pending completion and admission");
	expect(policy.progress().terminal && policy.progress().cancelled
		&& policy.progress().problem == PlaylistDownloadProblem::Cancelled,
		"retired policy exposes terminal cancellation rather than a live starting state");
	return failures ? 1 : 0;
}
