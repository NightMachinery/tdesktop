#pragma once

#include "base/system_unlock.h"

namespace Purple {

#ifdef Q_OS_MAC
void SuggestSystemUnlock(
	not_null<QWidget*> parent,
	const QString &text,
	Fn<void(base::SystemUnlockResult)> done,
	Fn<void()> usePasscode);
#else
inline void SuggestSystemUnlock(
		not_null<QWidget*> parent,
		const QString &text,
		Fn<void(base::SystemUnlockResult)> done,
		Fn<void()>) {
	base::SuggestSystemUnlock(parent, text, std::move(done));
}
#endif

} // namespace Purple
