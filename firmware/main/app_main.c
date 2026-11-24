#include "app_config.h"
#include "app_prefs.h"
#include "audio_engine.h"
#include "board_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "session_mgr.h"
#include "ui_app.h"
#include "wifi_mgr.h"

static const char *TAG = "app_main";

static void dispatch_button_action(app_btn_action_t act) {
    switch (act) {
        case BTN_ACT_NONE:
            break;
        case BTN_ACT_START_OR_MUTE:
            if (session_mgr_get_state() == APP_STATE_READY || session_mgr_get_state() == APP_STATE_ERROR) {
                session_mgr_start_session();
            } else {
                session_mgr_toggle_mute();
                ui_app_show_toast(session_mgr_is_muted() ? "Mic Muted" : "Mic Live");
            }
            break;
        case BTN_ACT_TOGGLE_SESSION:
            session_mgr_toggle_session();
            break;
        case BTN_ACT_TOGGLE_MUTE:
            session_mgr_toggle_mute();
            ui_app_show_toast(session_mgr_is_muted() ? "Mic Muted" : "Mic Live");
            break;
        case BTN_ACT_INTERRUPT:
            session_mgr_force_interrupt();
            ui_app_show_toast("Interrupted");
            break;
        case BTN_ACT_VOL_UP:
            session_mgr_adjust_volume(10, true);
            break;
        case BTN_ACT_VOL_DOWN:
            session_mgr_adjust_volume(-10, true);
            break;
        case BTN_ACT_CYCLE_GUARD:
            session_mgr_cycle_echo_guard(true);
            break;
        case BTN_ACT_TOGGLE_DIM:
            ui_app_toggle_dim();
            break;
        case BTN_ACT_POWER_OFF:
            ui_app_show_shutdown_notice();
            session_mgr_stop_session();
            audio_engine_set_pa_enabled(false);
            vTaskDelay(pdMS_TO_TICKS(250));
            board_hw_shutdown();
            break;
        default:
            break;
    }
}

static void handle_button_slot(app_btn_slot_t slot) {
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    const app_btn_action_t act = prefs.btn_actions[slot];
    ESP_LOGI(TAG, "%s -> %s", app_prefs_btn_slot_name(slot), app_prefs_btn_action_name(act));
    if (act != BTN_ACT_TOGGLE_DIM && act != BTN_ACT_NONE && act != BTN_ACT_POWER_OFF) {
        ui_app_note_user_activity();
    }
    dispatch_button_action(act);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3-Touch-AMOLED-1.8 V2 Voice Agent (ESP-IDF) ===");
    ESP_LOGI(TAG, "PSRAM: %u MB | Free heap: %lu KB",
             (unsigned)(esp_psram_get_size() / (1024U * 1024U)),
             (unsigned long)(esp_get_free_heap_size() / 1024U));

    ESP_ERROR_CHECK(board_hw_init());
    ESP_ERROR_CHECK(app_prefs_init());
    ESP_ERROR_CHECK(ui_app_init());
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_LOGI(TAG, "Post-init internal DMA heap: free=%u B, largest=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    uint32_t tick_count = 0;
    board_pmu_telemetry_t last_pmu = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ++tick_count;

        const board_boot_btn_event_t boot_ev = board_hw_poll_boot_button();
        if (boot_ev != BOARD_BOOT_BTN_NONE) {
            handle_button_slot((boot_ev == BOARD_BOOT_BTN_SHORT) ? BTN_SLOT_BOOT_SHORT : BTN_SLOT_BOOT_LONG);
        }

        if ((tick_count % 2U) == 0U) {
            board_pwr_btn_event_t pwr_ev = BOARD_PWR_BTN_NONE;
            board_hw_poll_pmu(&last_pmu, &pwr_ev);

            if ((tick_count % 20U) == 0U) {
                ui_app_update_telemetry(
                    wifi_mgr_is_connected(),
                    wifi_mgr_get_rssi(),
                    session_mgr_get_rtt_ms(),
                    &last_pmu);
            }

            if (pwr_ev != BOARD_PWR_BTN_NONE) {
                handle_button_slot((pwr_ev == BOARD_PWR_BTN_SHORT) ? BTN_SLOT_PWR_SHORT : BTN_SLOT_PWR_LONG);
            }
        }
    }
}
