#pragma once

#include "purple/purple_archive_v2_media.h"
#include <utility>

namespace Purple::ArchiveV2 {

enum class IncidentStage { Startup, Capture, WholeFile, Maintenance, Cleanup };
enum class ArchiveWarning { None, MessageLost, CapturePaused };

struct Incident {
	QStringView reason;
	ArchiveWarning warning = ArchiveWarning::None;
};

class Attention final {
public:
	[[nodiscard]] Incident observe(IncidentStage stage, QStringView reason, bool committed) {
		if (reason.isEmpty()) {
			const auto idle = stage == IncidentStage::Startup || stage == IncidentStage::Maintenance;
			if (idle) _lost = false;
			if (_paused && (idle || (stage == IncidentStage::Capture && committed))) {
				_paused = false;
				return { u"capture_resumed" };
			}
			return {};
		}
		for (const auto loss : { u"text_queue_full", u"text_encode_failed", u"text_write_failed" }) {
			if (reason == loss && stage == IncidentStage::Capture) {
				return { loss, std::exchange(_lost, true) ? ArchiveWarning::None : ArchiveWarning::MessageLost };
			}
		}
		for (const auto fault : { u"disk_full", u"key_unavailable", u"store_busy", u"store_unreadable",
				u"schema_unknown", u"pointer_authentication_failed", u"quota_unreclaimable", u"cleanup_pending", u"cleanup_blocked" }) {
			if (reason == fault) {
				const auto stopsCapture = stage == IncidentStage::Startup
					|| ((stage == IncidentStage::Capture || stage == IncidentStage::Maintenance)
						&& reason != u"cleanup_pending" && reason != u"cleanup_blocked");
				return { fault, stopsCapture && !std::exchange(_paused, true)
					? ArchiveWarning::CapturePaused : ArchiveWarning::None };
			}
		}
		for (const auto detail : { u"policy_description_only", u"policy_invalid", u"over_file_limit", u"over_allowance",
				u"no_source", u"expired", u"transfer_failed", u"copy_incomplete", u"scope_retired",
				u"delete_unresolved", u"media_queue_full", u"pointer_changed", u"reset_review_stale" }) {
			if (reason == detail) return { detail };
		}
		return { u"archive_operation_failed" };
	}
private:
	bool _paused = false;
	bool _lost = false;
};

}
