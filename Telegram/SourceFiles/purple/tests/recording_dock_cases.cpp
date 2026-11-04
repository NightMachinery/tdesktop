#include "purple/purple_recording_dock_model.h"
#include "purple/purple_round_recording_geometry.h"
#include <cstdio>

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool result) { ++checks; if (!result) ++failures; };
	auto mailbox = Purple::RecordingTargetMailbox();
	check(!mailbox.get());
	mailbox.set(Purple::RecordingCaptureTarget{ 1, 9, 101, 22, 31 });
	const auto firstTarget = mailbox.get();
	mailbox.set(Purple::RecordingCaptureTarget{ 2, 10, 202, 0, 0 });
	check(firstTarget->account == 1 && firstTarget->user == 9);
	check(firstTarget->peer == 101 && firstTarget->topic == 22 && firstTarget->reply == 31);
	check(mailbox.get()->account == 2 && mailbox.get()->peer == 202);
	mailbox.set(std::nullopt); check(!mailbox.get());
	check(firstTarget->peer == 101);
	auto model = Purple::RecordingDockModel();
	check(!model.active()); check(!model.accepts(0));
	check(model.begin()); const auto first = model.generation();
	check(!model.begin()); check(model.accepts(first));
	check(model.pause(first, true)); check(model.paused());
    const auto revision = model.pauseRevision();
	check(!model.pause(first - 1, false)); check(model.paused());
	check(model.pause(first, false)); check(!model.paused());
    check(model.pauseRevision() != revision);
	check(model.finish(first)); check(model.finishing());
	check(!model.finish(first)); check(!model.pause(first, true));
	check(!model.accepts(first)); model.clear(); check(!model.active());
	check(model.begin()); check(!model.accepts(first));
	check(!model.finish(first)); check(model.accepts(model.generation()));
	for (auto corner = 0; corner != 4; ++corner) {
		for (const auto width : { 0, 80, 160, 600, 1200 }) {
			for (const auto height : { 0, 90, 160, 500, 900 }) {
				auto geometry = Purple::RoundRecordingGeometry{ corner };
				geometry.place(width, height, 160, 12);
				check(geometry.x >= 0 && geometry.x <= std::max(0, width - 160));
				check(geometry.y >= 0 && geometry.y <= std::max(0, height - 160));
				geometry.x = -50; geometry.y = 2000;
				geometry.clamp(width, height, 160);
				check(geometry.x == 0 && geometry.y == std::max(0, height - 160));
			}
		}
	}
	for (auto corner = 0; corner != 4; ++corner) {
		auto geometry = Purple::RoundRecordingGeometry{ corner };
		geometry.place(800, 600, 160, 12);
		geometry.snap(800, 600, 160, 12);
		check(geometry.corner == corner);
	}
	std::printf("RECORDING_DOCK_CHECKS=%d\n%d checks, %d failures\n", checks, checks, failures);
	return failures ? 1 : 0;
}
