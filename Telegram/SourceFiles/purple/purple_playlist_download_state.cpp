#include "purple/purple_playlist_download_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Purple {
namespace {

const auto Key = u"desktop_playlist_downloads"_q;

std::optional<uint64_t> Decimal(const QJsonValue &value) {
	if (!value.isString()) return std::nullopt;
	const auto text = value.toString();
	auto ok = false;
	const auto result = text.toULongLong(&ok);
	return ok && result && QString::number(result) == text
		&& result <= uint64_t(std::numeric_limits<qint64>::max())
		? std::optional<uint64_t>(result) : std::nullopt;
}

bool FileIdentity(const QByteArray &value) {
	if (value.isEmpty() || value.size() > 256 || value.front() == ':'
		|| value.back() == ':' || value.contains("::")) return false;
	const auto separators = value.count(':');
	return (separators == 1 || separators == 2)
		&& std::all_of(begin(value), end(value), [](char ch) {
			return ch == ':' || (ch >= '0' && ch <= '9');
		});
}

bool Digest(const QByteArray &value) {
	return value.size() == 64 && std::all_of(begin(value), end(value), [](char ch) {
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
	});
}

} // namespace

bool PlaylistDownloadTokenSafe(const QString &token) {
	return token.size() == 32
		&& std::all_of(begin(token), end(token), [](QChar ch) {
			return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
		});
}

bool PlaylistDownloadExtensionSafe(const QString &extension) {
	return !extension.isEmpty() && extension.size() <= 12
		&& std::all_of(begin(extension), end(extension), [](QChar ch) {
			return (ch >= u'a' && ch <= u'z') || (ch >= u'0' && ch <= u'9');
		});
}

PlaylistDownloadRead ReadPlaylistDownloadState(const PlaylistLocalState &local) {
	const auto value = local.preserved.value(Key);
	if (value.isUndefined()) return { PlaylistDownloadStatus::Ready, {} };
	if (!value.isObject()) return {};
	const auto object = value.toObject();
	const auto version = object.value(u"format"_q);
	if (!version.isDouble() || version.toDouble() < 1
		|| std::floor(version.toDouble()) != version.toDouble()) return {};
	if (version.toDouble() > 1) return { PlaylistDownloadStatus::Newer, {} };
	const auto prefs = object.value(u"preferences"_q);
	const auto files = object.value(u"files"_q);
	if (!prefs.isObject() || !files.isObject()) return {};
	auto result = PlaylistDownloadRead{ PlaylistDownloadStatus::Ready, {} };
	result.state.preserved = object;
	result.state.preserved.remove(u"format"_q);
	result.state.preserved.remove(u"preferences"_q);
	result.state.preserved.remove(u"files"_q);
	result.state.preserved.remove(u"pending"_q);
	const auto preferences = prefs.toObject();
	for (auto i = preferences.begin(); i != preferences.end(); ++i) {
		if (!IsPlaylistId(i.key()) || !i.value().isString()) return {};
		const auto text = i.value().toString();
		if (text != u"on"_q && text != u"off"_q) return {};
		result.state.preferences.emplace(i.key(), text == u"on"_q
			? PlaylistKeepDownloaded::On : PlaylistKeepDownloaded::Off);
	}
	const auto inventory = files.toObject();
	for (auto i = inventory.begin(); i != inventory.end(); ++i) {
		const auto id = ParsePlaylistDocumentId(i.key());
		if (!id || !i.value().isObject()) return {};
		const auto entry = i.value().toObject();
		const auto size = Decimal(entry.value(u"size"_q));
		const auto extension = entry.value(u"extension"_q).toString();
		const auto identity = entry.value(u"identity"_q).toString().toLatin1();
		const auto hash = entry.value(u"sha256"_q).toString().toLatin1();
		if (!size || !PlaylistDownloadExtensionSafe(extension)
			|| !FileIdentity(identity)
			|| !Digest(hash)) return {};
		auto preserved = entry;
		for (const auto key : { u"extension"_q, u"size"_q, u"identity"_q, u"sha256"_q }) {
			preserved.remove(key);
		}
		result.state.files.emplace(*id, PlaylistKeptFile{
			extension, *size, identity, hash, preserved });
	}
	const auto pending = object.value(u"pending"_q);
	if (!pending.isUndefined() && !pending.isObject()) return {};
	const auto intents = pending.toObject();
	auto pendingDocuments = std::set<uint64_t>();
	for (auto i = intents.begin(); i != intents.end(); ++i) {
		if (!PlaylistDownloadTokenSafe(i.key()) || !i.value().isObject()) return {};
		const auto entry = i.value().toObject();
		const auto playlistId = entry.value(u"playlist"_q).toString();
		const auto id = ParsePlaylistDocumentId(entry.value(u"document"_q).toString());
		const auto extension = entry.value(u"extension"_q).toString();
		const auto size = Decimal(entry.value(u"size"_q));
		const auto identityValue = entry.value(u"identity"_q);
		const auto identity = identityValue.toString().toLatin1();
		const auto digestValue = entry.value(u"sha256"_q);
		const auto digest = digestValue.toString().toLatin1();
		if (!IsPlaylistId(playlistId) || !id || !size
			|| !PlaylistDownloadExtensionSafe(extension) || !identityValue.isString()
			|| (!identity.isEmpty() && !FileIdentity(identity))
			|| !digestValue.isString() || (!digest.isEmpty() && !Digest(digest))
			|| (identity.isEmpty() && !digest.isEmpty())
			|| !pendingDocuments.insert(*id).second) return {};
		auto preserved = entry;
		for (const auto key : { u"playlist"_q, u"document"_q, u"extension"_q,
			u"size"_q, u"identity"_q, u"sha256"_q }) preserved.remove(key);
		result.state.pending.emplace(i.key(), PlaylistOwnedDownloadIntent{
			playlistId, *id, extension, *size, identity, digest, preserved });
	}
	return result;
}

PlaylistDownloadStatus SetPlaylistDownloadState(
		PlaylistLocalState &local,
		const PlaylistDownloadState &state) {
	const auto old = ReadPlaylistDownloadState(local);
	if (!old) return old.status;
	auto object = state.preserved;
	object.insert(u"format"_q, 1);
	auto preferences = QJsonObject();
	for (const auto &[id, preference] : state.preferences) {
		if (!IsPlaylistId(id)) return PlaylistDownloadStatus::Invalid;
		switch (preference) {
		case PlaylistKeepDownloaded::Inherit: break;
		case PlaylistKeepDownloaded::On: preferences.insert(id, u"on"_q); break;
		case PlaylistKeepDownloaded::Off: preferences.insert(id, u"off"_q); break;
		default: return PlaylistDownloadStatus::Invalid;
		}
	}
	object.insert(u"preferences"_q, preferences);
	auto files = QJsonObject();
	for (const auto &[id, file] : state.files) {
		const auto key = FormatPlaylistDocumentId(id);
		if (!key) return PlaylistDownloadStatus::Invalid;
		auto entry = file.preserved;
		entry.insert(u"extension"_q, file.extension);
		entry.insert(u"size"_q, QString::number(file.size));
		entry.insert(u"identity"_q, QString::fromLatin1(file.identity));
		entry.insert(u"sha256"_q, QString::fromLatin1(file.sha256));
		files.insert(*key, entry);
	}
	object.insert(u"files"_q, files);
	auto pending = QJsonObject();
	for (const auto &[token, intent] : state.pending) {
		auto entry = intent.preserved;
		entry.insert(u"playlist"_q, intent.playlistId);
		entry.insert(u"document"_q, QString::number(intent.documentId));
		entry.insert(u"extension"_q, intent.extension);
		entry.insert(u"size"_q, QString::number(intent.size));
		entry.insert(u"identity"_q, QString::fromLatin1(intent.identity));
		entry.insert(u"sha256"_q, QString::fromLatin1(intent.sha256));
		pending.insert(token, entry);
	}
	if (!pending.isEmpty()) object.insert(u"pending"_q, pending);
	else object.remove(u"pending"_q);
	auto candidate = local;
	candidate.preserved.insert(Key, object);
	if (!ReadPlaylistDownloadState(candidate)) return PlaylistDownloadStatus::Invalid;
	local = std::move(candidate);
	return PlaylistDownloadStatus::Ready;
}

bool PlaylistKeepsDownloads(
		const PlaylistDownloadState &state,
		const QString &playlistId,
		bool appWideDefault) {
	const auto i = state.preferences.find(playlistId);
	return i == end(state.preferences) || i->second == PlaylistKeepDownloaded::Inherit
		? appWideDefault : i->second == PlaylistKeepDownloaded::On;
}

std::set<uint64_t> PlaylistDownloadReferences(
		const PlaylistLibrary &library,
		const QString &excluding) {
	auto result = std::set<uint64_t>();
	for (const auto &[id, playlist] : library.playlists) {
		if (id == excluding || library.purged.contains(id) || !playlist.songs) continue;
		for (const auto &[documentId, song] : *playlist.songs) {
			if (!song.value.isRemoved()) result.insert(documentId);
		}
	}
	return result;
}

std::vector<uint64_t> PlaylistDownloadRemovalCandidates(
		const PlaylistLibrary &library,
		const PlaylistDownloadState &state,
		const std::optional<QString> &removeFromPlaylist) {
	for (const auto &[id, playlist] : library.playlists) {
		if (!library.purged.contains(id) && !playlist.songs) return {};
	}
	const auto protectedIds = PlaylistDownloadReferences(
		library, removeFromPlaylist.value_or(QString()));
	auto targets = std::set<uint64_t>();
	if (removeFromPlaylist) {
		const auto i = library.playlists.find(*removeFromPlaylist);
		if (i == end(library.playlists) || !i->second.songs) return {};
		for (const auto &[id, song] : *i->second.songs) {
			if (!song.value.isRemoved()) targets.insert(id);
		}
	}
	auto result = std::vector<uint64_t>();
	for (const auto &[id, file] : state.files) {
		if (!protectedIds.contains(id)
			&& (!removeFromPlaylist || targets.contains(id))) result.push_back(id);
	}
	return result;
}

} // namespace Purple
