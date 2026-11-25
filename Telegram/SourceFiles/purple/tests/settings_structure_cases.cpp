#include "purple/purple_settings_structures.h"

#include <iostream>

int main() {
	using namespace Purple;
	using namespace Purple::SettingsStructures;
	int checks = 0;
	int failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::cerr << "FAIL " << name << '\n';
		}
	};
	const auto source = QByteArray("[lists.alpha]\nunknown = 7\n[lists.beta]\n"
		"[presets.work]\nlist_order = [{list='alpha'}, 17, {list='beta', notify_p=false}]\n"
		"[schedule]\n[[schedule.rules]]\nfrom='22:00'\nto='06:00'\npreset='work'\n");
	const auto presets = SettingAddress{ u"presets"_q, u"work"_q, u"list_order"_q };
	const auto raw = Items(source, presets, false);
	check(raw.size() == 3 && raw[1].address.back() == SettingAddressPart(size_t(1)),
		"keeps parser-skipped physical ordinals");
	check(raw.size() == 3 && raw[1].literal == QByteArray("17") && !raw[1].recognized,
		"skipped item stays visible and marked");
	const auto move = ArrayEdit(presets, SettingsCollectionOp::Move, 2, 0);
	const auto mapped = MapSettingsSource(source);
	const auto moved = ApplySettingsEdits(source, mapped.revision, {move});
	check(moved.status == SettingsEditStatus::Changed
		&& moved.bytes.contains("{list='beta', notify_p=false}"), "move keeps raw record and changes order");
	const auto parsed = ParseSettings(QString::fromUtf8(moved.bytes), u"fixture"_q);
	const auto work = parsed.settings.preset(u"work"_q);
	check(work && !work->listOrder.empty() && work->listOrder.front().list == u"beta"_q, "first match order changes");
	const auto dup = ApplySettingsEdits(source, mapped.revision, {
		ArrayEdit(presets, SettingsCollectionOp::Duplicate, 0, 1) });
	check(dup.ok() && Items(dup.bytes, presets, false).size() == 4, "duplicates a raw item");
	const auto deleted = ApplySettingsEdits(source, mapped.revision, {
		ArrayEdit(presets, SettingsCollectionOp::Erase, 0) });
	check(deleted.ok() && Items(deleted.bytes, presets, false).size() == 2, "deletes at raw ordinal");
	const auto inserted = ApplySettingsEdits(source, mapped.revision, {
		ArrayEdit(presets, SettingsCollectionOp::Insert, 3, 0, QByteArray("{list='beta'}")) });
	check(inserted.ok() && Items(inserted.bytes, presets, false).size() == 4, "appends a typed TOML value");

	const auto alpha = SettingAddress{ u"lists"_q, u"alpha"_q };
	const auto repair = RewriteReferences(source, alpha, u"new"_q);
	check(repair.uses.size() == 1 && repair.edits.size() == 1, "reference repair is explicit and typed");
	auto edits = repair.edits;
	edits.push_back({ alpha, SettingsObjectEdit{ SettingsCollectionOp::Rename, u"new"_q } });
	const auto renamed = ApplySettingsEdits(source, mapped.revision, edits);
	check(renamed.ok() && renamed.bytes.contains("[lists.new]") && renamed.bytes.contains("unknown = 7")
		&& renamed.bytes.contains("'new'"), "named rename preserves unknowns and repairs references in one file");
	check(!Overnight(u"06:00"_q, u"22:00"_q) && Overnight(u"22:00"_q, u"06:00"_q), "cross-midnight preview");
	auto fieldError = QString();
	const auto rule = EncodeRecord(SettingId::FlatRules,
		{u"22:00"_q, u"06:00"_q, u"work"_q, u"mon,fri"_q, u"true"_q}, fieldError);
	check(rule && rule->contains("[\"mon\", \"fri\"]"), "typed rule groups hours and start-day chips");
	check(!EncodeRecord(SettingId::FlatRules,
		{u"22:00"_q, u"22:00"_q, u"work"_q, {}, u"true"_q}, fieldError), "identical rule times are invalid");
	check(EncodeRecord(SettingId::Views, {u"My tab"_q, u"beta"_q}, fieldError).has_value()
		&& !EncodeRecord(SettingId::Views, {u"My tab"_q, {}}, fieldError), "a view includes its first list in one record");
	check(EncodeRecord(SettingId::Budgets,
		{u"chat:-4"_q, u"45m"_q, u"hard"_q, u"5m"_q, u"2"_q}, fieldError).has_value(), "typed budget accepts offline chat IDs");
	check(CanReplay(source, source + "# different unrelated text\n", { move }), "unrelated edit keeps match identity");
	check(!CanReplay(source, moved.bytes, { move }), "array reorder makes old ordinals stale");
	check(!CanReplay(source, QByteArray("[broken"), {move}), "malformed current file cannot replay");
	check(ParsePeerLiteral(PeerField::Members, u"42"_q).has_value()
		&& !ParsePeerLiteral(PeerField::Members, u"channel:42"_q), "members use bare IDs");
	check(ParsePeerLiteral(PeerField::Media, u"channel:42"_q).has_value()
		&& !ParsePeerLiteral(PeerField::Media, u"42"_q), "media uses typed IDs");
	check(ParsePeerLiteral(PeerField::Notifications, u"MAGIC_BOTS"_q).has_value(),
		"notification magic group is available offline");
	check(ParsePeerLiteral(PeerField::Notifications, u"group:42"_q).has_value()
		&& !ParsePeerLiteral(PeerField::Notifications, u"42"_q), "notification IDs keep their own kind prefixes");
	check(ParsePeerLiteral(PeerField::Members, u"-42"_q).has_value()
		&& !ParsePeerLiteral(PeerField::Media, u"self"_q), "stored signed list IDs and explicit typed media IDs");
	check(ParsePeerLiteral(PeerField::Budget, u"chat:-42"_q).has_value(), "budget permits a signed chat target");
	const auto hidden = PeerLabel(u"42"_q, u"Hidden name"_q, false);
	check(hidden.contains(u"42"_q) && !hidden.contains(u"Hidden name"_q), "unavailable and hidden peers keep neutral labels");
	const auto hugeOld = QByteArray("a\n").repeated(1400);
	const auto hugeNew = QByteArray("b\n").repeated(1400);
	const auto pages = ReviewLines(DiffConfigText(hugeOld, hugeNew), hugeOld, hugeNew);
	check(pages.size() > 2 && pages.front().contains(u"Original file"_q)
		&& pages.back().contains(u"Candidate file"_q), "truncated diff supplies both complete files in pages");
	check(SourceExcerpt(QByteArray("a\nb\nc"), 2, 1).contains(u"2  b"_q), "recovery excerpt shows its physical line");
	check(SourceExcerpt(QByteArray(1, char(0xff)), 1, 1).isEmpty(), "invalid UTF-8 never has a fabricated excerpt");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
