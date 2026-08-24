/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Purple {

// The reference the app drops beside settings.toml, so the answer to "what can
// I write in here" is in the directory being edited rather than in a repository
// the user may not have. Generated, and said to be generated at the top of
// itself: it is rewritten on every launch and anything typed into it is lost.
[[nodiscard]] QString ReadmeFilePath();

// Writes it if what is there differs, and does nothing at all if it does not -
// so an ordinary launch leaves the mtime alone and the directory watch never
// wakes for it. Called before the watcher starts, so even the first write of a
// fresh install cannot queue a reload of settings.toml.
void RefreshReadme();

} // namespace Purple
