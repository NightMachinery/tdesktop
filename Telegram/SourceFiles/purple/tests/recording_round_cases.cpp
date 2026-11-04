#include "purple/purple_mp4_repair.h"
#include "purple/purple_recording_crypt.h"
#include "purple/purple_recording_round_media.h"
#include "purple/purple_round_journal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
} // extern "C"

#include <openssl/rand.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace Crypt = Purple::RecordingCrypt;
namespace Media = Purple::RoundMedia;

constexpr auto kSide = 240;
constexpr auto kFps = 30;
constexpr auto kRate = 48000;

[[nodiscard]] bool Random(std::uint8_t *data, std::size_t size) {
	return RAND_bytes(data, int(size)) == 1;
}

[[nodiscard]] std::shared_ptr<const Crypt::Key> TestKey() {
	auto key = std::make_shared<Crypt::Key>();
	for (auto i = 0; i != int(key->bytes.size()); ++i) {
		key->bytes[i] = std::uint8_t(i * 5 + 1);
	}
	key->check.fill(0x42);
	return key;
}

struct Sink {
	Bytes bytes;
	std::int64_t position = 0;
};

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(61, 1, 100)
int SinkWrite(void *opaque, const std::uint8_t *data, int size) {
#else
int SinkWrite(void *opaque, std::uint8_t *data, int size) {
#endif
	const auto sink = static_cast<Sink*>(opaque);
	if (sink->bytes.size() < std::size_t(sink->position + size)) {
		sink->bytes.resize(sink->position + size);
	}
	std::memcpy(sink->bytes.data() + sink->position, data, size);
	sink->position += size;
	return size;
}

std::int64_t SinkSeek(void *opaque, std::int64_t offset, int whence) {
	const auto sink = static_cast<Sink*>(opaque);
	if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) {
		return std::int64_t(sink->bytes.size());
	}
	sink->position = ((whence & ~AVSEEK_FORCE) == SEEK_CUR)
		? (sink->position + offset)
		: ((whence & ~AVSEEK_FORCE) == SEEK_END)
		? (std::int64_t(sink->bytes.size()) + offset)
		: offset;
	return sink->position;
}

struct Recorded {
	std::vector<AVPacket*> packets;
	int video = 0;
	int audio = 0;
	AVFormatContext *upstream = nullptr;
	Sink sink;
	Bytes audioRaw;
	std::vector<int> audioSizes;

	~Recorded() {
		for (auto &packet : packets) {
			av_packet_free(&packet);
		}
		if (upstream) {
			av_freep(&upstream->pb->buffer);
			avio_context_free(&upstream->pb);
			avformat_free_context(upstream);
		}
	}
};

void Drain(AVCodecContext *codec, AVStream *stream, Recorded &recorded, bool video) {
	auto packet = av_packet_alloc();
	while (avcodec_receive_packet(codec, packet) == 0) {
		packet->stream_index = stream->index;
		av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
		if (!video) {
			recorded.audioRaw.insert(recorded.audioRaw.end(), packet->data, packet->data + packet->size);
			recorded.audioSizes.push_back(packet->size);
		}
		recorded.packets.push_back(av_packet_clone(packet));
		(video ? recorded.video : recorded.audio) += 1;
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
}

[[nodiscard]] bool Record(Recorded &recorded, int seconds) {
	auto videoCodec = avcodec_find_encoder_by_name("libopenh264");
	if (!videoCodec) {
		videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
	}
	const auto audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!videoCodec || !audioCodec) {
		return false;
	}
	avformat_alloc_output_context2(&recorded.upstream, nullptr, "mp4", nullptr);
	const auto buffer = static_cast<std::uint8_t*>(av_malloc(4096));
	recorded.upstream->pb = avio_alloc_context(buffer, 4096, 1, &recorded.sink, nullptr, SinkWrite, SinkSeek);
	recorded.upstream->flags |= AVFMT_FLAG_CUSTOM_IO;

	auto video = avcodec_alloc_context3(videoCodec);
	video->width = kSide;
	video->height = kSide;
	video->pix_fmt = AV_PIX_FMT_YUV420P;
	video->time_base = AVRational{ 1, 1'000'000 };
	video->framerate = AVRational{ kFps, 1 };
	video->bit_rate = 400'000;
	video->max_b_frames = 0;
	video->gop_size = kFps;
	av_opt_set(video->priv_data, "preset", "ultrafast", 0);
	av_opt_set(video->priv_data, "tune", "zerolatency", 0);
	auto audio = avcodec_alloc_context3(audioCodec);
	audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
	audio->bit_rate = 64'000;
	audio->sample_rate = kRate;
	av_channel_layout_default(&audio->ch_layout, 1);
	if (avcodec_open2(video, videoCodec, nullptr) < 0 || avcodec_open2(audio, audioCodec, nullptr) < 0) {
		return false;
	}
	const auto videoStream = avformat_new_stream(recorded.upstream, videoCodec);
	const auto audioStream = avformat_new_stream(recorded.upstream, audioCodec);
	avcodec_parameters_from_context(videoStream->codecpar, video);
	avcodec_parameters_from_context(audioStream->codecpar, audio);
	videoStream->time_base = video->time_base;
	audioStream->time_base = AVRational{ 1, kRate };
	if (avformat_write_header(recorded.upstream, nullptr) < 0) {
		return false;
	}
	auto frame = av_frame_alloc();
	frame->format = video->pix_fmt;
	frame->width = kSide;
	frame->height = kSide;
	av_frame_get_buffer(frame, 0);
	auto sound = av_frame_alloc();
	sound->format = audio->sample_fmt;
	sound->nb_samples = audio->frame_size;
	sound->sample_rate = kRate;
	av_channel_layout_copy(&sound->ch_layout, &audio->ch_layout);
	av_frame_get_buffer(sound, 0);
	auto samples = std::int64_t(0);
	for (auto i = 0; i != seconds * kFps; ++i) {
		av_frame_make_writable(frame);
		for (auto y = 0; y != kSide; ++y) {
			std::memset(frame->data[0] + y * frame->linesize[0], (y + i * 3) & 0xFF, kSide);
		}
		std::memset(frame->data[1], 128 + (i % 20), frame->linesize[1] * kSide / 2);
		std::memset(frame->data[2], 128 - (i % 20), frame->linesize[2] * kSide / 2);
		frame->pts = std::int64_t(i) * 1'000'000 / kFps + ((i % 4 == 1) ? 9'000 : 0);
		avcodec_send_frame(video, frame);
		Drain(video, videoStream, recorded, true);
		while (samples * kFps < std::int64_t(i + 1) * kRate) {
			av_frame_make_writable(sound);
			const auto data = reinterpret_cast<float*>(sound->data[0]);
			for (auto s = 0; s != sound->nb_samples; ++s) {
				data[s] = float(0.3 * std::sin((samples + s) * 0.05));
			}
			sound->pts = samples;
			samples += sound->nb_samples;
			avcodec_send_frame(audio, sound);
			Drain(audio, audioStream, recorded, false);
		}
	}
	avcodec_send_frame(video, nullptr);
	Drain(video, videoStream, recorded, true);
	avcodec_send_frame(audio, nullptr);
	Drain(audio, audioStream, recorded, false);
	av_frame_free(&frame);
	av_frame_free(&sound);
	avcodec_free_context(&video);
	avcodec_free_context(&audio);
	return true;
}

[[nodiscard]] int CountVideo(const Bytes &bytes, std::int64_t *durationMs = nullptr) {
	const auto remuxed = Media::Remux(bytes);
	if (durationMs) {
		*durationMs = remuxed ? remuxed->durationMs : 0;
	}
	return remuxed ? remuxed->videoPackets : -1;
}

} // namespace

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	av_log_set_level(AV_LOG_QUIET);
	const auto root = std::filesystem::temp_directory_path()
		/ ("purple-round-cases-" + std::to_string(std::random_device()()));
	std::filesystem::create_directories(root);
	const auto key = TestKey();

	auto sets = Media::ParameterSets{ Bytes{ 0x67, 0x42, 0xC0, 0x1E, 0xAA }, Bytes{ 0x68, 0xCE, 0x3C } };
	const auto avcc = Media::MakeAvcc(sets);
	auto read = Media::ParameterSetsFromExtradata(avcc.data(), avcc.size());
	expect(read && read->sps == sets.sps && read->pps == sets.pps, "an avcC record gives back its SPS and PPS");
	auto annexB = Bytes{ 0, 0, 0, 1 };
	annexB.insert(annexB.end(), sets.sps.begin(), sets.sps.end());
	annexB.insert(annexB.end(), { 0, 0, 1 });
	annexB.insert(annexB.end(), sets.pps.begin(), sets.pps.end());
	read = Media::ParameterSetsFromExtradata(annexB.data(), annexB.size());
	expect(read && read->sps == sets.sps && read->pps == sets.pps, "Annex B extradata gives the same sets");
	auto sample = Bytes{ 0, 0, 0, 5 };
	sample.insert(sample.end(), sets.sps.begin(), sets.sps.end());
	sample.insert(sample.end(), { 0, 0, 0, 3 });
	sample.insert(sample.end(), sets.pps.begin(), sets.pps.end());
	sample.insert(sample.end(), { 0, 0, 0, 2, 0x65, 0x88 });
	read = Media::ParameterSetsFromSample(sample.data(), sample.size());
	expect(read && read->sps == sets.sps && read->pps == sets.pps, "an AVCC key frame gives its in-band sets");
	sample[3] = 200;
	expect(!Media::ParameterSetsFromSample(sample.data(), sample.size()), "a length past the sample stops the walk");
	expect(!Media::ParameterSetsFromExtradata(sets.pps.data(), sets.pps.size()), "too little extradata gives nothing");

	auto recorded = Recorded();
	const auto ok = Record(recorded, 3);
	expect(ok && recorded.video >= 85 && recorded.audio >= 140, "a three second round is encoded");
	if (!ok) {
		std::cout << (checks - failures) << '/' << checks << " round recording cases passed\n";
		return 1;
	}

	const auto config = Media::ConfigOf(recorded.upstream);
	expect(config && config->avccVideo && config->tracks.size() == 2, "the config header names both tracks with AVCC framing");
	expect(config && config->videoTrack() == 0 && config->audioTrack() == 1, "video comes first, as the recorder adds it");
	expect(config && !config->tracks[1].csd.empty() && config->tracks[1].sampleRate == kRate, "the audio track keeps its AudioSpecificConfig");

	auto measure = Media::MakeAacMeasure();
	auto exact = !recorded.audioSizes.empty() && config;
	auto at = std::size_t();
	for (auto i = 0; exact && i + 1 < int(recorded.audioSizes.size()); ++i) {
		const auto measured = measure(config->tracks[1], recorded.audioRaw.data() + at, std::min<std::size_t>(8192, recorded.audioRaw.size() - at));
		exact = (measured == recorded.audioSizes[i]);
		at += recorded.audioSizes[i];
	}
	expect(exact, "the AAC measure finds every raw frame's exact length");
	expect(measure(config->tracks[1], recorded.audioRaw.data(), 3) == 0, "a torn frame does not measure");

	const auto copy = [&](int journalSeconds, bool finish, Bytes *journal) -> Bytes {
		const auto path = root / ("copy-" + std::to_string(journalSeconds) + (finish ? "-f" : "-c") + ".prc");
		auto muxer = Media::CopyMuxer::Start(Crypt::File::Create(path, key, Random), recorded.upstream, journalSeconds);
		if (!muxer) {
			return {};
		}
		if (journal) {
			*journal = Purple::EncodeRoundJournalStart(*config);
		}
		auto good = true;
		for (const auto packet : recorded.packets) {
			good = muxer->write(packet) && good;
		}
		if (finish) {
			good = muxer->finish() && good;
		}
		muxer = nullptr;
		auto bytes = Crypt::ReadWhole(path, key);
		return (good && bytes) ? *bytes : Bytes();
	};
	const auto recover = [&](const Bytes &media, const Bytes *journal, bool sending) {
		const auto mediaPath = root / "recover.prc";
		const auto journalPath = root / "recover.pjr.prc";
		expect(Crypt::WriteWhole(mediaPath, key, Random, media.data(), media.size()), "a copy is stored");
		std::filesystem::remove(journalPath);
		if (journal) {
			expect(Crypt::WriteWhole(journalPath, key, Random, journal->data(), journal->size()), "a journal is stored");
		}
		auto result = Media::Recover(mediaPath, journal ? journalPath : std::filesystem::path(), key, Random, sending);
		auto bytes = Crypt::ReadWhole(mediaPath, key);
		return std::pair{ result, bytes ? *bytes : Bytes() };
	};
	using State = Purple::RecordingRegistry::State;

	const auto fragmented = copy(1, true, nullptr);
	auto duration = std::int64_t();
	expect(CountVideo(fragmented, &duration) == recorded.video, "the fragmented copy holds every video packet");
	expect(duration >= 2900 && duration <= 3200, "and lasts the three seconds");
	auto [result, bytes] = recover(fragmented, nullptr, false);
	expect(result.state == State::Recovered && !result.partial && CountVideo(bytes) == recorded.video, "a finished fragmented copy is remuxed whole");
	expect(bytes.size() > 8 && std::memcmp(bytes.data() + 4, "ftyp", 4) == 0, "into a standard MP4");
	std::tie(result, bytes) = recover(fragmented, nullptr, true);
	expect(result.state == State::Unsent, "one that was being sent is marked not sent");

	const auto joined = Media::Join({ fragmented, fragmented });
	expect(joined && joined->videoPackets == 2 * recorded.video && joined->durationMs >= 5800 && joined->durationMs <= 6400, "two parts of a resumed round join into one, back to back");
	expect(joined && CountVideo(joined->bytes) == 2 * recorded.video, "and the joined file reads back whole");

	const auto crashed = copy(1, false, nullptr);
	expect(!crashed.empty() && CountVideo(crashed) >= recorded.video - 2 * kFps, "a crash keeps the fragments written before it");
	for (const auto keep : { 0.95, 0.8, 0.6 }) {
		const auto cut = Bytes(crashed.begin(), crashed.begin() + std::size_t(crashed.size() * keep));
		std::tie(result, bytes) = recover(cut, nullptr, false);
		const auto count = CountVideo(bytes, &duration);
		expect(result.state == State::Recovered && count > 0 && count <= recorded.video, "a torn fragmented copy is recovered up to the tear");
		expect(keep < 0.7 || result.partial, "a tear inside a fragment marks it partial");
		expect(keep > 0.9 || count < recorded.video, "a deep tear loses the video after it");
		expect(duration > 0 && result.durationMs == duration, "its duration is reported");
	}

	auto journal = Bytes();
	const auto plain = copy(0, false, &journal);
	expect(!plain.empty() && CountVideo(plain) < 0, "with the journal off a crashed copy has no index");
	std::tie(result, bytes) = recover(plain, &journal, false);
	const auto plainCount = CountVideo(bytes, &duration);
	expect(result.state == State::Recovered && plainCount >= recorded.video - 2, "repair rebuilds it from the config header alone");
	expect(duration >= 2800 && duration <= 3300, "with the timing of the audio clock");
	const auto torn = Bytes(plain.begin(), plain.begin() + plain.size() * 2 / 3);
	std::tie(result, bytes) = recover(torn, &journal, false);
	expect(result.state == State::Recovered && CountVideo(bytes) > recorded.video / 2 && CountVideo(bytes) < recorded.video, "a torn plain copy is repaired up to the tear");
	std::tie(result, bytes) = recover(plain, nullptr, false);
	expect(result.state == State::Unrecoverable && bytes == plain, "without a config header it is unrecoverable and kept raw");
	const auto finished = copy(0, true, nullptr);
	std::tie(result, bytes) = recover(finished, nullptr, false);
	expect(result.state == State::Recovered && CountVideo(bytes) == recorded.video, "a finished plain copy needs no journal");

	expect(Media::CopyMuxer::Start(nullptr, recorded.upstream, 1) == nullptr, "no file, no copy");
	auto junk = Bytes(5000, 0x33);
	std::tie(result, bytes) = recover(junk, &journal, false);
	expect(result.state == State::Unrecoverable && bytes == junk, "junk is unrecoverable and kept as it was");

	std::filesystem::remove_all(root);
	std::cout << (checks - failures) << '/' << checks << " round recording cases passed\n";
	return failures ? 1 : 0;
}
