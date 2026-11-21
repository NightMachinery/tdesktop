#include "purple/purple_system_unlock.h"

#include <AppKit/AppKit.h>
#include <QtWidgets/QWidget>

namespace Purple {
namespace {

bool PromptPending = false;

bool HasKeyWindow(not_null<QWidget*> parent) {
	if (!NSApp.isActive || !parent->isVisible()) {
		return false;
	}
	const auto view = reinterpret_cast<NSView*>(
		parent->window()->effectiveWinId());
	return view.window.isKeyWindow;
}

} // namespace

void SuggestSystemUnlock(
		not_null<QWidget*> parent,
		const QString &text,
		Fn<void(base::SystemUnlockResult)> done) {
	if (PromptPending || !HasKeyWindow(parent)) {
		done(base::SystemUnlockResult::Cancelled);
		return;
	}
	PromptPending = true;
	base::SuggestSystemUnlock(parent, text, [done = std::move(done)](
			base::SystemUnlockResult result) {
		done(result);
		crl::on_main([] { PromptPending = false; });
	});
}

} // namespace Purple
