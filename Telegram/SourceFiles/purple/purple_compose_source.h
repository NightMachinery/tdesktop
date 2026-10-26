#pragma once

#include "base/basic_types.h"
#include <memory>

class HistoryItem;
namespace Api { struct MessageToSend; }
namespace Main { class Session; }
namespace Ui { class InputField; }

namespace Purple {

struct DesktopComposeSource;
void PrepareDesktopCompose(Api::MessageToSend &message, Ui::InputField *field);
[[nodiscard]] Fn<void(const MTPUpdates &)> BindDesktopCompose(
	const std::shared_ptr<DesktopComposeSource> &source,
	not_null<Main::Session*> session,
	not_null<HistoryItem*> item,
	uint64 randomId,
	const TextWithEntities &sending,
	const QVector<MTPMessageEntity> &entities,
	bool singlePart);

}
