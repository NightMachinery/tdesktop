#include "purple/purple_playlist_folders.h"

#include <QtCore/QJsonValue>
#include <algorithm>
#include <array>
#include <set>

namespace Purple {
namespace {

const auto kStateKey = u"desktop_playlist_folders"_q;
constexpr auto kFormat = 1;

QString NormalizeName(const QString &name) {
	return name.trimmed().normalized(QString::NormalizationForm_C);
}

bool ValidFolderName(const QString &id, const QString &name) {
	return IsPlaylistId(id) && IsValidPlaylistTitle(name)
		&& name == NormalizeName(name) && name.toCaseFolded() != HyperFolderId();
}

bool KnownFolder(const PlaylistFolders &folders, const QString &id) {
	return id == HyperFolderId() || folders.names.contains(id);
}

bool LivePlaylist(const PlaylistLibrary &library, const QString &id) {
	const auto i = library.playlists.find(id);
	return i != library.playlists.end() && !i->second.isPartial()
		&& !i->second.deleted->value && !library.purged.contains(id);
}

bool MatchesSongs(
		const Playlist &playlist,
		const std::vector<PlaylistAddSongInput> &songs,
		uint64_t selfUserId) {
	const auto ids = PlaylistVisibleSongIds(playlist);
	if (ids.size() != songs.size()) return false;
	for (auto i = size_t(0); i != ids.size(); ++i) {
		const auto &entry = songs[i];
		if (ids[i] != entry.documentId) return false;
		const auto &source = playlist.songs->at(ids[i]).value.source;
		const auto peer = (entry.peer.kind == PeerTokenKind::User
			&& entry.peer.id == selfUserId)
			? PeerToken{ PeerTokenKind::Self, 0 } : entry.peer;
		if (!source || source->peer != peer || source->msg != entry.msg) {
			return false;
		}
	}
	return true;
}

QString GenerateHyperName(
		const PlaylistFolders &folders,
		const PlaylistLibrary &library,
		const QByteArray &entropy) {
	const auto qualities = std::array{
		u"Amber"_q, u"Brisk"_q, u"Curious"_q, u"Dappled"_q, u"Evening"_q, u"Fuzzy"_q,
		u"Golden"_q, u"Hidden"_q, u"Ivory"_q, u"Lively"_q, u"Misty"_q, u"Quiet"_q,
		u"Silver"_q, u"Sleepy"_q, u"Velvet"_q, u"Wandering"_q,
	};
	const auto subjects = std::array{
		u"Badger"_q, u"Birch"_q, u"Cloud"_q, u"Comet"_q, u"Finch"_q, u"Harbor"_q,
		u"Hedgehog"_q, u"Kite"_q, u"Lantern"_q, u"Meadow"_q, u"Moth"_q, u"Orchard"_q,
		u"Otter"_q, u"Pebble"_q, u"Sparrow"_q, u"Willow"_q,
	};
	const auto total = qualities.size() * subjects.size();
	const auto first = ((uint16_t(uchar(entropy.at(0))) << 8)
		| uchar(entropy.at(1))) % total;
	auto used = std::set<QString>();
	for (const auto &[id, folder] : folders.placements) {
		if (folder == HyperFolderId() && LivePlaylist(library, id)) {
			used.emplace(library.playlists.at(id).title->value.toCaseFolded());
		}
	}
	for (auto step = size_t(0); step <= used.size(); ++step) {
		const auto index = (first + step) % total;
		auto name = qualities[index / subjects.size()]
			+ u' ' + subjects[index % subjects.size()];
		if (step >= total) name += u' ' + QString::number(qulonglong(1 + step / total));
		if (!used.contains(name.toCaseFolded())) return name;
	}
	return {};
}

} // namespace

QString HyperFolderId() {
	return u"hyper"_q;
}

std::optional<PlaylistFolders> ReadPlaylistFolders(const PlaylistLocalState &state) {
	if (!state.preserved.contains(kStateKey)) return PlaylistFolders();
	const auto data = state.preserved.value(kStateKey).toObject();
	const auto names = data.value(u"names"_q);
	const auto placements = data.value(u"placements"_q);
	if (data.size() != 3 || data.value(u"format"_q).toInt() != kFormat
		|| !names.isObject() || !placements.isObject()) {
		return std::nullopt;
	}
	auto result = PlaylistFolders();
	auto uniqueNames = std::set<QString>();
	const auto nameObject = names.toObject();
	for (auto i = nameObject.begin(); i != nameObject.end(); ++i) {
		const auto name = i.value().toString();
		if (!ValidFolderName(i.key(), name)
			|| !uniqueNames.emplace(name.toCaseFolded()).second) {
			return std::nullopt;
		}
		result.names.emplace(i.key(), name);
	}
	const auto placementObject = placements.toObject();
	for (auto i = placementObject.begin(); i != placementObject.end(); ++i) {
		const auto folder = i.value().toString();
		if (!IsPlaylistId(i.key()) || !KnownFolder(result, folder)) return std::nullopt;
		result.placements.emplace(i.key(), folder);
	}
	return result;
}

bool WritePlaylistFolders(PlaylistLocalState &state, const PlaylistFolders &folders) {
	if (!ReadPlaylistFolders(state)) return false;
	auto names = QJsonObject();
	auto placements = QJsonObject();
	auto uniqueNames = std::set<QString>();
	for (const auto &[id, name] : folders.names) {
		if (!ValidFolderName(id, name)
			|| !uniqueNames.emplace(name.toCaseFolded()).second) {
			return false;
		}
		names.insert(id, name);
	}
	for (const auto &[id, parent] : folders.placements) {
		if (!IsPlaylistId(id) || !KnownFolder(folders, parent)) return false;
		placements.insert(id, parent);
	}
	state.preserved.insert(kStateKey, QJsonObject{
		{ u"format"_q, kFormat },
		{ u"names"_q, names },
		{ u"placements"_q, placements },
	});
	return true;
}

bool SetPlaylistFolderName(PlaylistFolders &folders, const QString &id, const QString &name) {
	const auto normalized = NormalizeName(name);
	if (!ValidFolderName(id, normalized)) return false;
	const auto folded = normalized.toCaseFolded();
	if (std::ranges::any_of(folders.names, [&](const auto &entry) {
		return entry.first != id && entry.second.toCaseFolded() == folded;
	})) {
		return false;
	}
	folders.names.insert_or_assign(id, normalized);
	return true;
}

bool RemovePlaylistFolder(PlaylistFolders &folders, const QString &id) {
	if (!folders.names.erase(id)) return false;
	std::erase_if(folders.placements, [&](const auto &entry) { return entry.second == id; });
	return true;
}

bool MovePlaylistToFolder(
		PlaylistFolders &folders,
		const PlaylistLibrary &library,
		const QString &playlistId,
		const QString &folderId) {
	if (!LivePlaylist(library, playlistId)
		|| (!folderId.isEmpty() && !folders.names.contains(folderId))) {
		return false;
	}
	if (folderId.isEmpty()) {
		folders.placements.erase(playlistId);
	} else {
		folders.placements.insert_or_assign(playlistId, folderId);
	}
	return true;
}

std::optional<HyperPlaylist> PrepareHyperPlaylist(
		const PlaylistLibrary &library,
		const PlaylistFolders &folders,
		const std::vector<PlaylistAddSongInput> &songs,
		const QByteArray &entropy,
		uint64_t nowMs,
		uint64_t selfUserId) {
	if (songs.size() < 2 || !FormatPlaylistId(entropy) || !IsValidPlaylistStamp(nowMs)) {
		return std::nullopt;
	}
	const auto hyper = HyperFolderId();
	for (const auto &[id, folder] : folders.placements) {
		if (folder == hyper && LivePlaylist(library, id)
			&& MatchesSongs(library.playlists.at(id), songs, selfUserId)) {
			return HyperPlaylist{
				.edit = { .library = library, .playlistId = id },
				.folders = folders,
			};
		}
	}
	const auto name = GenerateHyperName(folders, library, entropy);
	const auto created = CreatePlaylist(library, name, entropy, nowMs, selfUserId);
	if (!created || !created.changed) return std::nullopt;
	auto populated = AddPlaylistSongs(
		created.library, created.playlistId, songs, nowMs, selfUserId);
	if (!populated || !populated.changed
		|| !MatchesSongs(
			populated.library.playlists.at(created.playlistId), songs, selfUserId)) {
		return std::nullopt;
	}
	auto placement = folders;
	placement.placements.emplace(created.playlistId, hyper);
	return HyperPlaylist{ std::move(populated), std::move(placement) };
}

} // namespace Purple
