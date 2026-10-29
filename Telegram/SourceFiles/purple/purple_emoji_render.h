#pragma once

#include "purple/purple_control_protocol.h"
#include "purple/purple_emoji_export_store.h"
#include "purple/purple_emoji_large.h"
#include <map>

namespace Purple {

inline constexpr auto kEmojiRenderPending = size_t(64);
inline constexpr auto kEmojiRenderTimeoutMs = qint64(8000);

struct EmojiRenderJob {
	quint64 serial = 0;
	quint64 id = 0;
	int pixels = kEmojiLargePixels;
	EmojiExportEntry source;
};

class EmojiRenderRequests final {
public:
	[[nodiscard]] bool full() const;
	[[nodiscard]] size_t size() const;
	void add(quint64 id, int pixels, qint64 now,
		std::function<bool()> active, ControlReply reply);
	void poll(qint64 now);
	void fail(quint64 id, ControlResult result);
	void clear(ControlResult result);
	void published(const std::vector<EmojiExportEntry> &entries);
	[[nodiscard]] std::optional<EmojiRenderJob> take();
	void complete(quint64 serial, qint64 now, ControlResult result);

private:
	struct Pending {
		EmojiRenderJob job;
		qint64 deadline = 0;
		std::function<bool()> active;
		ControlReply reply;
		bool working = false;
	};
	quint64 _serial = 0;
	std::map<quint64, Pending> _pending;

};

[[nodiscard]] ControlResult PublishEmojiRender(
	const QString &directory,
	std::vector<EmojiExportEntry> entries,
	const EmojiRenderJob &job,
	qint64 maximumBytes,
	const std::function<bool()> &current,
	const QString &controlSocket);

}
