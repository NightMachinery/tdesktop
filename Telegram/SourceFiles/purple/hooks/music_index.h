#pragma once

namespace Main { class Account; class Session; }

namespace Purple {

void ObserveMusicIndexArrivals(Main::Session &session);
void StopMusicIndex(Main::Account &account);

} // namespace Purple
