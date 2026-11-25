#include "purple/purple_settings_tree.h"

#include <QtGui/QInputMethodEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>

#include <iostream>

namespace {

using namespace Purple;
using namespace Purple::SettingsTree;

int Checks = 0;
int Failures = 0;

void Check(bool test, const char *label) {
	++Checks;
	if (!test) {
		++Failures;
		std::cerr << "FAIL " << label << '\n';
	}
}

} // namespace

int main() {
	const auto global = QByteArray("[peek]\nready_timeout_seconds = 600\n"
		"[last_seen]\nreasons_p = true\n[lists.\"a.b\"]\ntitle = 'Fixtures'\n"
		"[playback]\nseek_step = '10s'\n[unknown]\nreserved = 'SECRET_TOKEN'\n");
	const auto own = QByteArray("[peek]\nready_timeout_seconds = 900\n[media_reply]\ntext = 'SECRET_REPLY'\n");
	const auto layers = LayerSettings(QString::fromUtf8(global), QStringLiteral("global.toml"),
		QString::fromUtf8(own), QStringLiteral("own.toml"), { .traceReads = true });
	const auto rows = Build(layers, {
		{ QStringLiteral("Studio"), QStringLiteral("global.toml"), global, true },
		{ QStringLiteral("Personal"), QStringLiteral("own.toml"), own, true },
	});
	const auto peek = Address({ QStringLiteral("peek"), QStringLiteral("ready_timeout_seconds") });
	const auto last = Address({ QStringLiteral("last_seen"), QStringLiteral("reasons_p") });
	const auto found = [&](const QString &path) {
		return std::find_if(rows.begin(), rows.end(), [&](const Row &row) { return row.key == path; });
	};
	const auto timeout = found(peek);
	Check(timeout != rows.end(), "builds a stable scalar address");
	if (timeout != rows.end()) {
		Check(timeout->effective.toInt() == 600, "uses actual effective value");
		Check(timeout->source == QStringLiteral("Explicit · default value · Studio"), "explicit default is pinned");
		Check(timeout->physical.size() == 2 && timeout->physical[1].inactive, "inactive own source is shown");
		Check(timeout->destination == QStringLiteral("Studio"), "app-wide edits name the actual source");
	}
	const auto reasons = found(last);
	Check(reasons != rows.end() && reasons->source == QStringLiteral("Inherited · same value · Studio"),
		"inherited default has a distinct source label");
	const auto reply = QByteArray("[media_reply]\ntext = \"staged line one\\nstaged line two\"\n");
	const auto preview = LayerSettings(QString::fromUtf8(reply), QStringLiteral("fixture.toml"),
		QString::fromUtf8(reply), QStringLiteral("fixture.toml"), { .traceReads = true });
	const auto previewRows = Build(preview, {
		{ QStringLiteral("Fixture"), QStringLiteral("fixture.toml"), reply, true },
	});
	const auto stagedReply = std::find_if(previewRows.begin(), previewRows.end(), [](const Row &row) {
		return row.descriptor && row.descriptor->id == SettingId::ReplyText;
	});
	Check(stagedReply != previewRows.end()
		&& stagedReply->effective.toString() == QStringLiteral("staged line one\nstaged line two")
		&& stagedReply->defaultValue == QStringLiteral("@TranscribeThisAI"),
		"a staged multiline override keeps the built-in default in the inspector");
	const auto view = View(rows, {}, QStringLiteral("timeout peek"));
	Check(view.matches == 1 && std::any_of(view.rows.begin(), view.rows.end(), [&](size_t id) {
		return rows[id].key == peek;
	}), "orderless search reveals matching leaf and its ancestors");
	const auto folded = View(rows, {}, {});
	Check(View(rows, {}, {}).rows == folded.rows && std::none_of(folded.rows.begin(), folded.rows.end(),
		[&](size_t id) { return rows[id].key == peek; }), "clear filter restores original folds");
	Check(View(rows, {}, QStringLiteral("SECRET")).matches == 0, "never indexes sensitive or unknown values");
	Check(View(rows, {}, QStringLiteral("zzz none")).rows.empty(), "zero matches returns no rows");
	Check(View(rows, {}, QStringLiteral("zzz none"), peek).rows.size() > 1, "draft can keep the selected anchor until movement");
	Check(TomlPath({ QStringLiteral("lists"), QStringLiteral("a.b"), size_t(0), QStringLiteral("title") })
		== QStringLiteral("lists.\"a.b\".title"), "clipboard path quotes keys and leaves ordinal out");
	Check(TomlPath({ QStringLiteral("key\n") }) == QStringLiteral("\"key\\u000a\""),
		"clipboard path escapes a trailing newline in a quoted key");
	Check(Address({ QStringLiteral("a"), size_t(9), QStringLiteral("b") })
		!= Address({ QStringLiteral("a"), QStringLiteral("9"), QStringLiteral("b") }), "ordinals and literal keys differ");

	const auto &seconds = DescribeSetting(SettingId::ReadyTimeout);
	Check(Decode(seconds, QStringLiteral("5m")) == SettingScalar(int64(300)), "friendly duration uses real integer seconds");
	Check(!Decode(seconds, QStringLiteral("1.5s")), "fractional seconds refuse");
	Check(!Decode(seconds, QStringLiteral("5s")), "ready timeout obeys the real minimum");
	Check(!Step(seconds, QVariant(10), -1), "decrement at the minimum stages nothing");
	Check(!Step(seconds, QVariant(INT_MAX), 1), "increment at the maximum stages nothing");
	const auto &detent = DescribeSetting(SettingId::SeekStep);
	Check(Step(detent, QVariant(10), 1) == SettingScalar(QStringLiteral("15s")), "next allowed duration");
	Check(!Decode(detent, QStringLiteral("11s")), "unsupported duration refuses");
	const auto &retention = DescribeSetting(SettingId::BinKeep);
	Check(Step(retention, QVariant(0), 1) == SettingScalar(QStringLiteral("3600s"))
		&& Step(retention, QVariant(3600), -1) == SettingScalar(QStringLiteral("off")),
		"duration stepping skips unsupported values between off and the minimum");
	Check(Decode(DescribeSetting(SettingId::ReplyText), QStringLiteral(" a \n "))
		== SettingScalar(QStringLiteral(" a \n ")), "preserves native multiline text");
	Check(!Decode(DescribeSetting(SettingId::ReplyText), QStringLiteral(" \n ")), "blank reply refuses");
	Check(!CanReset(DescribeSetting(SettingId::RuleFrom)), "required field cannot reset");
	const auto invalid = QByteArray("[screen_time]\nretention_days = -10\n");
	const auto invalidLayer = LayerSettings(QString::fromUtf8(invalid), QStringLiteral("bad.toml"),
		{}, {}, { .accountPresent = false, .traceReads = true });
	const auto badRows = Build(invalidLayer, {
		{ QStringLiteral("Fixture"), QStringLiteral("bad.toml"), invalid, true },
	});
	const auto badDays = std::find_if(badRows.begin(), badRows.end(), [](const Row &row) {
		return row.descriptor && row.descriptor->id == SettingId::RetentionDays;
	});
	Check(badDays != badRows.end() && badDays->invalid && badDays->effective.toInt() == 90,
		"invalid raw value stays visible alongside the reader fallback");
	Check(ShouldHandleEditorKey(false, true, true, false) == EditorKey::Stage, "plain Return stages single line");
	const auto composition = QInputMethodEvent(QStringLiteral("仮"), {});
	const auto preedit = !composition.preeditString().isEmpty();
	Check(ShouldHandleEditorKey(preedit, true, true, false) == EditorKey::Native, "preedit owns Return");
	Check(ShouldHandleEditorKey(preedit, false, true, true) == EditorKey::Native, "preedit owns Escape");
	Check(ShouldHandleEditorKey(false, true, false, false) == EditorKey::Native, "multiline Return stays native");
	Check(ShouldHandleEditorKey(false, false, true, false) == EditorKey::Native, "native text undo stays native");
	auto document = QTextDocument();
	document.setPlainText(QStringLiteral("Existing reply"));
	auto caret = QTextCursor(&document);
	caret.movePosition(QTextCursor::End);
	caret.insertText(QStringLiteral("\nTemporary edit"));
	Check(document.isUndoAvailable(), "Qt records native multiline input for undo");
	document.undo();
	Check(document.toPlainText() == QStringLiteral("Existing reply"), "native document undo leaves staged settings alone");

	const auto color = ParseFixtureColor(QStringLiteral("#365A90"));
	Check(color == Rgb{ 54, 90, 144 }, "fixture hex to RGB");
	Check(color && EditFixtureColor(*color, 0, -100) == Rgb{ 0, 90, 144 }, "fixture channel clamps at zero");
	Check(!ParseFixtureColor(QStringLiteral("#11223344")), "fixture RGB rejects alpha");
	Check(!ParseFixtureColor(QStringLiteral("#365A90\n")), "fixture RGB requires exactly six digits");
	Check(color && FixtureColorHex(*color) == QStringLiteral("#365A90"), "fixture RGB formats hex");
	std::cout << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}
