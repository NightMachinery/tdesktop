#include "purple/purple_control.h"

#include "purple/purple_control_commands.h"
#include "base/weak_ptr.h"
#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Purple {
namespace {

void ConfirmAutomation(
		base::weak_ptr<Window::SessionController> window,
		quint64 token,
		bool second) {
	if (!window) { CancelControlConsent(token); return; }
	window->show(Ui::MakeConfirmBox({
		.text = second ? tr::lng_purple_control_confirm_twice(tr::now)
			: tr::lng_purple_control_confirm(tr::now),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (!window) { CancelControlConsent(token); return; }
			if (second) {
				(void)ConfirmControlConsent(token);
			} else if (AdvanceControlConsent(token)) {
				ConfirmAutomation(window, token, true);
			}
		},
		.cancelled = [=](Fn<void()> close) { CancelControlConsent(token); close(); },
		.confirmText = tr::lng_purple_control_enable(),
		.title = tr::lng_purple_control_title(),
	}));
}

void SocketPathBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_purple_control_path());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
		tr::lng_purple_control_path_placeholder(), ConfiguredControlSocketPath()));
	box->setFocusCallback([=] { field->setFocusFast(); });
	const auto save = [=] {
		if (!SetControlSocketPath(field->getLastText())) { field->showError(); return; }
		box->closeBox();
	};
	field->submits() | rpl::on_next(save, field->lifetime());
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

rpl::producer<QString> GroupTitle(int index) {
	switch (index) {
	case 0: return tr::lng_purple_control_window();
	case 1: return tr::lng_purple_control_media_control();
	case 2: return tr::lng_purple_control_media_info();
	case 3: return tr::lng_purple_control_lock();
	default: return tr::lng_purple_control_emoji();
	}
}

}

void BuildControlSettings(::Settings::Builder::SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_control"_q,
		.title = tr::lng_purple_control_title(),
		.keywords = { u"purple"_q, u"automation"_q, u"socket"_q },
	});
	const auto controller = builder.controller();
	const auto window = controller ? base::make_weak(controller)
		: base::weak_ptr<Window::SessionController>();
	const auto master = builder.addButton({
		.id = u"advanced/purple_control_enabled"_q,
		.title = tr::lng_purple_control_title(),
		.st = &st::settingsButtonNoIcon,
		.toggled = ControlSettingsChanges() | rpl::map([] { return ControlAutomationEnabled(); }),
		.keywords = { u"automation"_q, u"control"_q },
	});
	if (master) {
		master->toggledValue() | rpl::filter([](bool enabled) {
			return enabled != ControlAutomationEnabled();
		}) | rpl::on_next([=](bool enabled) {
			if (!enabled) DisableControlAutomation();
			else ConfirmAutomation(window, BeginControlConsent(), false);
		}, master->lifetime());
	}
	builder.addDividerText(tr::lng_purple_control_about());
	builder.addButton({
		.id = u"advanced/purple_control_path"_q,
		.title = tr::lng_purple_control_path(),
		.st = &st::settingsButtonNoIcon,
		.label = ControlSettingsChanges() | rpl::map([] {
			const auto path = ConfiguredControlSocketPath();
			return path.isEmpty() ? tr::lng_purple_control_default(tr::now) : path;
		}),
		.onClick = [=] { if (window) window->show(Box(SocketPathBox)); },
		.keywords = { u"socket"_q, u"path"_q },
	});
	builder.addDividerText(ControlSocketStatusValue() | rpl::map([](const QString &status) {
		return status.isEmpty() ? tr::lng_purple_control_no_socket(tr::now) : status;
	}));
	for (auto index = 0; index < kControlGroupCount; ++index) {
		const auto toggle = builder.addButton({
			.id = u"advanced/purple_control_group_"_q + ControlGroupName(index),
			.title = GroupTitle(index),
			.st = &st::settingsButtonNoIcon,
			.toggled = ControlSettingsChanges() | rpl::map([=] { return ControlGroupEnabled(index); }),
			.keywords = { u"automation"_q, ControlGroupName(index) },
		});
		if (toggle) {
			toggle->toggledValue() | rpl::filter([=](bool value) {
				return value != ControlGroupEnabled(index);
			}) | rpl::on_next([=](bool value) {
				SetControlGroupEnabled(index, value);
			}, toggle->lifetime());
		}
	}
	builder.addDividerText(tr::lng_purple_control_groups_about());
}

}
