#pragma once

#include "base/basic_types.h"

class QWidget;

namespace Purple {

void RegisterMessageBarHint(QWidget *widget, Fn<bool()> shown);

} // namespace Purple
