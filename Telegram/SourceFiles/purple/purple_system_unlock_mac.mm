#include "purple/purple_system_unlock.h"

#include "base/platform/mac/base_utilities_mac.h"
#include "lang/lang_keys.h"

#include <AppKit/AppKit.h>
#include <LocalAuthentication/LocalAuthentication.h>
#include <LocalAuthenticationEmbeddedUI/LAAuthenticationView.h>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

namespace Purple {
namespace {

using Result = base::SystemUnlockResult;

struct EmbeddedPrompt {
	~EmbeddedPrompt() {
		[context release];
		[alert release];
	}

	QPointer<QWidget> parent;
	Fn<void(Result)> done;
	LAContext *context = nil;
	NSAlert *alert = nil;
	id resignObserver = nil;
	id keyObserver = nil;
	QMetaObject::Connection destroyed;
};

std::shared_ptr<EmbeddedPrompt> ActivePrompt;
bool SystemPromptPending = false;

NSWindow *KeyWindow(not_null<QWidget*> parent) {
	if (!NSApp.isActive || !parent->isVisible()) {
		return nil;
	}
	const auto view = reinterpret_cast<NSView*>(
		parent->window()->effectiveWinId());
	const auto window = view.window;
	return (window.isKeyWindow && !window.attachedSheet) ? window : nil;
}

void StartSystemPrompt(
		not_null<QWidget*> parent,
		const QString &text,
		Fn<void(Result)> done) {
	SystemPromptPending = true;
	base::SuggestSystemUnlock(parent, text, [done = std::move(done)](
			Result result) {
		done(result);
		crl::on_main([] { SystemPromptPending = false; });
	});
}

void Finish(
		const std::shared_ptr<EmbeddedPrompt> &prompt,
		Result result,
		QString passwordReason = {}) {
	if (ActivePrompt != prompt) {
		return;
	}
	ActivePrompt.reset();
	QObject::disconnect(prompt->destroyed);
	[[NSNotificationCenter defaultCenter] removeObserver:prompt->resignObserver];
	[[NSNotificationCenter defaultCenter] removeObserver:prompt->keyObserver];
	[prompt->context invalidate];
	const auto sheet = prompt->alert.window;
	[sheet.sheetParent endSheet:sheet];
	if (passwordReason.isEmpty()) {
		prompt->done(result);
	} else {
		SystemPromptPending = true;
		crl::on_main([prompt, reason = std::move(passwordReason)] {
			SystemPromptPending = false;
			if (const auto parent = prompt->parent.data(); parent && KeyWindow(parent)) {
				StartSystemPrompt(parent, reason, std::move(prompt->done));
			} else {
				prompt->done(Result::Cancelled);
			}
		});
	}
}

} // namespace

void SuggestSystemUnlock(
		not_null<QWidget*> parent,
		const QString &text,
		Fn<void(base::SystemUnlockResult)> done) {
	if (ActivePrompt || SystemPromptPending || !KeyWindow(parent)) {
		done(Result::Cancelled);
		return;
	}
	const auto context = [[LAContext alloc] init];
	const auto touchId = [context canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
		error:nil];
	const auto policy = LAPolicyDeviceOwnerAuthenticationWithBiometricsOrWatch;
	const auto available = [context canEvaluatePolicy:policy error:nil];
	const auto window = KeyWindow(parent);
	if (!window) {
		[context release];
		done(Result::Cancelled);
		return;
	}
	if (!available) {
		[context release];
		StartSystemPrompt(parent, text, std::move(done));
		return;
	}
	const auto prompt = std::make_shared<EmbeddedPrompt>();
	ActivePrompt = prompt;
	prompt->parent = parent.get();
	prompt->done = std::move(done);
	prompt->context = context;
	prompt->alert = [[NSAlert alloc] init];
	const auto alert = prompt->alert;
	using Platform::Q2NSString;
	alert.messageText = Q2NSString(touchId
		? tr::lng_settings_use_touchid(tr::now)
		: tr::lng_settings_use_applewatch(tr::now));
	const auto view = [[LAAuthenticationView alloc] initWithContext:context];
	view.frame = NSMakeRect(0, 0, view.fittingSize.width, view.fittingSize.height);
	alert.accessoryView = view;
	[view release];
	const auto cancel = [alert addButtonWithTitle:Q2NSString(tr::lng_cancel(tr::now))];
	cancel.keyEquivalent = @"\033";
	[alert addButtonWithTitle:Q2NSString(tr::lng_settings_use_systempwd(tr::now))];
	const auto weak = std::weak_ptr(prompt);
	prompt->destroyed = QObject::connect(parent.get(), &QObject::destroyed, [weak] {
		if (const auto strong = weak.lock()) {
			Finish(strong, Result::Cancelled);
		}
	});
	prompt->resignObserver = [[NSNotificationCenter defaultCenter]
		addObserverForName:NSApplicationDidResignActiveNotification
		object:NSApp
		queue:nil
		usingBlock:^(NSNotification *) {
			if (const auto strong = weak.lock()) {
				Finish(strong, Result::Cancelled);
			}
		}];
	prompt->keyObserver = [[NSNotificationCenter defaultCenter]
		addObserverForName:NSWindowDidBecomeKeyNotification
		object:nil
		queue:nil
		usingBlock:^(NSNotification *notification) {
			if (const auto strong = weak.lock()) {
				if (notification.object != window
					&& notification.object != strong->alert.window) {
					Finish(strong, Result::Cancelled);
				}
			}
		}];
	[alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
		if (const auto strong = weak.lock()) {
			Finish(strong, Result::Cancelled,
				(response == NSAlertSecondButtonReturn) ? text : QString());
		}
	}];
	crl::on_main([weak, text] {
		if (const auto strong = weak.lock(); strong && strong == ActivePrompt) {
			[strong->context evaluatePolicy:policy localizedReason:Q2NSString(text)
				reply:^(BOOL success, NSError *error) {
					const auto result = success
						? Result::Success
						: (error.code == LAErrorBiometryLockout)
						? Result::FloodError
						: Result::Cancelled;
					crl::on_main([weak, result] {
						if (const auto request = weak.lock()) {
							Finish(request, result);
						}
					});
				}];
		}
	});
}

} // namespace Purple
