#include "purple/purple_emoji_render.h"

#include "purple/purple_emoji_export_path.h"
#include "purple/purple_emoji_large_store.h"
#include "base/basic_types.h"
#include <QtCore/QBuffer>
#include <QtGui/QImageReader>
#include <algorithm>

namespace Purple {
namespace {

bool Active(const std::function<bool()> &active) {
	return !active || active();
}

ControlResult Timeout() {
	return ControlError(u"timeout"_q, u"Emoji rendering did not finish in time."_q);
}

}

bool EmojiRenderRequests::full() const {
	return _pending.size() >= kEmojiRenderPending;
}

size_t EmojiRenderRequests::size() const {
	return _pending.size();
}

void EmojiRenderRequests::add(quint64 id, int pixels, qint64 now,
		std::function<bool()> active, ControlReply reply) {
	if (!Active(active)) return;
	if (full()) {
		reply(ControlError(u"busy"_q, u"The emoji render queue is full."_q));
		return;
	}
	const auto serial = ++_serial;
	_pending.emplace(serial, Pending{
		{ serial, id, pixels }, now + kEmojiRenderTimeoutMs,
		std::move(active), std::move(reply),
	});
}

void EmojiRenderRequests::poll(qint64 now) {
	auto expired = std::vector<Pending>();
	for (auto i = _pending.begin(); i != _pending.end();) {
		if (!Active(i->second.active)) {
			i = _pending.erase(i);
		} else if (now >= i->second.deadline) {
			expired.push_back(std::move(i->second));
			i = _pending.erase(i);
		} else {
			++i;
		}
	}
	for (auto &pending : expired) {
		if (Active(pending.active)) pending.reply(Timeout());
	}
}

void EmojiRenderRequests::fail(quint64 id, ControlResult result) {
	auto failed = std::vector<Pending>();
	for (auto i = _pending.begin(); i != _pending.end();) {
		if (i->second.job.id == id) {
			failed.push_back(std::move(i->second));
			i = _pending.erase(i);
		} else {
			++i;
		}
	}
	for (auto &pending : failed) {
		if (Active(pending.active)) pending.reply(result);
	}
}

void EmojiRenderRequests::clear(ControlResult result) {
	auto pending = std::exchange(_pending, {});
	for (auto &[serial, request] : pending) {
		if (Active(request.active)) request.reply(result);
	}
}

void EmojiRenderRequests::published(const std::vector<EmojiExportEntry> &entries) {
	for (auto &[serial, request] : _pending) {
		if (request.working) continue;
		const auto id = request.job.id;
		const auto i = std::find_if(entries.begin(), entries.end(), [=](const auto &entry) {
			return entry.id == id && !entry.large.isEmpty();
		});
		if (i != entries.end()) request.job.source = *i;
	}
}

std::optional<EmojiRenderJob> EmojiRenderRequests::take() {
	for (auto &[serial, request] : _pending) {
		if (request.working || request.job.source.large.isEmpty()) continue;
		request.working = true;
		return request.job;
	}
	return {};
}

void EmojiRenderRequests::complete(quint64 serial, qint64 now, ControlResult result) {
	const auto i = _pending.find(serial);
	if (i == _pending.end()) return;
	auto pending = std::move(i->second);
	_pending.erase(i);
	if (Active(pending.active)) {
		pending.reply(now >= pending.deadline ? Timeout() : std::move(result));
	}
}

ControlResult PublishEmojiRender(const QString &directory,
		std::vector<EmojiExportEntry> entries, const EmojiRenderJob &job,
		qint64 maximumBytes, const std::function<bool()> &current,
		const QString &controlSocket) {
	const auto unavailable = [] {
		return ControlError(u"not-found"_q, u"The published emoji image is unavailable; retry."_q);
	};
	const auto &source = job.source;
	const auto parent = EmojiExportDirectoryId(directory + u"/large"_q);
	if (!Active(current) || !parent || !source.sourcePx
		|| source.id != job.id || job.pixels < 1 || job.pixels > kEmojiLargePixels
		|| source.large != EmojiLargePngName(job.id, *source.sourcePx)) return unavailable();
	const auto path = directory + '/' + source.large;
	const auto info = QFileInfo(path);
	if (!info.isFile() || info.isSymLink() || info.size() <= 0
		|| info.size() > 4 * 1024 * 1024) return unavailable();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || !EmojiExportParentMatches(path, parent)) return unavailable();
	auto bytes = file.read(4 * 1024 * 1024 + 1);
	if (bytes.size() > 4 * 1024 * 1024) return unavailable();
	auto buffer = QBuffer(&bytes);
	if (!buffer.open(QIODevice::ReadOnly)) return unavailable();
	auto reader = QImageReader(&buffer, "PNG");
	const auto size = reader.size();
	if (size.isEmpty() || size.width() > kEmojiLargePixels || size.height() > kEmojiLargePixels) return unavailable();
	auto image = reader.read();
	if (image.isNull() || !Active(current)) return unavailable();
	if (std::max(image.width(), image.height()) > job.pixels) {
		const auto size = image.size().scaled(job.pixels, job.pixels, Qt::KeepAspectRatio)
			.expandedTo(QSize(1, 1));
		image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	}
	const auto pixels = std::max(image.width(), image.height());
	const auto relative = EmojiLargePngName(job.id, pixels);
	if (!WriteEmojiLargePng(directory, job.id, image, {}, current)
		|| !PruneEmojiLargeExport(directory, maximumBytes, current)) return unavailable();
	const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
		return entry.id == job.id;
	});
	if (entry == entries.end()) return unavailable();
	entry->large = relative;
	entry->sourcePx = pixels;
	if (!PublishEmojiLargeRequests(directory, entries, QDateTime::currentDateTimeUtc(),
		{}, current, controlSocket)) return unavailable();
	const auto advertised = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
		return entry.id == job.id && entry.large == relative;
	});
	const auto output = directory + '/' + relative;
	const auto usable = QFileInfo(output);
	if (advertised == entries.end() || !Active(current)
		|| !EmojiExportParentMatches(output, parent)
		|| !usable.isFile() || usable.isSymLink() || usable.size() <= 0) return unavailable();
	return { QJsonObject{
		{ u"path"_q, output }, { u"px"_q, pixels }, { u"sourceKind"_q, source.sourceKind },
	} };
}

}
