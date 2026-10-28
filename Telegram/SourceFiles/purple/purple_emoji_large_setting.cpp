#include "purple/purple_emoji_large_setting.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "purple/purple_emoji_large.h"
#include "settings/settings_builder.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include <QtCore/QtEndian>
#include <array>

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Core {

template <>
std::optional<qint64> Settings::readPrefImpl<qint64>(std::string_view key) {
	const auto bytes = readPrefGeneric(key);
	return (bytes && bytes->size() == sizeof(qint64))
		? std::make_optional(qFromBigEndian<qint64>(bytes->constData()))
		: std::nullopt;
}

template <>
void Settings::writePrefImpl<qint64>(std::string_view key, qint64 value) {
	auto bytes = QByteArray(sizeof(qint64), Qt::Uninitialized);
	qToBigEndian(value, bytes.data());
	writePrefGeneric(key, bytes);
}

}

namespace Purple {
namespace {

constexpr auto kCapPreference = "purple-emoji-large-cache-bytes";
constexpr auto kMiB = qint64(1024) * 1024;
constexpr auto kCaps = std::array{100 * kMiB, 512 * kMiB, 1024 * kMiB,
	2048 * kMiB, 4096 * kMiB, 8192 * kMiB};
rpl::event_stream<qint64> CapChanges;

void ChooseEmojiLargeCacheCap(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_purple_emoji_large_cap());
	const auto current = EmojiLargeCacheCap();
	auto selected = -1;
	for (auto i = 0; i != int(kCaps.size()); ++i) {
		if (kCaps[i] == current) selected = i;
	}
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(selected);
	const auto container = box->verticalLayout();
	for (auto i = 0; i != int(kCaps.size()); ++i) {
		container->add(object_ptr<Ui::Radiobutton>(container, group, i,
			Ui::FormatSizeText(kCaps[i]), st::defaultCheckbox), st::boxRowPadding);
	}
	group->setChangedCallback([=](int selected) {
		if (selected < 0 || selected >= int(kCaps.size())) return;
		SetEmojiLargeCacheCap(kCaps[selected]);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

}

qint64 EmojiLargeCacheCap() {
	return NormalizeEmojiLargeCap(Core::App().settings().readPref<qint64>(
		kCapPreference, kEmojiLargeDefaultBytes));
}

void SetEmojiLargeCacheCap(qint64 bytes) {
	const auto normalized = NormalizeEmojiLargeCap(bytes);
	if (normalized == EmojiLargeCacheCap()) return;
	Core::App().settings().writePref<qint64>(kCapPreference, normalized);
	Core::App().saveSettingsDelayed();
	CapChanges.fire_copy(normalized);
}

rpl::producer<qint64> EmojiLargeCacheCapValue() {
	return rpl::single(EmojiLargeCacheCap()) | rpl::then(CapChanges.events());
}

void BuildEmojiLargeCacheSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto weak = controller ? base::make_weak(controller)
		: base::weak_ptr<Window::SessionController>();
	builder.addButton({
		.id = u"advanced/purple_emoji_large_cache"_q,
		.title = tr::lng_purple_emoji_large_cap(),
		.st = &st::settingsButtonNoIcon,
		.label = EmojiLargeCacheCapValue() | rpl::map([](qint64 bytes) {
			return Ui::FormatSizeText(bytes);
		}),
		.onClick = [=] {
			if (const auto live = weak.get()) live->show(Box(ChooseEmojiLargeCacheCap));
		},
		.keywords = {u"purple"_q, u"emoji"_q, u"cache"_q, u"size"_q},
	});
}

}
