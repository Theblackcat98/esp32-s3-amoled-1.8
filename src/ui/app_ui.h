#pragma once

#include "bsp/board_bsp.h"

namespace ui {

void init(bsp::BoardBsp &board);
void update(const bsp::Telemetry &telemetry, const bsp::TouchPoint &last_touch);

}  // namespace ui
