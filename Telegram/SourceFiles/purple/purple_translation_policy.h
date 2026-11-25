/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"
#include "spellcheck/spellcheck_types.h"
#include "ui/text/text_entity.h"

#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

class History;
class PeerData;

namespace Purple {

inline constexpr auto kTranslationRecognitionThreshold = 10;

[[nodiscard]] rpl::producer<bool> TranslationPremiumValue(
	not_null<Main::Session*> session);
[[nodiscard]] bool TranslationPremiumAvailableForSession(
	not_null<Main::Session*> session);
void UpdateTranslationOffer(
	not_null<History*> history,
	const std::vector<LanguageId> &recognized,
	int count,
	bool allLoaded);

[[nodiscard]] bool ChatTranslationExcluded(not_null<PeerData*> peer);
[[nodiscard]] bool ChatTranslationDisabled(not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<bool> ChatTranslationExcludedValue(
	not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<> ChatTranslationExcludedChanges(
	not_null<Main::Session*> session);
void SetChatTranslationExcluded(not_null<PeerData*> peer, bool excluded);
void AddTranslateBarExcludeAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<PeerData*> peer);
void AddExcludedTranslationChatsRow(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Purple
