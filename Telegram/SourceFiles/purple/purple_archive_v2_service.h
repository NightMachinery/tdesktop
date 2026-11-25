#pragma once

#include "purple/purple_archive_v2_writer.h"
#include "data/data_types.h"

class PeerData;
namespace Main { class Account; class Session; }
namespace Ui { class GenericBox; }
namespace Window { class SessionController; }

namespace Purple::ArchiveV2 {

void StartServices();
void StartSession(not_null<Main::Session*> session);
void Logout(Main::Account &account);
[[nodiscard]] bool DrainOnQuit();
void CloseViews(Main::Session *session = nullptr);
[[nodiscard]] uint64_t ViewEpoch();
[[nodiscard]] PeerToken Peer(PeerId id);
[[nodiscard]] ChatClass Classify(PeerData *peer);
[[nodiscard]] const Policy &CurrentPolicy(not_null<Main::Session*> session);
[[nodiscard]] bool Queue(not_null<Main::Session*> session, CapturedEvent event);
[[nodiscard]] bool Query(not_null<Main::Session*> session, Writer::ReadOrCleanup operation);
[[nodiscard]] StoreEffect QueueBlob(not_null<Main::Session*> session, BlobInput blob);
[[nodiscard]] bool Downloading(Main::Session *session, const MessageIdentity &item, int64 sequence, int64 ordinal);
[[nodiscard]] bool Saving(Main::Session *session, const MessageIdentity &item, int64 sequence, int64 ordinal);
[[nodiscard]] std::shared_ptr<Lease> SessionLease(Main::Session *session);
void RegisterView(not_null<Main::Session*> session, not_null<Ui::GenericBox*> box);
void PublishIncident(Main::Session *session, const StoreEffect &effect, ArchiveWarning warning);
void RequestReset(not_null<Main::Session*> session, ResetReview review);

}
