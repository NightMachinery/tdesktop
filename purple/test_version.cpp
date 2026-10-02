/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_version.h"

#include <cstdio>

namespace {

auto checks = 0;
auto failures = 0;

void Check(bool ok, int line) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("FAIL line %d\n", line);
	}
}

#define CHECK(value) Check(bool(value), __LINE__)

} // namespace

int main() {
	using Purple::FormatVersion;

	CHECK(FormatVersion(u"1.0.0"_q, u"864b1934d3"_q, false, u"337829e"_q)
		== u"Purple 1.0.0 (864b1934d3, core 337829e)"_q);
	CHECK(FormatVersion(u"1.2.3"_q, u"864b1934d3"_q, true, u"337829e"_q)
		== u"Purple 1.2.3 (864b1934d3+dirty, core 337829e)"_q);
	CHECK(FormatVersion(u"1.0.0"_q, u"unknown"_q, false, u"unknown"_q)
		== u"Purple 1.0.0 (unknown, core unknown)"_q);

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
