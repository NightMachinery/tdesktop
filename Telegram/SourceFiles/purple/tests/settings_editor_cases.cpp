/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_settings_editor.h"
#include "purple/purple_settings_structures.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>

#include <cstdio>

namespace {

using namespace Purple;

const auto A = AccountKey{ 0, 123 };
const auto B = AccountKey{ 1, 123 };
const auto C = AccountKey{ 0, 456 };
const auto Current = [] { return true; };

auto Checks = 0;
auto Failures = 0;

void Check(bool ok, const char *expression, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL settings editor:%d %s\n", line, expression);
	}
}

#define CHECK(value) Check(bool(value), #value, __LINE__)

void Put(const QString &path, const QByteArray &bytes) {
	CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
	auto file = QSaveFile(path);
	CHECK(file.open(QIODevice::WriteOnly));
	CHECK(file.write(bytes) == bytes.size());
	CHECK(file.commit());
}

QByteArray Get(const QString &path) {
	return ReadSettingsFileInput(path).bytes;
}

SettingsEdit Scalar(
		std::initializer_list<QString> keys,
		std::optional<SettingScalar> value) {
	auto result = SettingsEdit();
	for (const auto &key : keys) {
		result.address.push_back(key);
	}
	result.operation = std::move(value);
	return result;
}

struct Fixture {
	QTemporaryDir temp;
	DesktopAccountConfig routing{ temp.path() };
	QString root = temp.filePath(u"settings.toml"_q);
	QString other;
	QString third;

	explicit Fixture(bool multiple = true) {
		Put(root, "version=2\n[premium]\nenabled_p=true # stay\n"
			"[lists.work]\nmembers=[123, 456]\n[presets.work]\nlist_order=[{list='work'}]\n"
			"[peek]\nstart_delay_seconds=10\nready_timeout_seconds=600\n");
		routing.start(multiple
			? std::vector<StartupAccountCandidate>{ { A, false, false }, { B, false, false }, { C, false, false } }
			: std::vector<StartupAccountCandidate>{ { A, false, false } }, A);
		other = routing.files(B)->settings;
		third = routing.files(C)->settings;
	}

	SettingsEditorPreparation prepare(AccountKey account, SettingsEditorIntent intent) {
		return PrepareSettingsEditorEdit(
			routing,
			CaptureSettingsEditorOpening(routing, account),
			std::move(intent));
	}

	SettingsEditorPreparation scalar(
			AccountKey account,
			std::initializer_list<QString> keys,
			std::optional<SettingScalar> value) {
		return prepare(account, std::vector{ Scalar(keys, std::move(value)) });
	}
};

void ArchivePolicyCases() {
	auto f = Fixture();
	Put(f.root, Get(f.root) + "\n[archive]\ndays=15\ntotal_mib=16\ntimed_media_mib=30\n");
	f.routing.refreshSettings();
	CHECK(f.routing.settings(A).archive.days == 15);
	CHECK(f.routing.settings(B).archive.timedMiB == 30);
	for (const auto value : { u"off"_q, u"on_open"_q }) {
		const auto allowed = f.scalar(A, { u"archive"_q, u"timed_download"_q }, value);
		CHECK(allowed.status == SettingsEditorStatus::Ready && allowed.review);
	}
	const auto gate = f.scalar(A, { u"archive"_q, u"timed_download"_q }, u"on_arrival"_q);
	CHECK(gate.status == SettingsEditorStatus::InvalidValue && gate.error.contains(u"two-account"_q));
	const auto oversized = f.scalar(A, { u"archive"_q, u"ordinary_file_mib"_q }, int64(17));
	CHECK(oversized.status == SettingsEditorStatus::InvalidValue && oversized.error.contains(u"1..16"_q));
	Put(f.root, Get(f.root) + "ordinary_file_mib=17\n");
	f.routing.refreshSettings();
	CHECK(f.routing.settings(A).archive.days == 15 && f.routing.settings(A).archive.ordinaryFileMiB == 16);
	CHECK(f.routing.settings(B).archive.timedMiB == 30);
	const auto repaired = f.scalar(A, { u"archive"_q, u"ordinary_file_mib"_q }, int64(2));
	CHECK(repaired.status == SettingsEditorStatus::Ready && repaired.review);

	const auto retention = f.prepare(B, std::vector{
		Scalar({ u"archive"_q, u"days"_q }, int64(7)),
		Scalar({ u"archive"_q, u"total_mib"_q }, int64(64)),
		Scalar({ u"archive"_q, u"timed_media_mib"_q }, int64(0)),
		Scalar({ u"archive"_q, u"ordinary_file_mib"_q }, int64(4)),
		Scalar({ u"archive"_q, u"days_by_class"_q, u"bot"_q }, int64(2)),
	});
	CHECK(retention.status == SettingsEditorStatus::Ready && retention.review);
	if (!retention.review) return;
	CHECK(retention.review->preview().fileAccount == A);
	CHECK(ApplySettingsEditorEdit(f.routing, *retention.review, Current).status == SettingsEditorStatus::Saved);
	CHECK(Get(f.root) == retention.review->preview().after);
	const auto disk = ParseSettings(QString::fromUtf8(Get(f.root)), f.root);
	CHECK(disk.ok() && disk.settings.archive.valid);
	CHECK(disk.settings.archive.days == 7 && disk.settings.archive.totalMiB == 64);
	CHECK(disk.settings.archive.timedMiB == 0 && disk.settings.archive.ordinaryFileMiB == 4);
	CHECK(disk.settings.archive.daysFor(ArchiveV2::ChatClass::Bot) == 2);
	f.routing.refreshSettings();
	CHECK(f.routing.settings(B).archive == disk.settings.archive);
}

void ScopeCases() {
	auto f = Fixture();
	const auto global = f.scalar(B, { u"premium"_q, u"enabled_p"_q }, false);
	CHECK(global.status == SettingsEditorStatus::Ready && global.review);
	if (!global.review) { return; }
	CHECK(global.review->preview().fileAccount == A);
	CHECK(global.review->preview().requester == B);
	CHECK(global.review->preview().before.contains("true # stay"));
	CHECK(global.review->preview().after.contains("false # stay"));
	CHECK(!QFileInfo::exists(f.other));
	const auto saved = ApplySettingsEditorEdit(f.routing, *global.review, Current);
	CHECK(saved.status == SettingsEditorStatus::Saved && saved.operation);
	CHECK(Get(f.root) == global.review->preview().after);
	CHECK(!QFileInfo::exists(f.other));
	CHECK(ApplySettingsEditorEdit(f.routing, *global.review, Current).status
		== SettingsEditorStatus::StaleRevision);

	const auto reset = f.scalar(B, { u"last_seen"_q, u"reasons_p"_q }, std::nullopt);
	CHECK(reset.status == SettingsEditorStatus::Unchanged);
	CHECK(!QFileInfo::exists(f.other));

	const auto own = f.scalar(B, { u"last_seen"_q, u"reasons_p"_q }, false);
	CHECK(own.review && own.review->preview().route == SettingsEditorRoute::AccountRow);
	if (!own.review) { return; }
	const auto first = ApplySettingsEditorEdit(f.routing, *own.review, Current);
	CHECK(first.status == SettingsEditorStatus::Saved && first.created);
	CHECK(Get(f.other) == own.review->preview().after);
	CHECK(!QFileInfo::exists(f.third));
	CHECK(!f.routing.observeAccountSave(B, true));
	const auto scoped = f.scalar(B, { u"last_seen"_q, u"trade_hold"_q }, QString(u"15s"_q));
	CHECK(scoped.review && scoped.review->preview().route == SettingsEditorRoute::Own);

	const auto fileLocal = f.scalar(B, { u"sync"_q, u"send_after_save_p"_q }, true);
	CHECK(fileLocal.review && fileLocal.review->preview().fileAccount == B);
	const auto mixed = f.prepare(B, std::vector{
		Scalar({ u"premium"_q, u"enabled_p"_q }, true),
		Scalar({ u"last_seen"_q, u"reasons_p"_q }, true),
	});
	CHECK(mixed.status == SettingsEditorStatus::Unsupported);
	const auto media = f.scalar(B, { u"media_reply"_q, u"text"_q }, QString(u"Reply"_q));
	CHECK(media.review && media.review->preview().route == SettingsEditorRoute::MediaReply);
	if (media.review) {
		CHECK(ApplySettingsEditorEdit(f.routing, *media.review, Current).status == SettingsEditorStatus::Saved);
		CHECK(Get(f.other).contains("Reply") && !Get(f.root).contains("Reply"));
	}
	const auto round = f.scalar(B, { u"media_reply"_q, u"private"_q, u"round"_q }, QString(u"own"_q));
	CHECK(round.review && round.review->preview().after.contains("round"));
	if (round.review) {
		CHECK(ApplySettingsEditorEdit(f.routing, *round.review, Current).status == SettingsEditorStatus::Saved);
	}
}

void MembershipCases() {
	for (const auto mode : { AccountWorkMode::Own, AccountWorkMode::Stock, AccountWorkMode::Follow }) {
		auto f = Fixture();
		const auto target = f.routing.workModeTarget(B);
		CHECK(target.has_value());
		if (!target) { continue; }
		CHECK(f.routing.writeWorkMode(*target, mode, Current).ok);
		const auto review = f.prepare(B, SettingsEditorMembers{ u"work"_q, std::vector<PeerIdValue>{ 456, 123 } });
		CHECK(review.review && review.review->preview().route == SettingsEditorRoute::Members);
		if (!review.review) { continue; }
		CHECK(review.review->preview().fileAccount == (mode == AccountWorkMode::Follow ? A : B));
		CHECK(ApplySettingsEditorEdit(f.routing, *review.review, Current).status == SettingsEditorStatus::Saved);
		CHECK(Get(mode == AccountWorkMode::Follow ? f.root : f.other) == review.review->preview().after);
		CHECK(!QFileInfo::exists(f.third));
	}
	auto f = Fixture();
	const auto review = f.prepare(A, SettingsEditorMembers{ u"work"_q, std::vector<PeerIdValue>{ 456, 123 } });
	const auto target = f.routing.workModeTarget(A);
	CHECK(review.review && target);
	if (!review.review || !target) { return; }
	const auto before = Get(f.root);
	CHECK(f.routing.writeWorkMode(*target, AccountWorkMode::Stock, Current).ok);
	CHECK(ApplySettingsEditorEdit(f.routing, *review.review, Current).status
		== SettingsEditorStatus::StaleRevision);
	CHECK(Get(f.root) == before);
}

void Refusals() {
	{
		auto f = Fixture(false);
		const auto before = Get(f.root);
		const auto setting = f.scalar(A, { u"peek"_q, u"ready_timeout_seconds"_q }, int64(300));
		CHECK(setting.review && setting.review->preview().route == SettingsEditorRoute::Peek);
		if (setting.review) {
			CHECK(ApplySettingsEditorEdit(f.routing, *setting.review, Current).status
				== SettingsEditorStatus::Unsupported);
		}
		CHECK(Get(f.root) == before);
		const auto invalid = f.scalar(A, { u"peek"_q, u"ready_timeout_seconds"_q }, int64(3));
		CHECK(invalid.status == SettingsEditorStatus::InvalidValue);
		Put(f.root, "version=2\n[lists.work]\nmembers=[123, 456, # second\n 789]");
		const auto unusual = f.prepare(A, SettingsEditorMembers{
			u"work"_q, std::vector<PeerIdValue>{ 456, 123, 789 },
		});
		CHECK(unusual.status == SettingsEditorStatus::Unsupported);
	}
	{
		auto f = Fixture();
		Put(f.root, "[premium]\n enabled_p = [");
		const auto invalid = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(invalid.status == SettingsEditorStatus::InvalidSource && invalid.line > 0 && invalid.column > 0);
		Put(f.root + u".good"_q, "version=2\n[last_seen]\nreasons_p=false\n");
		const auto other = f.scalar(B, { u"last_seen"_q, u"reasons_p"_q }, true);
		CHECK(other.review && other.review->preview().usesGlobalRecovery);
		CHECK(f.scalar(B, { u"premium"_q, u"enabled_p"_q }, true).status
			== SettingsEditorStatus::InvalidSource);
		CHECK(QFile::remove(f.root));
		CHECK(!f.scalar(A, { u"premium"_q, u"enabled_p"_q }, true).review);
		const auto missingWithRecovery = f.scalar(B, { u"last_seen"_q, u"reasons_p"_q }, true);
		CHECK(missingWithRecovery.review && missingWithRecovery.review->preview().usesGlobalRecovery);
		if (missingWithRecovery.review) {
			const auto &global = missingWithRecovery.review->preview().beforeLayers.global;
			CHECK(!global.current.present && global.good && global.good->path == f.root + u".good"_q);
		}
		Put(f.other + u".good"_q, "version=2\n");
		CHECK(!f.scalar(B, { u"last_seen"_q, u"reasons_p"_q }, true).review);
	}
	{
		auto f = Fixture(false);
		Put(f.root, "version=2\n# \xc0\n[premium]\nenabled_p=true\n");
		const auto invalid = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(invalid.status == SettingsEditorStatus::InvalidSource && invalid.line == 0);
	}
	{
		auto f = Fixture();
		f.routing.start({ { B, false, false }, { C, false, false } });
		auto opening = CaptureSettingsEditorOpening(f.routing, B);
		opening.globalSignedIn = false;
		const auto signedOut = PrepareSettingsEditorEdit(f.routing, opening,
			std::vector{ Scalar({ u"premium"_q, u"enabled_p"_q }, false) });
		CHECK(signedOut.review && signedOut.review->preview().fileAccount == A
			&& signedOut.review->preview().offlineGlobalSource);
		if (signedOut.review) {
			const auto saved = ApplySettingsEditorEdit(f.routing, *signedOut.review, Current);
			CHECK(saved.status == SettingsEditorStatus::Saved && saved.offlineGlobalSource);
		}
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"local.toml"_q), "broken [");
		auto routing = DesktopAccountConfig(temp.path());
		routing.start({ { A, false, false }, { B, false, false } });
		const auto held = PrepareSettingsEditorEdit(routing, CaptureSettingsEditorOpening(routing, A),
			std::vector{ Scalar({ u"premium"_q, u"enabled_p"_q }, false) });
		CHECK(held.status == SettingsEditorStatus::Unavailable);
	}
}

void CollectionAndConflictCases() {
	auto f = Fixture();
	const auto prepared = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
	CHECK(prepared.review);
	if (!prepared.review) return;
	const auto original = Get(f.root);
	Put(f.root, original + "\n# unrelated external edit\n");
	const auto opening = CaptureSettingsEditorOpening(f.routing, A);
	const auto rebased = ReprepareSettingsEditorEdit(f.routing, opening, *prepared.review);
	CHECK(rebased.review && rebased.review->preview().after.contains("# unrelated external edit"));
	if (rebased.review) {
		const auto before = ReadSettingsFileInput(f.root);
		CHECK(ApplySettingsEditorEdit(f.routing, *rebased.review, Current).status == SettingsEditorStatus::Saved);
		CHECK(Get(f.root) == rebased.review->preview().after);
		CHECK(f.routing.takeEditorGlobalReload(f.root, before, ReadSettingsFileInput(f.root)));
		CHECK(!f.routing.takeEditorGlobalReload(f.root, before, ReadSettingsFileInput(f.root)));
	}
	Put(f.root, QByteArray(original).replace("true # stay", "false # external"));
	CHECK(ReprepareSettingsEditorEdit(f.routing, CaptureSettingsEditorOpening(f.routing, A), *prepared.review).status
		== SettingsEditorStatus::StaleRevision);

	Put(f.other, "version=2\n[media_reply.sets.fixture]\npeers=['user:123']\nvoice='both'\n");
	const auto peerPath = SettingAddress{u"media_reply"_q, u"sets"_q, u"fixture"_q, u"peers"_q};
	const auto insert = SettingsEdit{ peerPath, SettingsArrayEdit{ .operation = SettingsCollectionOp::Insert,
		.index = 1, .value = QString(u"channel:456"_q) } };
	const auto move = SettingsEdit{ peerPath, SettingsArrayEdit{ .operation = SettingsCollectionOp::Move,
		.index = 1, .destination = 0 } };
	const auto media = f.prepare(B, std::vector{ insert, move });
	CHECK(media.review && media.review->preview().route == SettingsEditorRoute::MediaReply);
	if (media.review) {
		CHECK(media.review->preview().after.indexOf("channel:456") < media.review->preview().after.indexOf("user:123"));
		CHECK(ApplySettingsEditorEdit(f.routing, *media.review, Current).status == SettingsEditorStatus::Saved);
		CHECK(Get(f.other) == media.review->preview().after);
		CHECK(!f.routing.observeAccountSave(B, true));
	}
	CHECK(f.prepare(B, std::vector{ move, Scalar({ u"media_reply"_q, u"text"_q }, QString(u"Reply"_q)) }).status
		== SettingsEditorStatus::Unsupported);

	Put(f.root, original);
	const auto members = f.prepare(A, SettingsEditorMembers{ u"work"_q, std::vector<PeerIdValue>{ 456, 123 } });
	CHECK(members.review);
	if (!members.review) return;
	Put(f.root, original + "\n# independent membership-era edit\n");
	const auto memberReplay = ReprepareSettingsEditorEdit(f.routing, CaptureSettingsEditorOpening(f.routing, A), *members.review);
	CHECK(memberReplay.review && memberReplay.review->preview().after.contains("# independent membership-era edit"));
	Put(f.root, QByteArray(original).replace("members=[123, 456]", "members=[123, 78]"));
	CHECK(ReprepareSettingsEditorEdit(f.routing, CaptureSettingsEditorOpening(f.routing, A), *members.review).status
		== SettingsEditorStatus::StaleRevision);
}

void CounterpartReferences() {
	auto f = Fixture();
	const auto list = SettingAddress{ u"lists"_q, u"work"_q };
	const auto members = SettingAddress{ u"lists"_q, u"work"_q, u"members"_q };
	const auto ownBytes = QByteArray("version=2\n[lists.work]\nmembers=[77]\n"
		"[list_sets.review]\nlist_order=[{list='work'}]\n"
		"[presets.fixture]\nlist_order=['*review']\nfolders=['*saved']\n"
		"[focus_sync]\nenter_preset='work'\n");
	Put(f.other, ownBytes);
	const auto ownMode = f.routing.workModeTarget(B);
	CHECK(ownMode && f.routing.writeWorkMode(*ownMode, AccountWorkMode::Own, Current).ok);
	const auto opening = CaptureSettingsEditorOpening(f.routing, B);
	const auto ownRefs = SettingsEditorOtherReferences(opening, f.root, list);
	CHECK(ownRefs.size() == 1);
	if (ownRefs.size() != 1) return;
	CHECK(ownRefs.front().path == f.other && ownRefs.front().role == SettingsOpenRole::Own);
	CHECK(ownRefs.front().references.size() == 1 && ownRefs.front().memberTable == members);
	CHECK(!ownRefs.front().ignoredInFollow);
	for (const auto &object : { SettingAddress{ u"list_sets"_q, u"review"_q },
			SettingAddress{ u"folder_sets"_q, u"saved"_q }, SettingAddress{ u"presets"_q, u"work"_q } }) {
		const auto result = SettingsEditorOtherReferences(opening, f.root, object);
		CHECK(result.size() == 1 && result.front().references.size() == 1 && !result.front().memberTable);
	}
	const auto globalRefs = SettingsEditorOtherReferences(opening, f.other, list);
	CHECK(globalRefs.size() == 1 && globalRefs.front().path == f.root
		&& globalRefs.front().references.size() == 1 && globalRefs.front().memberTable == members);
	CHECK(SettingsEditorOtherReferences(CaptureSettingsEditorOpening(f.routing, A), f.root, list).empty());
	auto follow = opening;
	follow.workMode = AccountWorkMode::Follow;
	CHECK(SettingsEditorOtherReferences(follow, f.root, list).front().ignoredInFollow);
	auto unavailable = opening;
	unavailable.ownSource.reset();
	CHECK(SettingsEditorOtherReferences(unavailable, f.root, list).front().status == SettingsReferenceStatus::Unavailable);
	for (const auto &[input, status] : {
			std::pair{ SettingsFileState::Missing, SettingsReferenceStatus::Missing },
			std::pair{ SettingsFileState::ReadError, SettingsReferenceStatus::Unreadable } }) {
		auto captured = opening;
		captured.ownSource->file.input = { input };
		captured.ownGood = {};
		CHECK(SettingsEditorOtherReferences(captured, f.root, list).front().status == status);
	}
	auto invalid = opening;
	invalid.ownSource->file.input = { SettingsFileState::Readable, "broken=[" };
	invalid.ownGood = {};
	CHECK(SettingsEditorOtherReferences(invalid, f.root, list).front().status == SettingsReferenceStatus::Invalid);
	invalid.ownGood = { SettingsFileState::Readable, ownBytes };
	const auto recovered = SettingsEditorOtherReferences(invalid, f.root, list).front();
	CHECK(recovered.usesRecovery && recovered.references.size() == 1 && recovered.memberTable == members);
	CHECK(Get(f.other) == ownBytes && !QFileInfo::exists(f.third));

	auto rename = SettingsStructures::RewriteReferences(Get(f.root), list, u"named"_q).edits;
	rename.push_back({ list, SettingsObjectEdit{ SettingsCollectionOp::Rename, u"named"_q } });
	const auto reviewed = PrepareSettingsEditorEdit(f.routing, opening, rename);
	CHECK(reviewed.review);
	if (!reviewed.review) return;
	Put(f.other, ownBytes + "\n[list_sets.extra]\nlist_order=[{list='work'}]\n");
	const auto fresh = CaptureSettingsEditorOpening(f.routing, B);
	CHECK(ReprepareSettingsEditorEdit(f.routing, fresh, *reviewed.review).status == SettingsEditorStatus::StaleRevision);
}

void SaveCases() {
	{
		auto f = Fixture();
		const auto before = QByteArray("\xef\xbb\xbf") + Get(f.root);
		Put(f.root, before);
		const auto opening = CaptureSettingsEditorOpening(f.routing, B);
		const auto grouped = PrepareSettingsEditorEdit(f.routing, opening, std::vector{
			Scalar({ u"premium"_q, u"enabled_p"_q }, false),
			Scalar({ u"suggestions"_q, u"hide_invisible_p"_q }, false),
		});
		const auto concurrent = PrepareSettingsEditorEdit(f.routing, opening, std::vector{
			Scalar({ u"recording"_q, u"keep_discarded_p"_q }, false),
		});
		CHECK(grouped.review && concurrent.review);
		if (!grouped.review || !concurrent.review) { return; }
		CHECK(ApplySettingsEditorEdit(f.routing, *grouped.review, Current).status == SettingsEditorStatus::Saved);
		CHECK(ApplySettingsEditorEdit(f.routing, *concurrent.review, Current).status == SettingsEditorStatus::StaleRevision);
		CHECK(Get(f.root) == grouped.review->preview().after);
		CHECK(Get(f.root).startsWith("\xef\xbb\xbf"));
		CHECK(Get(f.root + u".bak"_q) == before);
	}
	{
		auto f = Fixture();
		CHECK(f.routing.observeListReloads({ A, B }).empty());
		const auto x = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(x.review);
		if (!x.review) { return; }
		const auto before = Get(f.root);
		const auto result = ApplySettingsEditorEdit(f.routing, *x.review, Current);
		CHECK(result.status == SettingsEditorStatus::Saved);
		const auto after = Get(f.root);
		CHECK(f.routing.takeEditorGlobalReload(f.root, { SettingsFileState::Readable, before },
			{ SettingsFileState::Readable, after }));
		CHECK(f.routing.observeListReloads({ A, B }).empty());
		Put(f.root, after + "[suggestions]\nhide_invisible_p=false\n");
		CHECK(!f.routing.observeListReloads({ A, B }).empty());
	}
	{
		auto f = Fixture();
		CHECK(f.routing.observeListReloads({ A, B }).empty());
		const auto x = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(x.review);
		if (!x.review) { return; }
		CHECK(ApplySettingsEditorEdit(f.routing, *x.review, Current).status == SettingsEditorStatus::Saved);
		Put(f.root, Get(f.root));
		CHECK(!f.routing.observeListReloads({ A, B }).empty());
	}
	{
		auto f = Fixture();
		CHECK(f.routing.observeListReloads({ A, B }).empty());
		const auto x = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(x.review);
		if (!x.review) { return; }
		CHECK(ApplySettingsEditorEdit(f.routing, *x.review, Current).status == SettingsEditorStatus::Saved);
		Put(f.root, Get(f.root) + "[suggestions]\nhide_invisible_p=false\n");
		CHECK(!f.routing.observeListReloads({ A, B }).empty());
	}
	{
		auto f = Fixture();
		const auto before = Get(f.root);
		const auto captured = CaptureSettingsEditorOpening(f.routing, A);
		Put(f.root, before + "# unrelated\n");
		const auto stale = PrepareSettingsEditorEdit(f.routing, captured,
			std::vector{ Scalar({ u"premium"_q, u"enabled_p"_q }, false) });
		CHECK(stale.status == SettingsEditorStatus::StaleRevision);
		const auto fresh = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(fresh.review);
		if (fresh.review) {
			CHECK(ApplySettingsEditorEdit(f.routing, *fresh.review, [] { return false; }).status
				== SettingsEditorStatus::StaleRevision);
		}
		CHECK(Get(f.root) == before + "# unrelated\n");
	}
	{
		auto f = Fixture();
		const auto before = Get(f.root);
		CHECK(QDir().mkdir(f.root + u".bak"_q));
		const auto review = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(review.review);
		if (review.review) {
			CHECK(ApplySettingsEditorEdit(f.routing, *review.review, Current).status
				== SettingsEditorStatus::WriteFailed);
		}
		CHECK(Get(f.root) == before);
	}
	{
		auto f = Fixture();
		CHECK(f.routing.observeListReloads({ A, B }).empty());
		const auto review = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(review.review);
		if (!review.review) { return; }
		const auto before = Get(f.root);
		const auto saved = ApplySettingsEditorEdit(f.routing, *review.review, [&] {
			return Get(f.root) == before;
		});
		CHECK(saved.status == SettingsEditorStatus::SavedWithFollowUpError && saved.operation);
		CHECK(Get(f.root) == review.review->preview().after);
		CHECK(f.routing.observeListReloads({ A, B }).empty());
	}
	{
		auto f = Fixture();
		const auto review = f.scalar(A, { u"premium"_q, u"enabled_p"_q }, false);
		CHECK(review.review);
		if (!review.review) { return; }
		auto afterCommit = 0;
		const auto saved = ApplySettingsEditorEdit(f.routing, *review.review, [&] {
			if (Get(f.root) == review.review->preview().after && ++afterCommit == 2) {
				CHECK(QDir().mkdir(f.root + u".good"_q));
			}
			return true;
		});
		CHECK(saved.status == SettingsEditorStatus::SavedWithFollowUpError);
		CHECK(saved.error.contains(u"last-good"_q));
		CHECK(Get(f.root) == review.review->preview().after);
	}
	for (auto lost = 0; lost != 3; ++lost) {
		auto f = Fixture();
		auto opening = CaptureSettingsEditorOpening(f.routing, A);
		auto unlocked = true;
		auto signedIn = true;
		auto sameWindow = true;
		opening.current = [&] { return unlocked && signedIn && sameWindow; };
		const auto review = PrepareSettingsEditorEdit(f.routing, opening, std::vector{
			Scalar({ u"premium"_q, u"enabled_p"_q }, false),
		});
		CHECK(review.review);
		if (!review.review) { continue; }
		unlocked = lost != 0;
		signedIn = lost != 1;
		sameWindow = lost != 2;
		CHECK(ApplySettingsEditorEdit(f.routing, *review.review, Current).status == SettingsEditorStatus::StaleRevision);
		CHECK(Get(f.root) == review.review->preview().before);
	}
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	ScopeCases();
	ArchivePolicyCases();
	MembershipCases();
	Refusals();
	CollectionAndConflictCases();
	CounterpartReferences();
	SaveCases();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
