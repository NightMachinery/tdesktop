#pragma once

#include "ui/rp_widget.h"

namespace Main { class Session; }

namespace Purple {

[[nodiscard]] object_ptr<Ui::RpWidget> MakeMusicNowPlaying(
	QWidget *parent,
	not_null<Main::Session*> session,
	bool transport);

} // namespace Purple
