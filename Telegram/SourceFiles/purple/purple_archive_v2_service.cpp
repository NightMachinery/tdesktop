#include "purple/purple_archive_v2_service.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "purple/purple_archive_v2_ui.h"
#include "purple/purple_archive_previous.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_config.h"
#include "purple/purple_notices.h"
#include "purple/purple_privacy_screen.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "window/window_controller.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <map>

namespace Purple::ArchiveV2 {
namespace {

struct AccountArchive {
	std::shared_ptr<Lease> lease;
	std::unique_ptr<Writer> writer;
	std::vector<QPointer<Ui::GenericBox>> views;
	Policy policy;
};
std::map<Main::Session*, AccountArchive> Accounts;
std::atomic<uint64_t> ArchiveViewEpoch = 0;
bool RecoveringPrevious = false;

void RecoverPrevious() {
	if (RecoveringPrevious || Core::Quitting() || !Core::App().domain().started()) return;
	RecoveringPrevious = true;
	const auto profile = PreviousArchiveOptions{ .archiveRoot = ConfigDirectory() + u"/content-archive"_q,
		.profileRoot = cWorkingDir() + u"tdata"_q, .profileNamespace = cDataFile() };
	crl::async([profile] {
		const auto retry = PreviousArchive::RetryPreviousArchiveDeletions(profile);
		crl::on_main([retry] {
			RecoveringPrevious = false;
			if (!Core::Quitting() && retry) QTimer::singleShot(15000, &Core::App(), RecoverPrevious);
		});
	});
}

AccountArchive *Find(Main::Session *session) {
	const auto found = Accounts.find(session);
	return found != end(Accounts) ? &found->second : nullptr;
}

QByteArray CopyKey(const MTP::AuthKey &key) {
	const auto bytes = key.data();
	return QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

StoreOptions Options(not_null<Main::Session*> session) {
	const auto profile = session->local().peekLegacyLocalKey();
	auto result = StoreOptions{
		.root = ConfigDirectory() + u"/content-archive-v2"_q,
		.account = SessionAccountKey(session),
		.profileKey = profile ? CopyKey(*profile) : QByteArray(),
		.profilePath = cWorkingDir() + u"tdata"_q,
		.profileNamespace = cDataFile().toUtf8(),
		.policy = AccountSettings(session).archive,
	};
	if (!result.policy.valid) result.policy = Policy();
	const auto &mtp = session->account().mtp();
	for (const auto &key : mtp.getKeysForWrite()) {
		if (!key || key->type() == MTP::AuthKey::Type::Temporary || key->type() == MTP::AuthKey::Type::Local) continue;
		const auto tag = QByteArray::number(key->keyId(), 16).rightJustified(16, '0');
		auto pair = std::make_pair(tag, CopyKey(*key));
		if (key->dcId() == mtp.mainDcId()) result.loginKeys.insert(result.loginKeys.begin(), std::move(pair));
		else result.loginKeys.push_back(std::move(pair));
	}
	return result;
}

}

PeerToken Peer(PeerId id) {
	return peerIsChannel(id) ? PeerToken{ PeerTokenKind::Channel, peerToChannel(id).bare }
		: peerIsChat(id) ? PeerToken{ PeerTokenKind::Chat, peerToChat(id).bare }
		: PeerToken{ PeerTokenKind::User, peerToUser(id).bare };
}

ChatClass Classify(PeerData *peer) {
	if (!peer) return ChatClass::Unknown;
	if (const auto user = peer->asUser()) return user->isBot() ? ChatClass::Bot : ChatClass::Direct;
	return peer->isBroadcast() ? ChatClass::Channel : ChatClass::Group;
}

void StartSession(not_null<Main::Session*> session) {
	if (Core::Quitting() || session->account().loggingOut() || Find(session.get())) return;
	auto options = Options(session);
	const auto entry = Accounts.try_emplace(session.get()).first;
	auto &state = entry->second;
	state.lease = options.lease;
	state.policy = options.policy;
	const auto weak = base::make_weak(session.get());
	const auto route = state.lease;
	state.writer = std::make_unique<Writer>(std::move(options),
		[weak, route, attention = Attention()](StoreEffect effect, QByteArray) mutable {
			const auto warning = LogIncident(attention, effect.stage, effect.reason, effect.committed,
				std::nullopt, effect.count);
			if (warning == ArchiveWarning::None && effect.stage != IncidentStage::Cleanup) return;
			crl::on_main([=] {
				const auto live = weak.get();
				if (!live || Core::Quitting() || SessionLease(live) != route) return;
				if (effect.stage == IncidentStage::Cleanup && effect.committed && !live->account().loggingOut()) {
					CloseViews(live);
					CancelArchiveDownloads(live);
					Accounts.erase(live);
					StartSession(live);
					if (effect) {
						if (const auto window = PurpleNoticeWindow(live)) {
							window->showToast(ArchiveCleanupText(tr::lng_purple_archive_v2_reset_done(tr::now), effect.reclaimed));
						}
					}
				}
				PublishIncident(live, effect, warning);
			});
		});
	SettingsChanges() | rpl::on_next([weak, route] {
		const auto live = weak.get();
		const auto state = Find(live);
		if (!state || state->lease != route) return;
		const auto policy = AccountSettings(live).archive;
		if (!policy.valid || state->policy == policy) return;
		if (state->writer->query({}, [policy](Store &store) { (void)store.updatePolicy(policy); })) {
			state->lease->mediaRevision.fetch_add(1);
			CancelArchiveDownloads(live);
			state->policy = policy;
		}
	}, session->lifetime());
	session->lifetime().add([session, route] {
		if (SessionLease(session.get()) != route) return;
		CloseViews(session.get());
		CancelArchiveDownloads(session.get());
		Accounts.erase(session.get());
	});
}

void StartServices() {
	static auto lifetime = rpl::lifetime();
	const auto refresh = [] { CloseViews(); RecoverPrevious(); };
	Core::App().domain().activeSessionChanges() | rpl::on_next(refresh, lifetime);
	PrivacyScreenChanges() | rpl::filter(rpl::mappers::_1) | rpl::on_next([] { CloseViews(); }, lifetime);
	RecoverPrevious();
}

void Logout(Main::Account &account) {
	if (const auto session = account.maybeSession()) {
		if (const auto state = Find(session)) {
			state->writer->logout({});
			CloseViews(session);
			CancelArchiveDownloads(session);
			Accounts.erase(session);
		}
	}
}

bool DrainOnQuit() {
	static const auto deadline = [] {
		for (const auto &[session, state] : Accounts) CancelArchiveDownloads(session);
		return std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
	}();
	auto pending = false;
	for (const auto &[session, state] : Accounts) pending = state.writer->drainUntil(deadline) || pending;
	if (pending) QTimer::singleShot(20, &Core::App(), [] { Core::App().quitPreventFinished(); });
	return pending;
}

uint64_t ViewEpoch() { return ArchiveViewEpoch.load(); }

void CloseViews(Main::Session *session) {
	ArchiveViewEpoch.fetch_add(1);
	auto closing = std::vector<QPointer<Ui::GenericBox>>();
	for (auto &[key, state] : Accounts) {
		if (session && key != session) continue;
		closing.insert(closing.end(), state.views.begin(), state.views.end());
		state.views.clear();
	}
	for (const auto &box : closing) {
		if (box) box->closeBox();
	}
}

void RegisterView(not_null<Main::Session*> session, not_null<Ui::GenericBox*> box) {
	if (const auto state = Find(session.get())) {
		std::erase_if(state->views, [](const auto &entry) { return !entry; });
		state->views.push_back(box.get());
	}
}

std::shared_ptr<Lease> SessionLease(Main::Session *session) {
	const auto state = Find(session);
	return state ? state->lease : nullptr;
}

const Policy &CurrentPolicy(not_null<Main::Session*> session) {
	const auto state = Find(session.get());
	return state ? state->policy : AccountSettings(session).archive;
}

bool Queue(not_null<Main::Session*> session, CapturedEvent event) {
	const auto state = Find(session.get());
	return state && bool(state->writer->post(std::move(event)));
}

bool Query(not_null<Main::Session*> session, Writer::ReadOrCleanup operation) {
	const auto state = Find(session.get());
	return state && state->writer->query({}, std::move(operation));
}

StoreEffect QueueBlob(not_null<Main::Session*> session, BlobInput blob) {
	const auto state = Find(session.get());
	return state ? state->writer->postBlob(std::move(blob)) : StoreEffect{ u"scope_retired"_q };
}

bool Saving(Main::Session *session, const MessageIdentity &item, int64 sequence, int64 ordinal) {
	const auto state = Find(session);
	return state && (Downloading(session, item, sequence, ordinal) || state->writer->saving(item, sequence, ordinal));
}

void RequestReset(not_null<Main::Session*> session, ResetReview review) {
	const auto state = Find(session.get());
	if (state && state->writer->reset(std::move(review))) return;
	PublishIncident(session.get(), { u"store_busy"_q, false, IncidentStage::Cleanup }, ArchiveWarning::None);
}

void PublishIncident(Main::Session *session, const StoreEffect &effect, ArchiveWarning warning) {
	if (!session || Core::Quitting() || (effect && warning == ArchiveWarning::None)) return;
	auto summary = warning == ArchiveWarning::MessageLost ? tr::lng_purple_archive_gap(tr::now)
		: warning == ArchiveWarning::CapturePaused ? tr::lng_purple_archive_unavailable(tr::now)
		: effect.stage == IncidentStage::Cleanup && effect.committed
			? tr::lng_purple_archive_v2_reset_pending(tr::now)
		: effect.reason.startsWith(u"discard-") ? tr::lng_purple_archive_v2_previous_failed(tr::now)
		: tr::lng_purple_archive_discard_failed(tr::now);
	summary += u" ("_q + effect.reason + u", "_q + QString::number(effect.count) + ')';
	ShowPurpleNotice({
		.session = session,
		.scope = tr::lng_purple_settings_archive(tr::now),
		.summary = std::move(summary),
		.coalesceKey = u"archive:v2:"_q + effect.reason,
		.severity = ModalNotices::NoticeSeverity::Warning,
		.background = true,
	});
}

}
