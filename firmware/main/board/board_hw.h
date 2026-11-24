#pragma once

#include <stdbool.h>

#include "board_pmu.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_BOOT_BTN_NONE = 0,
    BOARD_BOOT_BTN_SHORT,
    BOARD_BOOT_BTN_LONG,
} board_boot_btn_event_t;

esp_err_t              board_hw_init(void);
void                   board_hw_set_brightness(int pct);
int                    board_hw_get_brightness(void);
board_boot_btn_event_t board_hw_poll_boot_button(void);
void                   board_hw_poll_pmu(board_pmu_telemetry_t *pmu_out, board_pwr_btn_event_t *pwr_btn_out);
esp_err_t              board_hw_shutdown(void);

#ifdef __cplusplus
}
#endif
