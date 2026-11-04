#pragma once

#include "base/basic_types.h"
#include <QtCore/QRect>

class QPainter;
namespace Ui { class RpWidget; class RoundVideoRecorder; }
namespace ChatHelpers { class Show; }

namespace Purple {

void BindRoundRecorder(
	not_null<Ui::RoundVideoRecorder*> recorder,
	Fn<void()> pause,
	std::shared_ptr<ChatHelpers::Show> show);
[[nodiscard]] bool RoundRecorderFloats(
	not_null<Ui::RoundVideoRecorder*> recorder,
	not_null<Ui::RpWidget*> preview,
	QSize outer,
	int side);
void PaintRoundRecorder(not_null<Ui::RpWidget*> preview, QPainter &painter);
[[nodiscard]] QRect RoundRecorderCanvas(not_null<Ui::RpWidget*> preview);

} // namespace Purple
