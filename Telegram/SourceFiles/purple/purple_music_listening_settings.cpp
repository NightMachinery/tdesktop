#include "purple/hooks/music_listening.h"

#include "purple/purple_config.h"

#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "ui/widgets/buttons.h"

#include "styles/style_settings.h"

namespace Purple {

rpl::producer<bool> PrivateMusicListeningValue() {
	return rpl::single(rpl::empty) | rpl::then(
		SettingsChanges()
	) | rpl::map([] { return ActiveSettings().playback.privateListening; });
}

void AddPrivateMusicListeningSetting(
		::Settings::Builder::SectionBuilder &builder) {
	const auto toggle = builder.addButton({
		.id = u"advanced/purple_private_music_listening"_q,
		.title = tr::lng_purple_private_music_listening(),
		.st = &st::settingsButtonNoIcon,
		.toggled = PrivateMusicListeningValue(),
		.keywords = { u"purple"_q, u"music"_q, u"privacy"_q, u"listening"_q },
	});
	if (toggle) {
		const auto editor = CaptureAppWideSettingsEditor(toggle);
		toggle->toggledValue() | rpl::filter([](bool value) {
			return value != ActiveSettings().playback.privateListening;
		}) | rpl::on_next([=](bool value) {
			WriteSettingsFromEditor(editor, [=](const QString &text) {
				return SetTableBool(
					text,
					SettingsFilePath(),
					u"playback"_q,
					u"private_listening_p"_q,
					value);
			}, u"private music listening"_q, [] { return true; });
		}, toggle->lifetime());
	}
	builder.addDividerText(tr::lng_purple_private_music_listening_about());
	builder.addSkip();
}

} // namespace Purple
