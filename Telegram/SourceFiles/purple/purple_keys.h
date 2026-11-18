#pragma once

class QEvent;
class QObject;

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] bool HandleModalKeys(QObject *receiver, QEvent *event);

void BuildModalKeysSetting(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple
