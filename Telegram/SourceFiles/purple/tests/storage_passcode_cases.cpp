#include "purple/purple_storage_passcode.h"

#include <QtCore/QString>

#include <iostream>
#include <vector>

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	const auto utf8 = [](const char16_t *text) {
		return QString::fromUtf16(text).toUtf8();
	};
	using namespace Purple;
	const auto persian = utf8(u"ضصثق"); // qwer typed on the Persian layout.
	const auto english = QByteArray("qwer");
	auto asked = std::vector<QByteArray>();
	const auto opener = [&](QByteArray accepted) {
		return [&asked, accepted](const QByteArray &candidate) {
			asked.push_back(candidate);
			return candidate == accepted;
		};
	};

	auto passcode = english;
	PreferPersianKeyboardPasscode(passcode, opener(english));
	expect(passcode == english, "ascii passcode kept");
	expect(asked.empty(), "ascii passcode derives nothing extra");

	passcode = QByteArray();
	PreferPersianKeyboardPasscode(passcode, opener(english));
	expect(passcode.isEmpty() && asked.empty(), "empty passcode untouched");

	passcode = persian;
	PreferPersianKeyboardPasscode(passcode, opener(english));
	expect(passcode == english, "persian layout opens with its counterpart");
	expect(asked.size() == 2 && asked[0] == persian && asked[1] == english,
		"exact bytes tried before the counterpart");

	asked.clear();
	passcode = persian;
	PreferPersianKeyboardPasscode(passcode, opener(persian));
	expect(passcode == persian, "a persian passcode that opens is kept");
	expect(asked.size() == 1 && asked[0] == persian, "counterpart not tried after a match");

	asked.clear();
	passcode = persian;
	PreferPersianKeyboardPasscode(passcode, opener(QByteArray("other")));
	expect(passcode == persian, "wrong passcode is not rewritten");
	expect(asked.size() == 2, "wrong passcode tries both candidates");

	asked.clear();
	passcode = utf8(u"ضص") + "12";
	PreferPersianKeyboardPasscode(passcode, opener(QByteArray("qw12")));
	expect(passcode == "qw12", "mixed text maps only layout characters");

	auto attempted = QByteArray();
	const auto attempt = [&](const QByteArray &mapped) {
		attempted = mapped;
		return true;
	};
	expect(!TryPersianKeyboardPasscode(english, attempt) && attempted.isEmpty(),
		"legacy retry skips an ascii passcode");
	expect(TryPersianKeyboardPasscode(persian, attempt) && attempted == english,
		"legacy retry offers the counterpart");

	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
