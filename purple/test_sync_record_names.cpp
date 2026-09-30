/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <QtCore/QString>

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

template <typename Signature>
using Fn = std::function<Signature>;

template <typename T>
using not_null = T;

struct MsgId {
};

#include "purple/purple_sync.h"

namespace {

auto checks = 0;
auto failures = 0;

void Check(bool ok, int line) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("FAIL line %d\n", line);
	}
}

#define CHECK(value) Check(bool(value), __LINE__)

[[nodiscard]] bool CoreCandidateByName(const QString &name) {
	auto meta = Purple::SyncHistoryMessageMeta();
	meta.isMessage = true;
	meta.isDocument = true;
	meta.fileNames = { name };
	return Purple::IsSyncHistoryCandidate(meta);
}

} // namespace

int main() {
	using Purple::IsSyncRecordFileName;

	CHECK(IsSyncRecordFileName(Purple::SyncSettingsRecordFileName()));
	CHECK(IsSyncRecordFileName(Purple::SyncPlaylistsRecordFileName()));
	CHECK(IsSyncRecordFileName(u"Purple settings sync.json"_q));
	CHECK(IsSyncRecordFileName(u"Purple playlists sync.json"_q));

	const auto nearMisses = {
		u"Purple settings sync (2).json"_q,
		u"Purple playlists sync (3).json"_q,
		u"purple settings sync.json"_q,
		u"Purple Settings Sync.json"_q,
		u"Purple settings sync.JSON"_q,
		u"Purple settings sync.json "_q,
		u" Purple settings sync.json"_q,
		u"Purple settings sync"_q,
		u"Purple settings sync.json.txt"_q,
		u"settings.toml"_q,
		u"settings (2).toml"_q,
		QString(),
	};
	for (const auto &name : nearMisses) {
		CHECK(!IsSyncRecordFileName(name));
	}

	const auto all = {
		Purple::SyncSettingsRecordFileName(),
		Purple::SyncPlaylistsRecordFileName(),
		u"Purple settings sync (2).json"_q,
		u"purple settings sync.json"_q,
		u"settings.toml"_q,
		QString(),
	};
	for (const auto &name : all) {
		CHECK(IsSyncRecordFileName(name) == CoreCandidateByName(name));
	}

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
