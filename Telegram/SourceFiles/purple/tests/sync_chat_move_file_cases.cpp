namespace {

DesktopSyncChatSetupSnapshot MoveOpening(MoveInstall &install) {
	auto frame = DesktopSyncChatSetupSnapshot();
	const auto local = install.state();
	const auto cache = install.cache();
	frame.scope.origin = Origin;
	frame.scope.root = install.temporary.path();
	frame.scope.binding = install.scope.binding;
	frame.scope.loginBinding = install.scope.loginAuthority;
	frame.scope.settingsBound = true;
	frame.scope.settingsChat = local.chat;
	frame.scope.space = local.space;
	frame.scope.mark = cache.mark;
	frame.cache = cache;
	frame.input = install.cloud.resolution(local, cache);
	frame.status.complete = true;
	frame.status.phase = DesktopSyncChatPhase::Use;
	frame.status.resolution = ResolveSyncChat(frame.input);
	frame.status.postingHeld = frame.status.resolution.postingHeld;
	frame.stateImage = CaptureSyncPrivateFile(install.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes;
	frame.cacheImage = CaptureSyncPrivateFile(install.scope.cacheFile, 65536).bytes;
	frame.run = 1;
	return frame;
}

DesktopSyncChatMoveDraft MoveReview(MoveInstall &install) {
	const auto frame = MoveOpening(install);
	auto result = std::optional<DesktopSyncChatMoveDraft>();
	const auto peer = Route(install.target).peer;
	QMetaObject::invokeMethod(QCoreApplication::instance(), [&] {
		CHECK(!SyncStoreWorkerThread());
	auto action = DesktopSyncChatMoveAction(frame, [] { return true; });
	auto selection = DesktopSyncChatSetupSelection();
	selection.choice = DesktopSyncChatSetupChoice::Existing;
	selection.peer = peer;
	const auto offer = action.choose(selection);
	MoveRequire(offer.has_value(), "ordinary setup chooses next chat");
	auto starts = 0;
	MoveRequire(action.confirm(*offer, frame, selection.peer, false, true,
		[&](DesktopSyncChatMoveDraft) { ++starts; }), "ordinary setup accepts next chat");
	CHECK(starts == 0 && !DesktopSyncChatMoveActivated());
	CHECK(action.reason() == u"move-qualification-held"_q);
	MoveRequire(action.reviewed().has_value(), "accepted setup draft retained behind activation gate");
	result = *action.reviewed();
	}, Qt::BlockingQueuedConnection);
	MoveRequire(result.has_value(), "main-thread setup review accepted");
	return *result;
}

void MoveConsecutive() {
	MoveCase = u"one-install-consecutive-ordinary-setup"_q;
	std::printf("MOVE_CASE seed=%u case=%s\n", MoveSeed, qPrintable(MoveCase));
	auto cloud = MoveCloud();
	auto install = MoveInstall(cloud, 0);
	auto second = MoveInstall(cloud, 1);
	auto third = MoveInstall(cloud, 2);
	install.cleanup = false;
	install.start(MoveReview(install)); install.run(); install.done();
	const auto firstOperation = install.progress.journal->operationId;
	install.engine.reset();
	auto firstWrapper = QByteArray();
	{
		auto store = SyncChatCarryStore(install.root, install.scope);
		MoveRequire(bool(store.Open()), "normal carry-store reopen after first completion");
		CHECK(!store.journal() && install.active().isEmpty());
		const auto images = store.RetainedImages();
		MoveRequire(images.has_value(), "first operation retained images");
		for (const auto &image : *images) {
			const auto journal = InspectSyncCarryJournal(image, install.scope.binding);
			if (journal && journal->operationId == firstOperation) { firstWrapper = image; }
		}
	}
	MoveRequire(!firstWrapper.isEmpty(), "exact completed wrapper archived on disk");
	CHECK(!install.gate->postingHeld());
	{
		auto store = SyncLocalStore(install.root);
		MoveRequire(bool(store.Open(true, install.scope.binding)), "normal store reopen after completed change");
		CHECK(store.state()->chat == MoveFirst);
	}
	install.source = MoveFirst; install.target = MoveSecond;
	cloud.markDate = 1000;
	const auto nextDraft = MoveReview(install);
	CHECK(nextDraft.opening.scope.settingsChat == MoveFirst);
	install.start(nextDraft);
	MoveRequire(install.progress.command.has_value(), "second Begin emits a command");
	CHECK(install.progress.command->operation != firstOperation);
	CHECK(install.progress.command->source == MoveFirst && install.progress.command->target == MoveSecond);
	install.run(); install.done();
	CHECK(cloud.marks == 2 && cloud.deletes == 0);
	install.engine.reset();
	auto store = SyncChatCarryStore(install.root, install.scope);
	MoveRequire(bool(store.Open()), "reopen second completed journal");
	const auto retained = store.RetainedImages();
	MoveRequire(retained.has_value(), "previous operation archive readable");
	CHECK(std::count(retained->begin(), retained->end(), firstWrapper) == 1);
	CHECK(!store.journal() && store.state()->chat == MoveSecond);
	auto foundSecond = false;
	for (const auto &image : *retained) {
		const auto journal = InspectSyncCarryJournal(image, install.scope.binding);
		if (!journal || journal->operationId == firstOperation) { continue; }
		CHECK(journal->source == MoveFirst && journal->target == MoveSecond && journal->phase == SyncCarryPhase::Done);
		foundSecond = true;
	}
	CHECK(foundSecond);
	cloud.conserved(MoveSecond);
	++MoveRaces;
}

}
