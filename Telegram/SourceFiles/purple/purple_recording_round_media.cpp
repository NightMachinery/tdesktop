/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_round_media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
} // extern "C"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace Purple::RoundMedia {
namespace {

using State = RecordingRegistry::State;

constexpr auto kBlock = 4096;
constexpr auto kMicroseconds = AVRational{ 1, 1'000'000 };
constexpr auto kMaxNal = std::size_t(1024 * 1024);

struct Reader {
	const std::uint8_t *data = nullptr;
	std::int64_t size = 0;
	std::int64_t position = 0;

	static int Read(void *opaque, std::uint8_t *buffer, int size) {
		const auto reader = static_cast<Reader*>(opaque);
		const auto left = reader->size - reader->position;
		const auto count = int(std::min<std::int64_t>(size, left));
		if (count <= 0) {
			return AVERROR_EOF;
		}
		std::memcpy(buffer, reader->data + reader->position, count);
		reader->position += count;
		return count;
	}

	static std::int64_t Seek(void *opaque, std::int64_t offset, int whence) {
		const auto reader = static_cast<Reader*>(opaque);
		auto position = std::int64_t(-1);
		switch (whence & ~AVSEEK_FORCE) {
		case SEEK_SET: position = offset; break;
		case SEEK_CUR: position = reader->position + offset; break;
		case SEEK_END: position = reader->size + offset; break;
		case AVSEEK_SIZE: return reader->size;
		}
		if (position < 0 || position > reader->size) {
			return -1;
		}
		reader->position = position;
		return position;
	}
};

struct Writer {
	Bytes bytes;
	std::int64_t position = 0;

	int write(const std::uint8_t *data, int size) {
		if (size <= 0) {
			return 0;
		}
		const auto end = std::size_t(position) + std::size_t(size);
		if (bytes.size() < end) {
			bytes.resize(end);
		}
		std::memcpy(bytes.data() + position, data, size);
		position += size;
		return size;
	}

	std::int64_t seek(std::int64_t offset, int whence) {
		auto updated = std::int64_t(-1);
		switch (whence & ~AVSEEK_FORCE) {
		case SEEK_SET: updated = offset; break;
		case SEEK_CUR: updated = position + offset; break;
		case SEEK_END: updated = std::int64_t(bytes.size()) + offset; break;
		case AVSEEK_SIZE: return std::int64_t(bytes.size());
		}
		if (updated < 0) {
			return -1;
		}
		position = updated;
		return updated;
	}
};

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(61, 1, 100)
using WriteData = const std::uint8_t*;
#else
using WriteData = std::uint8_t*;
#endif

int WriterWrite(void *opaque, WriteData data, int size) {
	return static_cast<Writer*>(opaque)->write(data, size);
}

std::int64_t WriterSeek(void *opaque, std::int64_t offset, int whence) {
	return static_cast<Writer*>(opaque)->seek(offset, whence);
}

int MuxerWrite(void *opaque, WriteData data, int size) {
	return static_cast<CopyMuxer*>(opaque)->writeBytes(data, size);
}

std::int64_t MuxerSeek(void *opaque, std::int64_t offset, int whence) {
	return static_cast<CopyMuxer*>(opaque)->seek(offset, whence);
}

void FreeIo(AVIOContext *&io) {
	if (io) {
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
}

struct Output {
	AVFormatContext *format = nullptr;
	AVIOContext *io = nullptr;

	Output() = default;
	Output(const Output&) = delete;
	Output &operator=(const Output&) = delete;
	~Output() {
		if (format) {
			avformat_free_context(format);
		}
		FreeIo(io);
	}
};

struct Input {
	AVFormatContext *format = nullptr;
	AVIOContext *io = nullptr;

	Input() = default;
	Input(const Input&) = delete;
	Input &operator=(const Input&) = delete;
	~Input() {
		if (format) {
			avformat_close_input(&format);
		}
		FreeIo(io);
	}
};

[[nodiscard]] bool OpenOutput(Output &output, Writer &writer) {
	if (avformat_alloc_output_context2(&output.format, nullptr, "mp4", nullptr) < 0
		|| !output.format) {
		return false;
	}
	const auto buffer = static_cast<std::uint8_t*>(av_malloc(kBlock));
	if (!buffer) {
		return false;
	}
	output.io = avio_alloc_context(buffer, kBlock, 1, &writer, nullptr, WriterWrite, WriterSeek);
	if (!output.io) {
		av_free(buffer);
		return false;
	}
	output.format->pb = output.io;
	output.format->flags |= AVFMT_FLAG_CUSTOM_IO;
	return true;
}

[[nodiscard]] bool OpenInput(Input &input, Reader &reader) {
	const auto buffer = static_cast<std::uint8_t*>(av_malloc(kBlock));
	if (!buffer) {
		return false;
	}
	input.io = avio_alloc_context(buffer, kBlock, 0, &reader, Reader::Read, nullptr, Reader::Seek);
	if (!input.io) {
		av_free(buffer);
		return false;
	}
	input.format = avformat_alloc_context();
	if (!input.format) {
		return false;
	}
	input.format->pb = input.io;
	input.format->flags |= AVFMT_FLAG_CUSTOM_IO;
	input.format->protocol_whitelist = av_strdup("");
	if (avformat_open_input(
			&input.format,
			nullptr,
			av_find_input_format("mov"),
			nullptr) < 0) {
		input.format = nullptr;
		return false;
	}
	return true;
}

[[nodiscard]] std::uint32_t ReadBig(const std::uint8_t *data, int bytes) {
	auto result = std::uint32_t();
	for (auto i = 0; i != bytes; ++i) {
		result = (result << 8) | data[i];
	}
	return result;
}

void Collect(ParameterSets &sets, const std::uint8_t *nal, std::size_t size) {
	if (!size || size > kMaxNal) {
		return;
	}
	const auto type = nal[0] & 0x1F;
	if (type == 7 && sets.sps.empty()) {
		sets.sps.assign(nal, nal + size);
	} else if (type == 8 && sets.pps.empty()) {
		sets.pps.assign(nal, nal + size);
	}
}

[[nodiscard]] std::optional<ParameterSets> Complete(ParameterSets sets) {
	return (sets.sps.size() >= 4 && !sets.pps.empty())
		? std::optional(std::move(sets))
		: std::nullopt;
}

[[nodiscard]] AVCodecParameters *Parameters(const AVFormatContext *format, int index) {
	return format->streams[index]->codecpar;
}

[[nodiscard]] std::int64_t EndMs(AVRational base, std::int64_t pts, std::int64_t duration) {
	if (pts == AV_NOPTS_VALUE) {
		return 0;
	}
	return av_rescale_q(pts + std::max<std::int64_t>(duration, 0), base, AVRational{ 1, 1000 });
}

class AacMeasure final {
public:
	explicit AacMeasure(const RoundTrack &track) {
		const auto codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
		_context = codec ? avcodec_alloc_context3(codec) : nullptr;
		_packet = av_packet_alloc();
		_frame = av_frame_alloc();
		if (!_context || !_packet || !_frame || track.csd.empty()) {
			return;
		}
		const auto &config = track.csd.front();
		_context->extradata = static_cast<std::uint8_t*>(
			av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!_context->extradata) {
			return;
		}
		std::memcpy(_context->extradata, config.data(), config.size());
		_context->extradata_size = int(config.size());
		_context->sample_rate = int(track.sampleRate);
		av_channel_layout_default(&_context->ch_layout, std::max(int(track.channels), 1));
		_ready = (avcodec_open2(_context, codec, nullptr) >= 0);
	}

	~AacMeasure() {
		av_frame_free(&_frame);
		av_packet_free(&_packet);
		avcodec_free_context(&_context);
	}

	[[nodiscard]] int measure(const std::uint8_t *data, std::size_t available) {
		const auto most = int(std::min<std::size_t>(available, kRoundMaxAacFrame));
		if (!_ready || most <= 0 || !decodes(data, most)) {
			return 0;
		}
		auto low = 1;
		auto high = most;
		while (low < high) {
			const auto middle = low + (high - low) / 2;
			if (decodes(data, middle)) {
				high = middle;
			} else {
				low = middle + 1;
			}
		}
		return low;
	}

private:
	[[nodiscard]] bool decodes(const std::uint8_t *data, int size) {
		avcodec_flush_buffers(_context);
		_buffer.assign(data, data + size);
		_buffer.resize(std::size_t(size) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
		_packet->data = _buffer.data();
		_packet->size = size;
		if (avcodec_send_packet(_context, _packet) < 0) {
			return false;
		}
		const auto received = avcodec_receive_frame(_context, _frame);
		const auto result = (received >= 0) && (_frame->nb_samples == 1024);
		av_frame_unref(_frame);
		return result;
	}

	AVCodecContext *_context = nullptr;
	AVPacket *_packet = nullptr;
	AVFrame *_frame = nullptr;
	Bytes _buffer;
	bool _ready = false;

};

[[nodiscard]] bool SetExtradata(AVCodecParameters *par, const Bytes &bytes) {
	if (bytes.empty()) {
		return true;
	}
	par->extradata = static_cast<std::uint8_t*>(
		av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
	if (!par->extradata) {
		return false;
	}
	std::memcpy(par->extradata, bytes.data(), bytes.size());
	par->extradata_size = int(bytes.size());
	return true;
}

[[nodiscard]] std::optional<ParameterSets> VideoSets(
		const Bytes &media,
		const RoundRecovery &recovery,
		int video) {
	const auto &track = recovery.config.tracks[video];
	if (track.csd.size() >= 2) {
		auto sets = ParameterSets{ track.csd[0], track.csd[1] };
		if (auto complete = Complete(std::move(sets))) {
			return complete;
		}
	}
	for (const auto &sample : recovery.samples) {
		if (sample.track != video || !sample.sync) {
			continue;
		}
		if (auto sets = ParameterSetsFromSample(media.data() + sample.offset, sample.size)) {
			return sets;
		}
	}
	return std::nullopt;
}

} // namespace

std::optional<ParameterSets> ParameterSetsFromExtradata(
		const std::uint8_t *data,
		std::size_t size) {
	if (!data || size < 7) {
		return std::nullopt;
	}
	auto sets = ParameterSets();
	if (data[0] == 1) {
		auto at = std::size_t(5);
		for (auto kind = 0; kind != 2; ++kind) {
			if (at >= size) {
				return std::nullopt;
			}
			const auto count = (kind == 0) ? (data[at] & 0x1F) : data[at];
			++at;
			for (auto i = 0; i != count; ++i) {
				if (at + 2 > size) {
					return std::nullopt;
				}
				const auto length = std::size_t(ReadBig(data + at, 2));
				at += 2;
				if (length > size - at) {
					return std::nullopt;
				}
				Collect(sets, data + at, length);
				at += length;
			}
		}
		return Complete(std::move(sets));
	}
	auto start = std::size_t(0);
	const auto next = [&](std::size_t from) {
		for (auto i = from; i + 3 <= size; ++i) {
			if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
				return i;
			}
		}
		return size;
	};
	start = next(0);
	while (start < size) {
		const auto begin = start + 3;
		auto end = next(begin);
		const auto following = end;
		while (end > begin && data[end - 1] == 0) {
			--end;
		}
		Collect(sets, data + begin, end - begin);
		start = following;
	}
	return Complete(std::move(sets));
}

std::optional<ParameterSets> ParameterSetsFromSample(
		const std::uint8_t *data,
		std::size_t size) {
	auto sets = ParameterSets();
	auto at = std::size_t(0);
	while (data && at + 4 <= size) {
		const auto length = std::size_t(ReadBig(data + at, 4));
		at += 4;
		if (length > size - at) {
			break;
		}
		Collect(sets, data + at, length);
		at += length;
	}
	return Complete(std::move(sets));
}

Bytes MakeAvcc(const ParameterSets &sets) {
	if (sets.sps.size() < 4 || sets.pps.empty()
		|| sets.sps.size() > 0xFFFF || sets.pps.size() > 0xFFFF) {
		return {};
	}
	auto result = Bytes{ 1, sets.sps[1], sets.sps[2], sets.sps[3], 0xFF, 0xE1 };
	const auto add = [&](const Bytes &nal) {
		result.push_back(std::uint8_t(nal.size() >> 8));
		result.push_back(std::uint8_t(nal.size() & 0xFF));
		result.insert(result.end(), nal.begin(), nal.end());
	};
	add(sets.sps);
	result.push_back(1);
	add(sets.pps);
	return result;
}

std::optional<RoundConfig> ConfigOf(const AVFormatContext *input) {
	if (!input || !input->nb_streams || input->nb_streams > 4) {
		return std::nullopt;
	}
	auto result = RoundConfig();
	result.avccVideo = true;
	for (auto i = 0; i != int(input->nb_streams); ++i) {
		const auto par = Parameters(input, i);
		const auto base = input->streams[i]->time_base;
		auto track = RoundTrack();
		track.timescale = (base.num == 1 && base.den > 0)
			? std::uint32_t(base.den)
			: 90000;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id == AV_CODEC_ID_H264) {
			track.kind = RoundTrackKind::Video;
			track.mime = "video/avc";
			track.width = std::uint32_t(std::max(par->width, 0));
			track.height = std::uint32_t(std::max(par->height, 0));
			result.width = track.width;
			result.height = track.height;
			if (const auto sets = ParameterSetsFromExtradata(par->extradata, std::size_t(std::max(par->extradata_size, 0)))) {
				track.csd = { sets->sps, sets->pps };
			}
		} else if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->codec_id == AV_CODEC_ID_AAC) {
			track.kind = RoundTrackKind::Audio;
			track.mime = "audio/mp4a-latm";
			track.sampleRate = std::uint32_t(std::max(par->sample_rate, 0));
			track.channels = std::uint32_t(std::max(par->ch_layout.nb_channels, 0));
			if (par->extradata && par->extradata_size > 0) {
				track.csd = { Bytes(par->extradata, par->extradata + par->extradata_size) };
			}
		} else {
			return std::nullopt;
		}
		result.tracks.push_back(std::move(track));
	}
	return result;
}

std::unique_ptr<CopyMuxer> CopyMuxer::Start(
		std::unique_ptr<RecordingCrypt::File> file,
		const AVFormatContext *input,
		int journalSeconds) {
	if (!file || !input || !input->nb_streams) {
		return nullptr;
	}
	auto result = std::unique_ptr<CopyMuxer>(new CopyMuxer());
	result->_file = std::move(file);
	if (avformat_alloc_output_context2(&result->_output, nullptr, "mp4", nullptr) < 0
		|| !result->_output) {
		return nullptr;
	}
	const auto buffer = static_cast<std::uint8_t*>(av_malloc(kBlock));
	if (!buffer) {
		return nullptr;
	}
	result->_io = avio_alloc_context(buffer, kBlock, 1, result.get(), nullptr, MuxerWrite, MuxerSeek);
	if (!result->_io) {
		av_free(buffer);
		return nullptr;
	}
	result->_output->pb = result->_io;
	result->_output->flags |= AVFMT_FLAG_CUSTOM_IO;
	for (auto i = 0; i != int(input->nb_streams); ++i) {
		const auto stream = avformat_new_stream(result->_output, nullptr);
		if (!stream || avcodec_parameters_copy(stream->codecpar, Parameters(input, i)) < 0) {
			return nullptr;
		}
		stream->codecpar->codec_tag = 0;
		stream->time_base = input->streams[i]->time_base;
		result->_inputBase.push_back(input->streams[i]->time_base);
		result->_lastDts.push_back(std::numeric_limits<std::int64_t>::min());
	}
	auto options = (AVDictionary*)nullptr;
	if (journalSeconds > 0) {
		av_dict_set(&options, "movflags", "delay_moov+default_base_moof", 0);
		av_dict_set(&options, "frag_duration", std::to_string(std::int64_t(journalSeconds) * 1'000'000).c_str(), 0);
	}
	const auto written = avformat_write_header(result->_output, &options);
	av_dict_free(&options);
	if (written < 0) {
		return nullptr;
	}
	result->_headerWritten = true;
	avio_flush(result->_io);
	return result;
}

CopyMuxer::~CopyMuxer() {
	release();
}

void CopyMuxer::release() {
	if (_output) {
		avformat_free_context(_output);
		_output = nullptr;
	}
	FreeIo(_io);
}

int CopyMuxer::writeBytes(const std::uint8_t *data, int size) {
	if (size <= 0) {
		return 0;
	} else if (_failed || !_file->writeAt(std::uint64_t(_position), data, std::size_t(size))) {
		_failed = true;
		return AVERROR(EIO);
	}
	_position += size;
	return size;
}

std::int64_t CopyMuxer::seek(std::int64_t offset, int whence) {
	auto updated = std::int64_t(-1);
	switch (whence & ~AVSEEK_FORCE) {
	case SEEK_SET: updated = offset; break;
	case SEEK_CUR: updated = _position + offset; break;
	case SEEK_END: updated = std::int64_t(_file->size()) + offset; break;
	case AVSEEK_SIZE: return std::int64_t(_file->size());
	}
	if (updated < 0) {
		return -1;
	}
	_position = updated;
	return updated;
}

bool CopyMuxer::write(const AVPacket *packet) {
	if (_failed || !_output || !packet
		|| packet->stream_index < 0
		|| packet->stream_index >= int(_inputBase.size())) {
		return false;
	}
	const auto index = packet->stream_index;
	auto copy = av_packet_clone(packet);
	if (!copy) {
		_failed = true;
		return false;
	}
	av_packet_rescale_ts(copy, _inputBase[index], _output->streams[index]->time_base);
	if (copy->dts == AV_NOPTS_VALUE) {
		copy->dts = copy->pts;
	}
	if (copy->dts != AV_NOPTS_VALUE && copy->dts <= _lastDts[index]) {
		copy->dts = _lastDts[index] + 1;
		copy->pts = std::max(copy->pts, copy->dts);
	}
	_lastDts[index] = copy->dts;
	const auto result = av_write_frame(_output, copy);
	av_packet_free(&copy);
	if (result < 0) {
		_failed = true;
		return false;
	}
	avio_flush(_io);
	return !_failed;
}

bool CopyMuxer::finish() {
	if (!_output) {
		return false;
	}
	const auto ok = !_failed
		&& _headerWritten
		&& (av_write_trailer(_output) >= 0);
	if (_io) {
		avio_flush(_io);
	}
	release();
	return ok && !_failed;
}

std::uint64_t CopyMuxer::size() const {
	return _file ? _file->size() : 0;
}

std::optional<Remuxed> Remux(const Bytes &bytes) {
	auto reader = Reader{ bytes.data(), std::int64_t(bytes.size()) };
	auto input = Input();
	if (!OpenInput(input, reader) || !input.format->nb_streams) {
		return std::nullopt;
	}
	auto writer = Writer();
	auto output = Output();
	if (!OpenOutput(output, writer)) {
		return std::nullopt;
	}
	const auto count = int(input.format->nb_streams);
	auto lastDts = std::vector<std::int64_t>(count, std::numeric_limits<std::int64_t>::min());
	for (auto i = 0; i != count; ++i) {
		const auto stream = avformat_new_stream(output.format, nullptr);
		if (!stream || avcodec_parameters_copy(stream->codecpar, Parameters(input.format, i)) < 0) {
			return std::nullopt;
		}
		stream->codecpar->codec_tag = 0;
		stream->time_base = input.format->streams[i]->time_base;
	}
	if (avformat_write_header(output.format, nullptr) < 0) {
		return std::nullopt;
	}
	auto result = Remuxed();
	auto packet = av_packet_alloc();
	if (!packet) {
		return std::nullopt;
	}
	while (true) {
		const auto read = av_read_frame(input.format, packet);
		if (read == AVERROR_EOF) {
			break;
		} else if (read < 0 || (packet->flags & AV_PKT_FLAG_CORRUPT)) {
			result.partial = true;
			av_packet_unref(packet);
			break;
		}
		const auto index = packet->stream_index;
		if (index < 0 || index >= count) {
			av_packet_unref(packet);
			continue;
		}
		const auto inBase = input.format->streams[index]->time_base;
		const auto outBase = output.format->streams[index]->time_base;
		result.durationMs = std::max(result.durationMs, EndMs(inBase, packet->pts, packet->duration));
		if (Parameters(input.format, index)->codec_type == AVMEDIA_TYPE_VIDEO) {
			++result.videoPackets;
		}
		av_packet_rescale_ts(packet, inBase, outBase);
		if (packet->dts == AV_NOPTS_VALUE) {
			packet->dts = packet->pts;
		}
		if (packet->dts <= lastDts[index]) {
			packet->dts = lastDts[index] + 1;
			packet->pts = std::max(packet->pts, packet->dts);
		}
		lastDts[index] = packet->dts;
		const auto written = av_write_frame(output.format, packet);
		av_packet_unref(packet);
		if (written < 0) {
			av_packet_free(&packet);
			return std::nullopt;
		}
	}
	av_packet_free(&packet);
	if (av_write_trailer(output.format) < 0) {
		return std::nullopt;
	}
	avio_flush(output.io);
	result.bytes = std::move(writer.bytes);
	return result;
}

std::optional<Remuxed> Rebuild(
		const Bytes &media,
		const RoundRecovery &recovery) {
	const auto video = recovery.config.videoTrack();
	if (video < 0 || recovery.samples.empty()) {
		return std::nullopt;
	}
	const auto sets = VideoSets(media, recovery, video);
	if (!sets) {
		return std::nullopt;
	}
	auto writer = Writer();
	auto output = Output();
	if (!OpenOutput(output, writer)) {
		return std::nullopt;
	}
	auto streams = std::vector<int>(recovery.config.tracks.size(), -1);
	for (auto i = 0; i != int(recovery.config.tracks.size()); ++i) {
		const auto &track = recovery.config.tracks[i];
		const auto stream = avformat_new_stream(output.format, nullptr);
		if (!stream) {
			return std::nullopt;
		}
		const auto par = stream->codecpar;
		if (track.kind == RoundTrackKind::Video) {
			par->codec_type = AVMEDIA_TYPE_VIDEO;
			par->codec_id = AV_CODEC_ID_H264;
			par->width = int(track.width ? track.width : recovery.config.width);
			par->height = int(track.height ? track.height : recovery.config.height);
			par->format = AV_PIX_FMT_YUV420P;
			if (!SetExtradata(par, MakeAvcc(*sets))) {
				return std::nullopt;
			}
			stream->time_base = AVRational{ 1, 90000 };
		} else {
			if (track.csd.empty() || !track.sampleRate) {
				return std::nullopt;
			}
			par->codec_type = AVMEDIA_TYPE_AUDIO;
			par->codec_id = AV_CODEC_ID_AAC;
			par->sample_rate = int(track.sampleRate);
			par->frame_size = 1024;
			av_channel_layout_default(&par->ch_layout, std::max(int(track.channels), 1));
			if (!SetExtradata(par, track.csd.front())) {
				return std::nullopt;
			}
			stream->time_base = AVRational{ 1, int(track.sampleRate) };
		}
		streams[i] = stream->index;
	}
	if (avformat_write_header(output.format, nullptr) < 0) {
		return std::nullopt;
	}
	auto result = Remuxed();
	auto lastDts = std::vector<std::int64_t>(streams.size(), std::numeric_limits<std::int64_t>::min());
	auto packet = av_packet_alloc();
	if (!packet) {
		return std::nullopt;
	}
	for (const auto &sample : recovery.samples) {
		if (sample.track >= streams.size()
			|| sample.offset > media.size()
			|| sample.size > media.size() - sample.offset
			|| av_new_packet(packet, int(sample.size)) < 0) {
			av_packet_free(&packet);
			return std::nullopt;
		}
		std::memcpy(packet->data, media.data() + sample.offset, sample.size);
		const auto index = streams[sample.track];
		const auto base = output.format->streams[index]->time_base;
		const auto audio = (recovery.config.tracks[sample.track].kind == RoundTrackKind::Audio);
		packet->stream_index = index;
		packet->pts = av_rescale_q(sample.ptsUs, kMicroseconds, base);
		if (packet->pts <= lastDts[sample.track]) {
			packet->pts = lastDts[sample.track] + 1;
		}
		packet->dts = packet->pts;
		packet->duration = audio ? 1024 : 0;
		lastDts[sample.track] = packet->dts;
		if (sample.sync) {
			packet->flags |= AV_PKT_FLAG_KEY;
		}
		if (!audio) {
			++result.videoPackets;
		}
		result.durationMs = std::max(result.durationMs, EndMs(base, packet->pts, packet->duration));
		const auto written = av_write_frame(output.format, packet);
		av_packet_unref(packet);
		if (written < 0) {
			av_packet_free(&packet);
			return std::nullopt;
		}
	}
	av_packet_free(&packet);
	if (av_write_trailer(output.format) < 0) {
		return std::nullopt;
	}
	avio_flush(output.io);
	result.bytes = std::move(writer.bytes);
	result.partial = (recovery.status == RoundRecoveryStatus::Partial);
	return result;
}

AacFrameMeasure MakeAacMeasure() {
	struct Cache {
		std::unique_ptr<AacMeasure> measure;
		std::vector<Bytes> config;
	};
	const auto cache = std::make_shared<Cache>();
	return [=](const RoundTrack &audio, const std::uint8_t *data, std::size_t available) {
		if (!cache->measure || cache->config != audio.csd) {
			cache->measure = std::make_unique<AacMeasure>(audio);
			cache->config = audio.csd;
		}
		return cache->measure->measure(data, available);
	};
}

RecordingRecovery::Result Recover(
		const std::filesystem::path &media,
		const std::filesystem::path &journal,
		std::shared_ptr<const RecordingCrypt::Key> key,
		const RecordingCrypt::RandomFill &random,
		bool wasSending) {
	auto result = RecordingRecovery::Result();
	const auto bytes = RecordingCrypt::ReadWhole(media, key);
	if (!bytes) {
		return result;
	}
	auto rebuilt = Remux(*bytes);
	if (!rebuilt || rebuilt->videoPackets <= 0) {
		const auto journalBytes = journal.empty()
			? std::nullopt
			: RecordingCrypt::ReadWhole(journal, key);
		const auto recovery = RecoverRound(
			bytes->data(),
			bytes->size(),
			journalBytes ? journalBytes->data() : nullptr,
			journalBytes ? journalBytes->size() : 0,
			MakeAacMeasure());
		if (recovery.status == RoundRecoveryStatus::Unrecoverable) {
			return result;
		}
		rebuilt = Rebuild(*bytes, recovery);
	}
	if (!rebuilt || rebuilt->videoPackets <= 0) {
		return result;
	}
	const auto changed = (rebuilt->bytes != *bytes);
	if (changed && !RecordingCrypt::WriteWhole(
			media,
			key,
			random,
			rebuilt->bytes.data(),
			rebuilt->bytes.size())) {
		return result;
	}
	result.state = wasSending ? State::Unsent : State::Recovered;
	result.durationMs = rebuilt->durationMs;
	result.changed = changed;
	result.partial = rebuilt->partial;
	return result;
}

} // namespace Purple::RoundMedia
