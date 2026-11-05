/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_restart.h"

#include <crl/crl_on_main.h>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

namespace Purple {

void QueueHistoryMediaRestart(
		not_null<QWidget*> context,
		rpl::producer<> playerLifetime,
		AudioMsgId identity,
		Fn<AudioMsgId()> current,
		Fn<void()> restart) {
	const auto window = QPointer<QWidget>(context->window());
	const auto alive = std::make_shared<bool>(true);
	// Player updates complete when the non-QObject player is destroyed.
	auto subscription = std::move(playerLifetime) | rpl::on_done([=] {
		*alive = false;
	});
	crl::on_main(context, [
		window,
		alive,
		subscription = std::move(subscription),
		identity,
		current = std::move(current),
		restart = std::move(restart)
	]() mutable {
		subscription.destroy();
		if (window && *alive && current() == identity) {
			restart();
		}
	});
}

} // namespace Purple
