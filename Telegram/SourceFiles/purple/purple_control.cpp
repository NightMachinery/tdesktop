#include "purple/purple_control.h"

#include "purple/purple_control_server.h"
#include "purple/purple_emoji_export.h"
#include "purple/purple_version.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "platform/platform_specific.h"
#include "settings.h"
#include "window/window_controller.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QPointer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <rpl/event_stream.h>
#include <rpl/variable.h>
#include <algorithm>

namespace Core {

template <>
std::optional<QByteArray> Settings::readPrefImpl<QByteArray>(std::string_view key) {
	return readPrefGeneric(key);
}

template <>
void Settings::writePrefImpl<QByteArray>(std::string_view key, QByteArray value) {
	writePrefGeneric(key, value);
}

}

namespace Purple {
namespace {

constexpr auto kEnabledKey = std::string_view("automation.enabled");
constexpr auto kPathKey = std::string_view("automation.socket-path");
constexpr auto kGroupKeys = std::array<std::string_view, kControlGroupCount>{
	"automation.window", "automation.media.control", "automation.media.info",
	"automation.lock", "automation.emoji",
};
rpl::event_stream<> SettingsChanges;
rpl::variable<QString> SocketPath;
rpl::variable<QString> SocketStatus;
ControlConsent Consent;

struct WindowEntry {
	not_null<Window::Controller*> controller;
	QWidget *window = nullptr;
};

std::vector<WindowEntry> CollectControlWindows() {
	auto result = std::vector<WindowEntry>();
	for (const auto widget : QApplication::topLevelWidgets()) {
		const auto controller = Core::App().findWindow(widget);
		if (!controller || std::any_of(result.begin(), result.end(), [&](const auto &entry) {
			return entry.controller.get() == controller;
		})) continue;
		result.push_back({ controller, widget->window() });
		if (result.size() > 64) break;
	}
	std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
		return a.controller->isPrimary() != b.controller->isPrimary()
			? a.controller->isPrimary() : std::less<Window::Controller*>()(
				a.controller.get(), b.controller.get());
	});
	return result;
}

ControlWindowAdapter WindowAdapter() {
	return {
		.list = [] {
			auto result = std::vector<ControlWindow>();
			const auto active = Core::App().activeWindow();
			for (const auto &entry : CollectControlWindows()) {
				result.push_back({ entry.window->windowTitle(), entry.controller->isPrimary(),
					entry.controller.get() == active });
				if (result.size() > 64) break;
			}
			return result;
		},
		.activate = [](int index) {
			const auto windows = CollectControlWindows();
			if (index < 0 || index >= windows.size()) return false;
			windows[index].controller->activate();
			return true;
		},
	};
}

class ControlService final : public QObject {
public:
	ControlService();
	void refreshPolicy();
	void reconfigure();

private:
	ControlServer _server;
	rpl::lifetime _lifetime;

};

QPointer<ControlService> Service;

ControlService::ControlService()
: QObject(QCoreApplication::instance())
, _server([](const ControlRequest &request, const ControlPolicy &policy, ControlReply reply) {
	DispatchControlRequest(request, policy, VersionText(), WindowAdapter(), std::move(reply), RequestEmojiRender, AppControlMediaAdapter());
}, [](QString path, QString error) {
	SocketPath = path;
	SocketStatus = error.isEmpty() ? path : error;
}, this) {
	Core::App().passcodeLockChanges() | rpl::on_next([=] { refreshPolicy(); }, _lifetime);
	connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [=] {
		Consent.invalidate();
		_server.stop();
	});
	refreshPolicy();
	reconfigure();
}

void ControlService::refreshPolicy() {
	auto policy = ControlPolicy();
	policy.enabled = ControlAutomationEnabled();
	policy.locked = Core::App().passcodeLocked();
	for (auto i = 0; i < kControlGroupCount; ++i) policy.groups[i] = ControlGroupEnabled(i);
	_server.setPolicy(policy);
}

void ControlService::reconfigure() {
	_server.configure({
		.workingDirectory = cWorkingDir(),
		.configuredPath = ConfiguredControlSocketPath(),
		.defaultDirectory = [] { return psAppDataPath(); },
		.fallbackPath = QDir::homePath() + u"/Library/Application Support/Purple/control.sock"_q,
	});
}

void Changed(bool path = false) {
	Consent.invalidate();
	if (Service) {
		Service->refreshPolicy();
		if (path) Service->reconfigure();
	}
	SettingsChanges.fire({});
}

}

void StartControlServer() {
	if (!Service) Service = new ControlService();
}

QString CurrentControlSocketPath() {
	return SocketPath.current();
}

rpl::producer<QString> ControlSocketPathValue() {
	return SocketPath.value();
}

rpl::producer<QString> ControlSocketStatusValue() {
	return SocketStatus.value();
}

rpl::producer<> ControlSettingsChanges() {
	return rpl::single(rpl::empty) | rpl::then(SettingsChanges.events());
}

bool ControlAutomationEnabled() {
	return Core::App().settings().readPref<bool>(kEnabledKey, false);
}

void DisableControlAutomation() {
	Core::App().settings().writePref<bool>(kEnabledKey, false);
	Changed();
}

quint64 BeginControlConsent() {
	const auto token = Consent.begin();
	SettingsChanges.fire({});
	return token;
}

bool AdvanceControlConsent(quint64 token) {
	return Consent.advance(token);
}

bool ConfirmControlConsent(quint64 token) {
	if (!Consent.confirm(token)) return false;
	Core::App().settings().writePref<bool>(kEnabledKey, true);
	Changed();
	return true;
}

void CancelControlConsent(quint64 token) {
	Consent.cancel(token);
	SettingsChanges.fire({});
}

bool ControlGroupEnabled(int index) {
	return index >= 0 && index < kControlGroupCount
		&& Core::App().settings().readPref<bool>(kGroupKeys[index], true);
}

void SetControlGroupEnabled(int index, bool enabled) {
	if (index < 0 || index >= kControlGroupCount) return;
	Core::App().settings().writePref<bool>(kGroupKeys[index], enabled);
	Changed();
}

QString ConfiguredControlSocketPath() {
	return DecodeControlSocketPath(Core::App().settings().readPref<QByteArray>(kPathKey));
}

bool SetControlSocketPath(const QString &path) {
	if (!ValidControlSocketPath(path)) return false;
	Core::App().settings().writePref<QByteArray>(kPathKey, path.toUtf8());
	Changed(true);
	return true;
}

}
