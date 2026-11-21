#pragma once

#include "base/system_unlock.h"

namespace Purple {

#ifdef Q_OS_MAC
void SuggestSystemUnlock(
	not_null<QWidget*> parent,
	const QString &text,
	Fn<void(base::SystemUnlockResult)> done);
#else
using base::SuggestSystemUnlock;
#endif

} // namespace Purple
