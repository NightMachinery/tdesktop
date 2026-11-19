#pragma once

namespace Ui { class VerticalLayout; }
namespace Window { class SessionController; }

namespace Purple {

void AddMusicMenuRow(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);

void ShowMusicSection(not_null<Window::SessionController*> controller);
void ChooseMusicSourceFolder(not_null<Window::SessionController*> controller);

} // namespace Purple
