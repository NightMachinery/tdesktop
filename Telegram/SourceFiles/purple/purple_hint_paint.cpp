#include "purple/purple_hint_paint.h"

#include "purple/purple_config.h"
#include "styles/palette.h"
#include "styles/style_keys_legend.h"

namespace Purple {

HintPalette CurrentHintPalette() {
	const auto &settings = ActiveSettings().hints;
	auto result = HintPalette{
		.symbols = settings.symbols,
		.markWidth = st::keysLegendCapSkip * 2,
		.stroke = st::lineWidth,
		.radius = st::keysLegendCapRadius,
	};
	const auto accents = std::array{ st::windowSubTextFg->c, st::windowActiveTextFg->c,
		st::historyPeer5NameFg->c, st::historyPeer7NameFg->c, st::historyPeer7NameFg->c,
		st::historyPeer3NameFg->c, st::attentionButtonFg->c, st::attentionButtonFg->c };
	static_assert(accents.size() == int(HintRole::Count));
	for (auto i = size_t(0); i != accents.size(); ++i) {
		result.colors[i] = ResolveHintColors(st::windowBg->c, st::windowFg->c,
			accents[i], HintRole(i), settings.opacityPercent.value_or(80));
	}
	return result;
}

} // namespace Purple
