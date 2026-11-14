#include "purple/purple_playlists_service.h"

#include "purple/purple_config.h"
#include "base/weak_ptr.h"
#include "crl/crl_object_on_thread.h"
#include "main/main_account.h"
#include "main/main_session.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

namespace Purple {
namespace {

struct AccountService {
	PlaylistStoreRoute route;
	std::shared_ptr<std::atomic<bool>> authority =
		std::make_shared<std::atomic<bool>>(true);
	crl::object_on_thread<PlaylistAccountWorker> worker;

	explicit AccountService(PlaylistStoreRoute route)
	: route(std::move(route))
	, worker(PlaylistStoreRoute(this->route), std::shared_ptr<std::atomic<bool>>(authority)) {
	}

	~AccountService() {
		authority->store(false, std::memory_order_release);
	}
};

std::map<Main::Session*, std::unique_ptr<AccountService>> Services;

void RequireMainThread() {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
}

AccountService *For(not_null<Main::Session*> session) {
	RequireMainThread();
	const auto routing = CaptureAccountSyncRouting(session);
	auto i = Services.find(session.get());
	if (i != Services.end()
		&& (!routing || i->second->route.account != SessionAccountKey(session)
			|| !SameAccountRoutingSnapshot(i->second->route.routing, *routing))) {
		Services.erase(i);
		i = Services.end();
	}
	if (!routing) {
		return nullptr;
	} else if (i == Services.end()) {
		auto value = std::make_unique<AccountService>(PlaylistStoreRoute{
			.routing = *routing,
			.account = SessionAccountKey(session),
		});
		i = Services.emplace(session.get(), std::move(value)).first;
		const auto key = session.get();
		session->lifetime().add([key] { Services.erase(key); });
	}
	return i->second.get();
}

void Request(
		not_null<Main::Session*> session,
		std::optional<PlaylistAccountEdit> request,
		PlaylistAccountDone done) {
	const auto service = For(session);
	if (!service) {
		done({ .snapshot = { .status = PlaylistStoreStatus::Changed } });
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto route = service->route;
	const auto authority = service->authority;
	service->worker.with([
		weak, route, authority, request = std::move(request), done = std::move(done)
	](PlaylistAccountWorker &worker) mutable {
		auto result = request ? worker.edit(*request) : worker.read();
		crl::on_main([
			weak, route, authority, result = std::move(result), done = std::move(done)
		]() mutable {
			const auto session = weak.get();
			if (!session) {
				return;
			} else if (!authority->load(std::memory_order_acquire)
				|| SessionAccountKey(session) != route.account
				|| !AccountSyncRoutingCurrent(session, route.routing)) {
				result.snapshot = { .status = PlaylistStoreStatus::Changed };
			}
			done(std::move(result));
		});
	});
}

} // namespace

void ReadAccountPlaylists(
		not_null<Main::Session*> session,
		PlaylistAccountDone done) {
	Request(session, std::nullopt, std::move(done));
}

void EditAccountPlaylists(
		not_null<Main::Session*> session,
		PlaylistAccountEdit request,
		PlaylistAccountDone done) {
	Request(session, std::move(request), std::move(done));
}

void InvalidatePlaylistServices() {
	RequireMainThread();
	Services.clear();
}

void StopAccountPlaylists(Main::Account &account) {
	RequireMainThread();
	if (const auto session = account.maybeSession()) {
		Services.erase(session);
	}
}

} // namespace Purple
