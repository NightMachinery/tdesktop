/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/assertion.h"
#include "base/basic_types.h"
#include "ui/text/text_entity.h"

#include <crl/crl_time.h>
#include <rpl/lifetime.h>
#include <rpl/producer.h>
#include <QtCore/QRect>

#include "purple/purple_config.h"
#include "purple/purple_last_seen.h"

class PeerData;

namespace Ui {
class FlatLabel;
class RoundButton;
template <typename Widget>
class FadeWrap;
} // namespace Ui

namespace Purple {

struct LastSeenTail {
	int from = 0;
	bool shown = false;
};

struct ProfileStatusText {
	TextWithEntities text;
	bool hasLink = false;
};

void OnLastSeenInputsChanged(Fn<void()> refresh, rpl::lifetime &lifetime);
[[nodiscard]] QString TopBarOnlineText(
	not_null<UserData*> user,
	TimeId now,
	int available,
	LastSeenTail &tail);
[[nodiscard]] QRect LastSeenTailRect(
	const LastSeenTail &tail,
	float64 narrowRatio,
	int leftTaken,
	int rightTaken,
	int outerWidth,
	int fullWidth);
[[nodiscard]] bool HandleLastSeenTailClick(
	not_null<Window::SessionController*> controller,
	PeerData *peer,
	QPoint position,
	const QRect &rect);
void AttachProfileStatus(
	not_null<Ui::FlatLabel*> label,
	not_null<PeerData*> peer,
	Fn<void()> refresh,
	rpl::lifetime &lifetime);
void RefreshProfileStatus(not_null<PeerData*> peer, Fn<void()> refresh);
[[nodiscard]] ProfileStatusText ProfileStatusBody(
	not_null<UserData*> user,
	TimeId now,
	bool colorized);
void ApplyLastSeenLink(
	not_null<Ui::FlatLabel*> label,
	UserData *user,
	bool hasLink);
[[nodiscard]] bool SetupShowLastSeen(
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user,
	not_null<Ui::FadeWrap<Ui::RoundButton>*> button);
[[nodiscard]] QString RowOnlineText(not_null<UserData*> user, TimeId now);
[[nodiscard]] crl::time RowOnlineChangeTimeout(
	not_null<UserData*> user,
	TimeId now);

} // namespace Purple
