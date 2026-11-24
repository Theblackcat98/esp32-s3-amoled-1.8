#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     online;
    bool     vbus_present;
    bool     batt_present;
    bool     charging;
    uint16_t batt_mv;
    int      batt_pct;
} board_pmu_telemetry_t;

typedef enum {
    BOARD_PWR_BTN_NONE = 0,
    BOARD_PWR_BTN_SHORT,
    BOARD_PWR_BTN_LONG,
} board_pwr_btn_event_t;

esp_err_t board_pmu_init(i2c_master_bus_handle_t i2c_bus);
bool      board_pmu_is_online(void);
void      board_pmu_poll(board_pmu_telemetry_t *out, board_pwr_btn_event_t *btn_event);
esp_err_t board_pmu_shutdown(void);

#ifdef __cplusplus
}
#endif
