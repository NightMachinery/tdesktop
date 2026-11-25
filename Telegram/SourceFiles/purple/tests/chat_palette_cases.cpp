#include "purple/purple_chat_palette_search.h"

#include <cassert>

int main() {
	using Purple::ChatPaletteSearch::Parse;
	using Purple::ChatPaletteSearch::Rank;

	assert(Rank(Parse(QStringLiteral("ALI ر ض")), QStringLiteral("Ali ر ض"), {}, {}) == 1);
	assert(Rank(Parse(QStringLiteral("sghl")), QStringLiteral("سلام"), {}, {}) == 1);
	assert(Rank(Parse(QStringLiteral("ali سشمشپ")), QStringLiteral("Ali salam"), {}, {}) == 4);
	assert(Rank(Parse(QStringLiteral("سشمشپ")), QStringLiteral("salam"), {}, {}) == 1);
	assert(Rank(Parse(QStringLiteral("علي")), QStringLiteral("عَـل‌ی"), {}, {}) == 0);
	assert(Rank(Parse(QStringLiteral("۱۲۳")), {}, QStringLiteral("12345"), {}) == 6);
	assert(Rank(Parse(QStringLiteral("ali desk")), QStringLiteral("Ali"), {}, QStringLiteral("On the desk")) == 7);
	assert(!Rank(Parse(QStringLiteral("sghl")), {}, {}, QStringLiteral("سلام")));
	assert(!Rank(Parse(QStringLiteral("sغhl")), QStringLiteral("سلام"), {}, {}));
	assert(!Rank(Parse(QStringLiteral("s")), QStringLiteral("س"), {}, {}));
	assert(Rank(Parse(QStringLiteral("be")), QStringLiteral("Maybe"), {}, {}) == 2);
}
