/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <QtCore/QString>

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

template <typename Signature>
using Fn = std::function<Signature>;

template <typename T>
using not_null = T;

struct MsgId {
};

#include "purple/purple_sync.h"

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

void CheckOfferVerdicts() {
	using Purple::ImportOfferVerdict;
	using Purple::JudgeImportOffer;

	CHECK(JudgeImportOffer(false, 10, 5, 200, 100)
		== ImportOfferVerdict::InactiveAccount);
	CHECK(JudgeImportOffer(false, 3, 5, 200, 100)
		== ImportOfferVerdict::InactiveAccount);
	CHECK(JudgeImportOffer(false, 10, 5, 50, 100)
		== ImportOfferVerdict::InactiveAccount);

	CHECK(JudgeImportOffer(true, 5, 5, 200, 100)
		== ImportOfferVerdict::AlreadyOffered);
	CHECK(JudgeImportOffer(true, 4, 5, 200, 100)
		== ImportOfferVerdict::AlreadyOffered);
	CHECK(JudgeImportOffer(true, 6, 5, 100, 100)
		== ImportOfferVerdict::NotNewer);
	CHECK(JudgeImportOffer(true, 6, 5, 99, 100)
		== ImportOfferVerdict::NotNewer);
	CHECK(JudgeImportOffer(true, 6, 5, 101, 100)
		== ImportOfferVerdict::Offer);
	CHECK(JudgeImportOffer(true, 1, 0, 1, 0)
		== ImportOfferVerdict::Offer);
}

void CheckAccountLabels() {
	using Purple::ImportAccountLabel;

	CHECK(ImportAccountLabel(u"Ada"_q, QString()) == u"Ada"_q);
	CHECK(ImportAccountLabel(u"Ada Test"_q, u"tester"_q)
		== u"Ada Test (@tester)"_q);
	CHECK(ImportAccountLabel(u"  Ada  "_q, u"tester"_q)
		== u"Ada (@tester)"_q);
	CHECK(ImportAccountLabel(QString(), u"tester"_q) == u"@tester"_q);
	CHECK(ImportAccountLabel(u"  "_q, u"tester"_q) == u"@tester"_q);
	CHECK(ImportAccountLabel(u"  "_q, QString()).isEmpty());
}

} // namespace

int main() {
	CheckOfferVerdicts();
	CheckAccountLabels();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
