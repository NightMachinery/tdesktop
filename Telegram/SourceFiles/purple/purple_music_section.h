#pragma once

namespace Ui { class VerticalLayout; }
namespace Window { class SectionWidget; class SessionController; }

namespace Purple {

void AddMusicMenuRow(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);

bool ShowMusicSection(not_null<Window::SessionController*> controller);
bool ToggleMusicSection(not_null<Window::SessionController*> controller);
[[nodiscard]] bool MusicAllowsModalKeys(
	not_null<Window::SessionController*> controller,
	not_null<QWidget*> focus);
[[nodiscard]] bool MusicSearchContains(
	not_null<Window::SessionController*> controller,
	QWidget *focus);
void FocusMusicSearch(
	not_null<Window::SessionController*> controller,
	QWidget *focus,
	bool search);
[[nodiscard]] bool MusicHidesChats(const Window::SectionWidget *section);
void UpdateMusicSidebar(QWidget *widget, bool suppress, bool restore);
[[nodiscard]] bool MusicHideChatsCurrent();
[[nodiscard]] rpl::producer<bool> MusicHideChatsValue();
void SetMusicHideChats(bool hide);
void ChooseMusicSourceFolder(not_null<Window::SessionController*> controller);

} // namespace Purple
