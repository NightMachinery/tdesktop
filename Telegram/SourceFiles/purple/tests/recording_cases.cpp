#include "purple/purple_ogg_repair.h"
#include "purple/purple_recording_crypt.h"
#include "purple/purple_recording_mirror.h"
#include "purple/purple_recording_recovery.h"
#include "purple/purple_recording_registry.h"
#include "purple/purple_seek_rules.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] Bytes OggPage(
		std::uint8_t flags,
		std::int64_t granule,
		std::uint32_t sequence,
		const Bytes &body) {
	auto page = Bytes{ 'O', 'g', 'g', 'S', 0, flags };
	for (auto i = 0; i != 8; ++i) {
		page.push_back(std::uint8_t(std::uint64_t(granule) >> (8 * i)));
	}
	for (const auto value : { 0x5150u, sequence, 0u }) {
		for (auto i = 0; i != 4; ++i) {
			page.push_back(std::uint8_t(value >> (8 * i)));
		}
	}
	auto lacing = Bytes();
	auto left = body.size();
	do {
		const auto step = std::min<std::size_t>(left, 255);
		lacing.push_back(std::uint8_t(step));
		left -= step;
		if (step < 255) {
			break;
		}
	} while (true);
	page.push_back(std::uint8_t(lacing.size()));
	page.insert(page.end(), lacing.begin(), lacing.end());
	page.insert(page.end(), body.begin(), body.end());
	const auto crc = Purple::OggCrc(page.data(), page.size());
	for (auto i = 0; i != 4; ++i) {
		page[22 + i] = std::uint8_t(crc >> (8 * i));
	}
	return page;
}

[[nodiscard]] Bytes OpusStream(int audioPages, bool finished) {
	auto head = Bytes{ 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0 };
	auto tags = Bytes{ 'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0 };
	auto result = OggPage(0x02, 0, 0, head);
	const auto add = [&](const Bytes &page) {
		result.insert(result.end(), page.begin(), page.end());
	};
	add(OggPage(0, 0, 1, tags));
	for (auto i = 0; i != audioPages; ++i) {
		const auto last = finished && (i + 1 == audioPages);
		add(OggPage(last ? 0x04 : 0, 312 + 48'000 * (i + 1), 2 + i, Bytes(40, std::uint8_t(i + 1))));
	}
	return result;
}

[[nodiscard]] std::shared_ptr<const Purple::RecordingCrypt::Key> TestKey(std::uint8_t seed) {
	auto key = std::make_shared<Purple::RecordingCrypt::Key>();
	for (auto i = 0; i != int(key->bytes.size()); ++i) {
		key->bytes[i] = std::uint8_t(seed + i * 7);
	}
	for (auto i = 0; i != int(key->check.size()); ++i) {
		key->check[i] = std::uint8_t(seed ^ (i * 13));
	}
	return key;
}

[[nodiscard]] bool Random(std::uint8_t *data, std::size_t size) {
	return RAND_bytes(data, int(size)) == 1;
}

[[nodiscard]] Bytes FileBytes(const std::filesystem::path &path) {
	auto file = std::ifstream(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(file), {});
}

[[nodiscard]] bool Contains(const Bytes &haystack, const Bytes &needle) {
	return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

} // namespace

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using namespace Purple::SeekRules;
	using ms = std::int64_t;

	expect(StepFrom(5) == 5 && StepFrom(10) == 10 && StepFrom(15) == 15 && StepFrom(30) == 30, "every listed step is kept");
	expect(StepFrom(0) == 10 && StepFrom(3) == 10 && StepFrom(60) == 10 && StepFrom(-10) == 10, "any other step reads as 10");
	expect(kStepDefault == 10, "the default step is 10 s");
	expect(StepSetting(5) == "5s" && StepSetting(30) == "30s", "the step is written the way settings.toml spells it");
	expect(StepSetting(7) == "10s", "an unknown step is written as the default");

	expect(SeekTarget(20'000, 60'000, 1, 10) == ms(30'000), "forward moves by the step");
	expect(SeekTarget(20'000, 60'000, -1, 10) == ms(10'000), "back moves by the step");
	expect(SeekTarget(20'000, 60'000, 1, 5) == ms(25'000), "a 5 s step");
	expect(SeekTarget(20'000, 60'000, -1, 15) == ms(5'000), "a 15 s step");
	expect(SeekTarget(40'000, 120'000, 1, 30) == ms(70'000), "a 30 s step");
	expect(SeekTarget(20'000, 60'000, 1, 7) == ms(30'000), "an unknown step seeks by 10 s");

	expect(SeekTarget(4'000, 60'000, -1, 10) == ms(0), "back before the start goes to 0:00");
	expect(!SeekTarget(0, 60'000, -1, 10), "back at 0:00 does nothing");
	expect(SeekTarget(55'000, 60'000, 1, 10) == ms(59'000), "forward past the end goes to one second before it");
	expect(!SeekTarget(59'000, 60'000, 1, 10), "forward at one second before the end does nothing");
	expect(!SeekTarget(59'500, 60'000, 1, 10), "forward inside the last second never moves back");
	expect(SeekTarget(59'500, 60'000, -1, 10) == ms(49'500), "back from the last second still works");
	expect(!SeekTarget(300, 800, 1, 10), "forward in a track shorter than a second does nothing");
	expect(SeekTarget(300, 800, -1, 10) == ms(0), "back in a track shorter than a second goes to 0:00");

	expect(!SeekTarget(10'000, 0, 1, 10), "an unknown length does nothing");
	expect(!SeekTarget(10'000, -1, -1, 10), "a negative length does nothing");
	expect(!SeekTarget(10'000, 60'000, 0, 10), "no direction does nothing");
	expect(SeekTarget(-500, 60'000, 1, 10) == ms(10'000), "a position before 0 is read as 0");
	expect(SeekTarget(70'000, 60'000, -1, 10) == ms(50'000), "a position past the end is read as the end");

	expect(ToMs(48'000, 48'000) == ms(1'000) && ToMs(1'500, 1'000) == ms(1'500), "samples and milliseconds convert");
	expect(ToMs(0x7FFF'FFFF'FFFF'FFFFLL, 1'000) == std::nullopt, "an unavailable duration is unknown, not an overflow");
	expect(ToMs(0x7FFF'FFFF'FFFF'FFFFLL, 48'000) == std::nullopt, "an unavailable sample count is unknown, not an overflow");
	expect(ToMs(kLongestMs, 1'000) == ms(kLongestMs), "the longest supported length still converts");
	expect(!ToMs(-1, 1'000) && !ToMs(1'000, 0), "a negative value or no frequency is unknown");

	const auto playing = SeekFacts{
		.current = true,
		.position = 20'000,
		.length = 60'000,
		.frequency = 1'000,
	};
	auto facts = playing;
	auto plan = PlanSeek(facts, -1, 10);
	expect(plan.action == SeekAction::SeekPlaying && plan.progress == 10'000. / 60'000., "a playing track seeks and keeps playing");
	facts.paused = true;
	plan = PlanSeek(facts, 1, 10);
	expect(plan.action == SeekAction::SeekPaused && plan.progress == 0.5, "a paused track seeks and stays paused");
	facts = playing;
	facts.current = false;
	expect(PlanSeek(facts, 1, 10).action == SeekAction::Decline, "a track that is not current is declined");
	facts = playing;
	facts.length = 0;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Decline, "an unknown length is declined");
	facts.length = 0x7FFF'FFFF'FFFF'FFFFLL;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Decline, "an unavailable length is declined");
	facts = playing;
	facts.stopped = true;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Decline, "a stream that is stopping is declined");

	const auto ended = SeekFacts{ .ended = true, .endedDurationMs = 60'000 };
	plan = PlanSeek(ended, -1, 10);
	expect(plan.action == SeekAction::Restart && plan.restartAtMs == 50'000, "back after the end plays again from the last step");
	plan = PlanSeek(ended, -1, 30);
	expect(plan.action == SeekAction::Restart && plan.restartAtMs == 30'000, "the restart follows the step");
	auto shortEnded = ended;
	shortEnded.endedDurationMs = 4'000;
	plan = PlanSeek(shortEnded, -1, 10);
	expect(plan.action == SeekAction::Restart && plan.restartAtMs == 0, "back after the end of a short track plays it from 0:00");
	expect(PlanSeek(ended, 1, 10).action == SeekAction::Consume, "forward after the end does nothing but keeps the key");
	auto unknownEnded = ended;
	unknownEnded.endedDurationMs = 0;
	expect(PlanSeek(unknownEnded, -1, 10).action == SeekAction::Decline, "an ended track of unknown length is declined");
	unknownEnded.endedDurationMs = 0x7FFF'FFFF'FFFF'FFFFLL;
	expect(PlanSeek(unknownEnded, -1, 10).action == SeekAction::Decline, "an ended track of unavailable length is declined");
	auto nothing = ended;
	nothing.ended = false;
	expect(PlanSeek(nothing, -1, 10).action == SeekAction::Decline, "no stream and no current track is declined");
	facts = playing;
	facts.position = 0;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Consume, "back at 0:00 does nothing but keeps the key");

	expect(SeekShortcutKeys(-1, false) == "alt+shift+left" && SeekShortcutKeys(1, false) == "alt+shift+right", "Windows and Linux keep alt+shift and an arrow");
	expect(SeekShortcutKeys(-1, true) == "ctrl+alt+left" && SeekShortcutKeys(1, true) == "ctrl+alt+right", "macOS uses Command+Option and an arrow");
	expect(SeekShortcutKeys(-1, true).find("shift") == std::string::npos, "the macOS keys are not Option+Shift word selection");

	namespace Crypt = Purple::RecordingCrypt;
	namespace Registry = Purple::RecordingRegistry;
	namespace Recovery = Purple::RecordingRecovery;
	const auto root = std::filesystem::temp_directory_path()
		/ ("purple-recording-cases-" + std::to_string(std::random_device()()));
	std::filesystem::create_directories(root);
	const auto key = TestKey(3);
	const auto other = TestKey(4);

	auto plain = Bytes(5000);
	for (auto i = 0; i != int(plain.size()); ++i) {
		plain[i] = std::uint8_t(i * 31 + 7);
	}
	auto iv = Crypt::Iv();
	iv.fill(0xFF);
	auto whole = plain;
	expect(Crypt::Apply(*key, iv, 0, whole.data(), whole.size()), "the keystream applies");
	auto pieces = plain;
	auto piecewise = true;
	for (const auto &[from, till] : { std::pair{ 0, 5 }, std::pair{ 5, 37 }, std::pair{ 37, 4096 }, std::pair{ 4096, 5000 } }) {
		piecewise = piecewise && Crypt::Apply(*key, iv, from, pieces.data() + from, till - from);
	}
	expect(piecewise && pieces == whole, "the keystream at any offset matches one pass, across a counter carry");
	expect(whole != plain, "the keystream changes the bytes");
	expect(Crypt::Apply(*key, iv, 0, whole.data(), whole.size()) && whole == plain, "applying it again restores them");

	const auto mediaPath = root / "a.prc";
	{
		auto file = Crypt::File::Create(mediaPath, key, Random);
		expect(file && file->writeAt(0, plain.data(), plain.size()), "an encrypted file is written");
		const auto patch = Bytes{ 1, 2, 3, 4 };
		expect(file && file->writeAt(100, patch.data(), patch.size()), "a write at an offset rewrites in place");
		std::copy(patch.begin(), patch.end(), plain.begin() + 100);
		expect(file && file->size() == plain.size(), "the size is the plain size");
	}
	const auto disk = FileBytes(mediaPath);
	expect(disk.size() == plain.size() + Crypt::kHeaderBytes, "the file is the header and the ciphertext");
	expect(!Contains(disk, Bytes(plain.begin(), plain.begin() + 64)), "no plain bytes reach the disk");
	auto status = Crypt::OpenStatus::Missing;
	auto read = Crypt::ReadWhole(mediaPath, key, &status);
	expect(read && *read == plain && status == Crypt::OpenStatus::Ok, "the file reads back with the key");
	expect(!Crypt::ReadWhole(mediaPath, other, &status) && status == Crypt::OpenStatus::WrongKey, "another key is refused, not read as garbage");
	expect(!Crypt::ReadWhole(root / "missing.prc", key, &status) && status == Crypt::OpenStatus::Missing, "a missing file is missing");
	{
		auto stray = std::ofstream(root / "stray.prc", std::ios::binary);
		stray << "this is not a recording file at all, only text";
	}
	expect(!Crypt::ReadWhole(root / "stray.prc", key, &status) && status == Crypt::OpenStatus::NotOurs, "a file without the magic is not ours");
	auto bumped = disk;
	bumped[4] = 2;
	{
		auto out = std::ofstream(root / "future.prc", std::ios::binary);
		out.write(reinterpret_cast<const char*>(bumped.data()), std::streamsize(bumped.size()));
	}
	expect(!Crypt::ReadWhole(root / "future.prc", key, &status) && status == Crypt::OpenStatus::UnknownVersion, "a future version is not read");
	const auto small = Bytes{ 'h', 'i' };
	expect(Crypt::WriteWhole(root / "whole.prc", key, Random, small.data(), small.size()), "a whole file is written");
	expect(Crypt::ReadWhole(root / "whole.prc", key) == small, "a whole file reads back");
	expect(!std::filesystem::exists(root / "whole.prc.new"), "the atomic write leaves no temporary file");
	const auto first = FileBytes(root / "whole.prc");
	expect(Crypt::WriteWhole(root / "whole.prc", key, Random, small.data(), small.size()), "a whole file is replaced");
	expect(FileBytes(root / "whole.prc") != first, "every write takes a fresh IV");

	auto opened = 0;
	auto failedIds = std::vector<std::string>();
	auto restartedIds = std::vector<std::string>();
	auto refuse = false;
	auto mirror = Purple::RecordingMirror([&]() -> std::optional<Purple::RecordingMirror::Opened> {
		if (refuse) {
			return std::nullopt;
		}
		const auto id = "m" + std::to_string(++opened);
		auto file = Crypt::File::Create(root / (id + ".prc"), key, Random);
		if (!file) {
			return std::nullopt;
		}
		return Purple::RecordingMirror::Opened{ id, std::move(file) };
	}, [&](const std::string &id) {
		failedIds.push_back(id);
	}, [&](Purple::RecordingMirror::Closed closed) {
		restartedIds.push_back(closed.id);
	});
	const auto owner = reinterpret_cast<const void*>(&opened);
	const auto block = Bytes(4096, 0xAB);
	mirror.write(owner, 4096, block.data(), block.size());
	expect(opened == 0, "a write that does not start at 0 opens nothing");
	auto memory = Bytes();
	const auto model = [&](std::int64_t offset, const Bytes &data) {
		if (memory.size() < offset + data.size()) {
			memory.resize(offset + data.size());
		}
		std::copy(data.begin(), data.end(), memory.begin() + offset);
		mirror.write(owner, offset, data.data(), data.size());
	};
	model(0, Bytes(4096, 0x11));
	model(4096, Bytes(4096, 0x22));
	model(8192, Bytes(1000, 0x33));
	model(28, Bytes{ 9, 9, 9, 9 });
	model(0, Bytes{ 'O', 'g', 'g', 'S' });
	expect(opened == 1 && mirror.activeId(owner) == std::string("m1"), "a rewrite at 0 keeps the open file");
	const auto ids = mirror.requestCloseAll();
	expect(ids == std::vector<std::string>{ "m1" }, "closing names the open recording");
	model(9192, Bytes(10, 0x44));
	auto closed = mirror.close(owner);
	expect(closed && closed->id == "m1" && closed->size == memory.size() && !closed->failed, "closing reports the size");
	expect(Crypt::ReadWhole(root / "m1.prc", key) == memory, "the mirror holds exactly the bytes written, seeks included");
	model(0, Bytes(10, 0x55));
	const auto ids2 = mirror.requestCloseAll();
	model(0, Bytes(10, 0x66));
	expect(opened == 3 && mirror.activeId(owner) == std::string("m3"), "a write at 0 after a close request starts the next recording");
	expect(Crypt::ReadWhole(root / "m2.prc", key) == Bytes(10, 0x55), "the earlier recording is left whole");
	expect(restartedIds == std::vector<std::string>{ "m2" }, "a recording closed by the next one is reported");
	expect(mirror.closeAll().size() == 1 && !mirror.activeId(owner), "close all closes every recording");
	refuse = true;
	mirror.write(owner, 0, block.data(), block.size());
	mirror.write(owner, 4096, block.data(), block.size());
	closed = mirror.close(owner);
	expect(closed && closed->failed && closed->id.empty() && opened == 3, "a refused open writes nothing until the recording ends");
	refuse = false;
	expect(failedIds.empty(), "no write failed so far");

	auto manifest = Registry::Manifest();
	manifest.entries.push_back({
		.id = "e1",
		.account = 0,
		.peer = 0x1234'5678'9ABCULL,
		.topic = 7,
		.kind = Registry::Kind::Voice,
		.started = 100,
		.durationMs = 1500,
		.media = "e1.ogg.prc",
		.state = Registry::State::Recording,
	});
	manifest.entries.push_back({
		.id = "e2",
		.account = 1,
		.peer = 99,
		.reply = -3,
		.kind = Registry::Kind::Round,
		.started = 200,
		.binned = 250,
		.viewOnce = true,
		.partial = true,
		.media = "e2.mp4.prc",
		.journal = "e2.pjr.prc",
		.state = Registry::State::Recovered,
	});
	auto readStatus = Registry::ReadStatus::Damaged;
	const auto encoded = Registry::Encode(manifest);
	const auto decoded = Registry::Decode(encoded, &readStatus);
	expect(readStatus == Registry::ReadStatus::Ok && decoded.entries == manifest.entries, "the manifest round-trips");
	expect(Registry::Decode("", &readStatus).entries.empty() && readStatus == Registry::ReadStatus::Empty, "an empty manifest is empty");
	expect(Registry::Decode("{\"version\":2,\"entries\":[]}", &readStatus).entries.empty() && readStatus == Registry::ReadStatus::UnknownVersion, "a future manifest is not read");
	expect(Registry::Decode("{\"version\":1,\"entries\":[", &readStatus).entries.empty() && readStatus == Registry::ReadStatus::Damaged, "a torn manifest is damaged");
	auto hostile = encoded;
	hostile.replace(hostile.find("e1.ogg.prc"), 10, "../../x.ogg");
	const auto filtered = Registry::Decode(hostile, &readStatus);
	expect(readStatus == Registry::ReadStatus::Damaged && filtered.entries.size() == 1 && filtered.entries[0].id == "e2", "an entry naming a path outside the directory is dropped");
	expect(!Registry::ValidFileName("a/b") && !Registry::ValidFileName(".x") && !Registry::ValidFileName("") && Registry::ValidFileName("0a1b.ogg.prc"), "file names stay inside the directory");

	expect(Registry::StopKindFromOrdinal(0) == Registry::StopKind::Cancel && Registry::StopKindFromOrdinal(1) == Registry::StopKind::Send && Registry::StopKindFromOrdinal(2) == Registry::StopKind::Listen && !Registry::StopKindFromOrdinal(3), "the stop type is read by its upstream order");
	auto decision = Registry::DecideStop(Registry::StopKind::Send, false);
	expect(decision.keepFiles && decision.state == Registry::State::Sending, "send keeps the files until the message is sent");
	decision = Registry::DecideStop(Registry::StopKind::Cancel, true);
	expect(decision.keepFiles && decision.state == Registry::State::Discarded, "discard moves to the bin with keep_discarded_p on");
	decision = Registry::DecideStop(Registry::StopKind::Cancel, false);
	expect(!decision.keepFiles, "discard deletes with keep_discarded_p off");
	decision = Registry::DecideStop(Registry::StopKind::Listen, false);
	expect(decision.keepFiles && decision.state == Registry::State::Recording, "listening keeps recording");
	expect(Registry::NeedsRecovery(Registry::State::Recording) && Registry::NeedsRecovery(Registry::State::Sending) && !Registry::NeedsRecovery(Registry::State::Discarded), "recording and sending belong to a crash at start");
	auto entry = manifest.entries[1];
	expect(Registry::MarkOf(entry) == Registry::Mark::Partial, "a partial recovery is marked partly recovered");
	entry.partial = false;
	expect(Registry::MarkOf(entry) == Registry::Mark::Recovered, "a recovery is marked recovered");
	entry.state = Registry::State::Unsent;
	expect(Registry::MarkOf(entry) == Registry::Mark::NotSent, "an unsent one is marked not sent");
	entry.state = Registry::State::Discarded;
	expect(Registry::MarkOf(entry) == Registry::Mark::Discarded, "a discarded one is marked discarded");

	auto sends = Registry::Manifest();
	sends.entries = {
		{ .id = "s1", .account = 0, .peer = 5, .kind = Registry::Kind::Voice, .started = 30, .media = "s1", .state = Registry::State::Sending },
		{ .id = "s2", .account = 0, .peer = 5, .kind = Registry::Kind::Voice, .started = 10, .media = "s2", .state = Registry::State::Sending },
		{ .id = "s3", .account = 0, .peer = 5, .kind = Registry::Kind::Round, .started = 5, .media = "s3", .state = Registry::State::Sending },
		{ .id = "s4", .account = 0, .peer = 6, .kind = Registry::Kind::Voice, .started = 1, .binned = 1000, .fromListen = true, .media = "s4", .state = Registry::State::Discarded },
		{ .id = "s5", .account = 0, .peer = 7, .kind = Registry::Kind::Voice, .started = 1, .binned = 1000, .media = "s5", .state = Registry::State::Discarded },
	};
	expect(Registry::MatchSent(sends, { .account = 0, .peer = 5, .kind = Registry::Kind::Voice, .now = 1000 }) == std::size_t(1), "a sent voice settles the earliest waiting one");
	expect(Registry::MatchSent(sends, { .account = 0, .peer = 5, .kind = Registry::Kind::Round, .now = 1000 }) == std::size_t(2), "kinds are matched apart");
	expect(!Registry::MatchSent(sends, { .account = 1, .peer = 5, .kind = Registry::Kind::Voice, .now = 1000 }), "another account matches nothing");
	expect(Registry::MatchSent(sends, { .account = 0, .peer = 6, .kind = Registry::Kind::Voice, .now = 1100 }) == std::size_t(3), "a send from the listen state settles its discarded entry");
	expect(!Registry::MatchSent(sends, { .account = 0, .peer = 6, .kind = Registry::Kind::Voice, .now = 1000 + Registry::kListenSendWindowSeconds + 1 }), "only within the window");
	expect(!Registry::MatchSent(sends, { .account = 0, .peer = 7, .kind = Registry::Kind::Voice, .now = 1001 }), "a real discard is never settled by a send");
	sends.entries[0].state = Registry::State::Sent;
	const auto expired = Registry::Expired(sends, [](std::int64_t binned) { return binned < 2000; });
	expect(expired == std::vector<std::string>{ "s1", "s4", "s5" }, "sent entries and expired bin items are swept");
	expect(Registry::Expired(sends, nullptr) == std::vector<std::string>{ "s1" }, "without a rule only sent entries go");
	const auto newest = Registry::BinNewestFirst(manifest);
	expect(newest.size() == 1 && newest[0]->id == "e2", "the bin lists only bin states");

	const auto torn = OpusStream(5, false);
	auto cut = Bytes(torn.begin(), torn.end() - 20);
	const auto voicePath = root / "v.prc";
	expect(Crypt::WriteWhole(voicePath, key, Random, cut.data(), cut.size()), "a torn recording is stored");
	auto voice = Recovery::RecoverVoice(voicePath, key, Random, false);
	expect(voice.state == Registry::State::Recovered && voice.changed, "a torn voice recording is recovered");
	expect(voice.durationMs == 4000, "its duration comes from the last good granule");
	const auto fixed = Crypt::ReadWhole(voicePath, key);
	expect(fixed && Purple::ScanOgg(fixed->data(), fixed->size()).clean(), "it is a finished Ogg stream afterwards");
	voice = Recovery::RecoverVoice(voicePath, key, Random, true);
	expect(voice.state == Registry::State::Unsent && !voice.changed, "a finished one that was sending is not sent");
	const auto junk = Bytes(300, 0x5A);
	expect(Crypt::WriteWhole(voicePath, key, Random, junk.data(), junk.size()), "junk is stored");
	voice = Recovery::RecoverVoice(voicePath, key, Random, false);
	expect(voice.state == Registry::State::Unrecoverable && Crypt::ReadWhole(voicePath, key) == junk, "junk is unrecoverable and kept as it was");
	voice = Recovery::RecoverVoice(voicePath, other, Random, false);
	expect(voice.state == Registry::State::Unrecoverable, "a file under another key is unrecoverable");
	expect(Recovery::VoiceDurationMs(OpusStream(3, true)) == 3000, "a finished stream reports its duration");

	std::filesystem::remove_all(root);

	std::cout << (checks - failures) << '/' << checks << " recording cases passed\n";
	return failures ? 1 : 0;
}
