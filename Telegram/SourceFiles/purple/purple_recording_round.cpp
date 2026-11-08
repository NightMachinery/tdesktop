/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_round.h"

#include "purple/hooks/media_recording_capture.h"
#include "purple/purple_config.h"
#include "purple/purple_recording_mirror.h"
#include "purple/purple_recording_round_media.h"
#include "purple/purple_recording_store.h"

#include "crl/crl_on_main.h"

#include <QtCore/QTimer>

#include <atomic>
#include <map>
#include <mutex>

namespace Purple {
namespace {

using Kind = RecordingRegistry::Kind;
using Closed = RecordingMirror::Closed;

constexpr auto kCloseDelay = 3000;

struct Owner {
	std::vector<Closed> closedParts;
	std::string activeId;
	std::string group;
	std::unique_ptr<RoundMedia::CopyMuxer> active;
	bool closing = false;
};

std::atomic<int> JournalSeconds = 1;

[[nodiscard]] std::mutex &Mutex() {
	static auto result = std::mutex();
	return result;
}

[[nodiscard]] std::map<const void*, Owner> &Owners() {
	static auto result = std::map<const void*, Owner>();
	return result;
}

void CloseActive(Owner &owner) {
	if (!owner.active) {
		return;
	}
	const auto finished = owner.active->finish();
	owner.closedParts.push_back({
		.id = owner.activeId,
		.size = owner.active->size(),
		.failed = !finished,
		.empty = finished && owner.active->empty(),
	});
	owner.active = nullptr;
	owner.activeId = std::string();
}

void Report(std::vector<Closed> closed) {
	if (closed.empty()) {
		return;
	}
	crl::on_main([closed = std::move(closed)]() mutable {
		FinishRecordingFiles(std::move(closed));
	});
}

} // namespace

void StartRoundCopies() {
	static auto lifetime = rpl::lifetime();
	rpl::single(rpl::empty) | rpl::then(
		SettingsChanges()
	) | rpl::on_next([] {
		JournalSeconds = ActiveSettings().recording.roundJournalSeconds;
	}, lifetime);
}

void OnRoundEncodingStarted(const void *owner, AVFormatContext *format) {
	if (!owner || !format) {
		return;
	}
	const auto journalSeconds = JournalSeconds.load();
	const auto config = RoundMedia::ConfigOf(format);
	auto group = std::string();
	{
		const auto lock = std::lock_guard(Mutex());
		const auto i = Owners().find(owner);
		if (i != Owners().end() && !i->second.closing) {
			group = i->second.group;
		}
	}
	const auto id = config
		? BeginRecordingEntry(
			Kind::Round,
			".mp4.prc",
			journalSeconds > 0 ? std::string() : std::string(".pjr.prc"),
			group)
		: std::nullopt;
	auto muxer = std::unique_ptr<RoundMedia::CopyMuxer>();
	if (id) {
		const auto key = RecordingKey();
		const auto random = RecordingRandom();
		if (journalSeconds <= 0) {
			const auto header = EncodeRoundJournalStart(*config);
			const auto journal = RecordingCrypt::File::Create(
				RecordingPath(*id + ".pjr.prc"),
				key,
				random);
			if (!journal || !journal->writeAt(0, header.data(), header.size())) {
				LOG(("Purple Recording: could not write a round config header."));
			}
		}
		muxer = RoundMedia::CopyMuxer::Start(
			RecordingCrypt::File::Create(
				RecordingPath(*id + ".mp4.prc"),
				key,
				random),
			format,
			journalSeconds);
		if (!muxer) {
			LOG(("Purple Recording: could not start a round copy."));
			ForgetRecordingEntry(*id);
		}
	}
	auto report = std::vector<Closed>();
	{
		const auto lock = std::lock_guard(Mutex());
		auto &entry = Owners()[owner];
		CloseActive(entry);
		if (entry.closing) {
			report = base::take(entry.closedParts);
			entry.closing = false;
			entry.group = std::string();
		}
		if (entry.group.empty() && id) {
			entry.group = *id;
		}
		if (muxer) {
			entry.active = std::move(muxer);
			entry.activeId = *id;
		}
	}
	Report(std::move(report));
}

void OnRoundPacket(const void *owner, const AVPacket *packet) {
	const auto lock = std::lock_guard(Mutex());
	const auto i = Owners().find(owner);
	if (i == Owners().end() || !i->second.active) {
		return;
	}
	auto &active = i->second.active;
	if (!active->failed() && !active->write(packet) && active->failed()) {
		LOG(("Purple Recording: a round copy write failed, copying stopped."));
	}
}

std::vector<std::string> RoundCopyIds() {
	const auto lock = std::lock_guard(Mutex());
	auto result = std::vector<std::string>();
	for (const auto &[owner, entry] : Owners()) {
		if (entry.closing) {
			continue;
		}
		for (const auto &part : entry.closedParts) {
			result.push_back(part.id);
		}
		if (entry.active) {
			result.push_back(entry.activeId);
		}
	}
	return result;
}

std::vector<std::string> RequestRoundCopiesClose() {
	const auto lock = std::lock_guard(Mutex());
	auto result = std::vector<std::string>();
	for (auto &[owner, entry] : Owners()) {
		if (entry.closing) {
			continue;
		}
		for (const auto &part : entry.closedParts) {
			result.push_back(part.id);
		}
		if (entry.active) {
			result.push_back(entry.activeId);
		}
		entry.closing = true;
	}
	return result;
}

void FinishRoundCopiesLater() {
	QTimer::singleShot(kCloseDelay, [] {
		auto report = std::vector<Closed>();
		{
			const auto lock = std::lock_guard(Mutex());
			auto &owners = Owners();
			for (auto i = owners.begin(); i != owners.end();) {
				if (!i->second.closing) {
					++i;
					continue;
				}
				CloseActive(i->second);
				for (auto &part : i->second.closedParts) {
					report.push_back(std::move(part));
				}
				i = owners.erase(i);
			}
		}
		FinishRecordingFiles(std::move(report));
	});
}

} // namespace Purple
