#include "purple/purple_settings_draft.h"

#include <iostream>

int main() {
	using namespace Purple::SettingsDraft;
	int checks = 0;
	int failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::cerr << "FAIL " << name << '\n';
		}
	};
	auto history = History<int>();
	check(!history.current() && !history.canUndo(), "starts clean");
	history.push(7);
	history.push(3);
	history.push(std::nullopt);
	check(!history.current() && history.canUndo(), "matching opening bytes preserves the previous edit");
	history.undo();
	check(history.current() && *history.current() == 3, "undo restores earlier nonempty draft");
	history.undo();
	check(history.current() && *history.current() == 7, "older staged gesture still undoable");
	history.undo();
	check(!history.current() && !history.canUndo(), "undo reaches original bytes");
	check(ShouldRecapture(true, history.current()), "stale draft at zero requires new opening");
	check(!ShouldRecapture(false, history.current()), "clean opening needs no recapture after ordinary undo");
	history.push(1);
	check(!ShouldRecapture(true, history.current()), "stale staged edits must not be retargeted");
	history.clear();
	check(!history.canUndo() && !history.current(), "retiring discards staged memory");
	const auto opened = FormValue(QStringLiteral("reply  \n"));
	check(!NeedsDiscardConfirmation(opened, opened), "unchanged native text can close");
	check(NeedsDiscardConfirmation(opened, QStringLiteral("reply\n")), "typed whitespace change requires confirmation");
	check(NeedsDiscardConfirmation(FormValue(0), FormValue(2)), "a changed radio value requires confirmation");
	check(CanRestoreForm(true, true) && !CanRestoreForm(false, true) && !CanRestoreForm(true, false),
		"resume requires the same live session and the exact opening");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
