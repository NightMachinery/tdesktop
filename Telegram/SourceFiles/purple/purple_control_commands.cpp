#include "purple/purple_control_commands.h"

#include "purple/purple_emoji_large.h"

#include "base/basic_types.h"
#include <algorithm>

namespace Purple {
namespace {

const auto Commands = std::array{
	u"hello"_q, u"ping"_q, u"window.list"_q, u"window.activate"_q, u"window.cycle"_q, u"emoji.render"_q,
};

}

quint64 ControlConsent::begin() {
	_stage = 1;
	return ++_token;
}

bool ControlConsent::advance(quint64 token) {
	if (token != _token || _stage != 1) return false;
	_stage = 2;
	return true;
}

bool ControlConsent::confirm(quint64 token) {
	if (token != _token || _stage != 2) return false;
	invalidate();
	return true;
}

void ControlConsent::cancel(quint64 token) {
	if (token == _token) invalidate();
}

void ControlConsent::invalidate() {
	++_token;
	_stage = 0;
}

QString ControlGroupName(int index) {
	const auto groups = std::array{
		u"window"_q, u"media.control"_q, u"media.info"_q, u"lock"_q, u"emoji"_q,
	};
	return (index >= 0 && index < groups.size()) ? groups[index] : QString();
}

QJsonArray ControlCommandNames() {
	auto result = QJsonArray();
	for (const auto &name : Commands) result.append(name);
	return result;
}

ControlResult AuthorizeControlCommand(const QString &command, const ControlPolicy &policy) {
	if (command == u"hello"_q) return {};
	if (!policy.enabled) {
		return ControlError(u"automation-disabled"_q, u"Enable Local automation in Purple Settings."_q);
	}
	if (std::find(Commands.begin(), Commands.end(), command) == Commands.end()) {
		return ControlError(u"unsupported"_q, u"This command is not implemented by this build."_q);
	}
	if (command.startsWith(u"window."_q)) {
		if (!policy.groups[0]) return ControlError(u"capability-off"_q, u"The window capability is disabled."_q);
		if (policy.locked) return ControlError(u"locked"_q, u"Window automation is unavailable while locked."_q);
	}
	if (command == u"emoji.render"_q) {
		if (!policy.groups[4]) return ControlError(u"capability-off"_q, u"The emoji capability is disabled."_q);
		if (policy.locked) return ControlError(u"locked"_q, u"Emoji automation is unavailable while locked."_q);
	}
	return {};
}

void DispatchControlRequest(
		const ControlRequest &request,
		const ControlPolicy &policy,
		const QString &build,
		const ControlWindowAdapter &windows,
		ControlReply reply,
		const ControlEmojiAdapter &emoji) {
	if (auto error = AuthorizeControlCommand(request.command, policy)
		; !error.code.isEmpty()) {
		reply(std::move(error));
		return;
	}
	if (request.command == u"hello"_q) {
		auto groups = QJsonArray();
		for (auto i = 0; i < kControlGroupCount; ++i) {
			if (policy.enabled && policy.groups[i]) groups.append(ControlGroupName(i));
		}
		reply({ QJsonObject{
			{ u"version"_q, 1 }, { u"build"_q, build.left(256) },
			{ u"automation"_q, policy.enabled }, { u"groups"_q, groups },
			{ u"commands"_q, ControlCommandNames() },
		} });
		return;
	}
	if (request.command == u"ping"_q) {
		reply({ QJsonObject{ { u"pong"_q, true } } });
		return;
	}
	if (request.command == u"emoji.render"_q) {
		const auto id = ControlUnsignedArgument(request, u"id"_q);
		const auto pixels = request.arguments.contains(u"px"_q)
			? ControlIntegerArgument(request, u"px"_q) : std::optional<qint64>(kEmojiLargePixels);
		if (!id || !pixels || *pixels < 1 || *pixels > kEmojiLargePixels) {
			reply(ControlError(u"bad-request"_q, u"Expected a nonzero uint64 id and px from 1 to 512."_q));
		} else if (!emoji) {
			reply(ControlError(u"export-disabled"_q, u"Enable local emoji export in Purple Settings."_q));
		} else {
			emoji(*id, int(*pixels), request.active, std::move(reply));
		}
		return;
	}
	const auto list = windows.list ? windows.list() : std::vector<ControlWindow>();
	if (list.size() > 64) {
		reply(ControlError(u"busy"_q, u"Too many windows to describe."_q));
		return;
	}
	if (request.command == u"window.list"_q) {
		auto array = QJsonArray();
		for (auto i = 0; i < list.size(); ++i) {
			array.append(QJsonObject{
				{ u"id"_q, i }, { u"title"_q, list[i].title.left(128) },
				{ u"primary"_q, list[i].primary }, { u"active"_q, list[i].active },
			});
		}
		reply({ QJsonObject{ { u"windows"_q, array } } });
		return;
	}
	auto index = qint64(0);
	if (request.command == u"window.activate"_q) {
		const auto requested = ControlIntegerArgument(request, u"index"_q);
		if (!requested) {
			reply(ControlError(u"bad-request"_q, u"index must be an integer."_q));
			return;
		}
		index = *requested;
	} else if (!list.empty()) {
		const auto active = std::find_if(list.begin(), list.end(), [](const auto &window) { return window.active; });
		index = ((active == list.end() ? 0 : active - list.begin()) + 1) % list.size();
	}
	if (index < 0 || index >= list.size() || !windows.activate || !windows.activate(int(index))) {
		reply(ControlError(u"not-found"_q, u"No such window."_q));
		return;
	}
	reply({ QJsonObject{ { u"activated"_q, index } } });
}

}
