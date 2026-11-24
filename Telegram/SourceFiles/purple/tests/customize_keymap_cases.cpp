#include "../../../ThirdParty/purple_edn/include/edn.h"

#include <QtCore/QSet>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>

#include <iostream>
#include <map>

namespace {

#include "customize-keymap.inc"

int Checks = 0;
int Failures = 0;

void Check(bool test, const char *label) {
	++Checks;
	if (!test) {
		++Failures;
		std::cerr << "FAIL " << label << '\n';
	}
}

std::optional<Action> ActionAt(const KeymapData &map, const QString &chord) {
	const auto keys = ParseChord(chord, MapKind::Normal);
	const auto binding = ranges::find_if(map.bindings, [&](const Binding &entry) {
		return entry.keys == keys && entry.map == MapKind::Normal;
	});
	return binding == map.bindings.end() ? std::nullopt : std::make_optional(binding->action);
}

} // namespace

int main() {
	auto defaults = KeymapData();
	auto error = QString();
	Check(ParseEdnOverlay(Bundled, defaults, false, error), "bundled map has no collisions");
	Check(ParseChord(u"SPC f e d"_q, MapKind::Normal).size() == 4, "accepts all four strokes");
	Check(ParseChord(u"SPC f e d x"_q, MapKind::Normal).empty(), "rejects five strokes");
	Check(ActionAt(defaults, u"SPC f e d"_q) == Action::CustomizeSettings, "Customize default");
	Check(ActionAt(defaults, u"SPC o p"_q) == Action::OpenCalls, "Calls keeps its default");
	Check(FindActionById(u"customize-settings"_q) == Action::CustomizeSettings, "palette catalog resolves command");
	auto user = defaults;
	Check(ParseEdnOverlay(
		R"({:normal {"SPC f e d" :open-settings "SPC f e u" :customize-settings}})",
		user, true, error), "user replaces a default and adds a new four-stroke binding");
	Check(ActionAt(user, u"SPC f e d"_q) == Action::OpenSettings
		&& ActionAt(user, u"SPC f e u"_q) == Action::CustomizeSettings
		&& ActionAt(user, u"SPC o p"_q) == Action::OpenCalls, "overlay wins and Calls stays reachable");
	auto collision = defaults;
	Check(!ParseEdnOverlay(R"({:normal {"SPC f e" :open-settings}})", collision, true, error)
		&& error.contains(u"prefix collision"_q), "shorter user binding cannot shadow Customize");
	auto removed = defaults;
	Check(ParseEdnOverlay(R"({:normal {"SPC f e d" nil}})", removed, true, error)
		&& !ActionAt(removed, u"SPC f e d"_q), "user can unbind Customize without losing the command");
	std::cout << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}
