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
	const auto original = Purple::RecordingCaptureTarget{ 1, 9, 101, 22, 31 };
	auto sameThread = original; sameThread.reply = 88;
	check(Purple::SameRecordingThread(original, sameThread));
	sameThread.topic = 23; check(!Purple::SameRecordingThread(original, sameThread));
	sameThread = original; sameThread.account = 2; check(!Purple::SameRecordingThread(original, sameThread));
	sameThread = original; sameThread.user = 10; check(!Purple::SameRecordingThread(original, sameThread));
	sameThread = original; sameThread.peer = 202; check(!Purple::SameRecordingThread(original, sameThread));
	auto chosen = Purple::RecordingCaptureTarget{ 1, 9, 202, 33, 88 };
	const auto replacement = Purple::ReplacementRecordingTarget(original, chosen);
	check(replacement.has_value()); check(replacement->peer == 202 && replacement->topic == 33 && replacement->reply == 0);
	check(original.peer == 101 && original.reply == 31);
	chosen.account = 2; check(!Purple::ReplacementRecordingTarget(original, chosen));
	chosen.account = 1; chosen.user = 10; check(!Purple::ReplacementRecordingTarget(original, chosen));
	chosen.user = 9; chosen.peer = 0; check(!Purple::ReplacementRecordingTarget(original, chosen));
	check(Purple::RecordingComposeActive(false, true));
	check(!Purple::RecordingComposeActive(true, true));
	check(!Purple::RecordingComposeActive(false, false));
	check(!Purple::RecordingComposeActive(true, false));
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
	const auto targetRevision = model.targetRevision();
	check(!model.retarget(first)); check(model.targetRevision() == targetRevision);
	check(model.retarget(model.generation())); check(model.targetRevision() != targetRevision);
	check(!model.finish(first)); check(model.accepts(model.generation()));
	check(model.requestPause(model.generation())); check(model.transitioning());
	check(!model.requestPause(model.generation())); check(!model.finish(model.generation()));
	check(!model.retarget(model.generation()));
	check(!model.pause(first, true)); check(model.transitioning());
	check(model.pause(model.generation(), true)); check(!model.transitioning());
	check(model.requestPause(model.generation())); model.clear(); check(!model.transitioning());
	check(!model.requestPause(first));
	check(!model.receiveCompletion(first));
	check(model.begin()); const auto completed = model.generation();
	check(model.finish(completed)); check(model.receiveCompletion(completed));
	check(model.paused()); model.clear(); check(!model.receiveCompletion(completed));

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
