/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_settings_editor.h"

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
	MembershipCases();
	Refusals();
	SaveCases();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
