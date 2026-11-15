#include "purple/purple_playlist_download_registry.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <algorithm>
#include <vector>

namespace Purple {
namespace {

struct JobRegistration {
	Main::Session *session = nullptr;
	QString playlistId;
	std::weak_ptr<void> alive;
	Fn<void()> cancel;
};

std::vector<JobRegistration> Jobs;
std::vector<std::weak_ptr<void>> TransferSlots;

void RequireMainThread() {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
}

} // namespace

std::shared_ptr<void> AcquirePlaylistOwnedTransferSlot() {
	RequireMainThread();
	std::erase_if(TransferSlots, [](const auto &slot) { return slot.expired(); });
	if (TransferSlots.size() == 2) return nullptr;
	auto slot = std::make_shared<int>(0);
	TransferSlots.push_back(slot);
	return slot;
}

void RegisterPlaylistDownloadJob(
		Main::Session *session, QString playlistId,
		std::weak_ptr<void> alive, Fn<void()> cancel) {
	RequireMainThread();
	std::erase_if(Jobs, [](const auto &job) { return job.alive.expired(); });
	Jobs.push_back({ session, std::move(playlistId), std::move(alive), std::move(cancel) });
}

void StopAccountPlaylistDownloads(
		Main::Session *session,
		const std::optional<QString> &playlistId) {
	RequireMainThread();
	auto cancel = std::vector<Fn<void()>>();
	for (auto i = Jobs.begin(); i != Jobs.end();) {
		if (i->alive.expired()) {
			i = Jobs.erase(i);
		} else if (i->session == session && (!playlistId || i->playlistId == *playlistId)) {
			cancel.push_back(std::move(i->cancel));
			i = Jobs.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &callback : cancel) callback();
}

void InvalidatePlaylistDownloads() {
	RequireMainThread();
	const auto jobs = std::move(Jobs);
	Jobs.clear();
	for (const auto &job : jobs) {
		if (!job.alive.expired()) job.cancel();
	}
}

} // namespace Purple
