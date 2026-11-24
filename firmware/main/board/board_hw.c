#include "board_hw.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "board_hw";

#define BOOT_BTN_GPIO        GPIO_NUM_0
#define BOOT_LONG_PRESS_US   (700 * 1000LL)
#define BOOT_DEBOUNCE_US     (35 * 1000LL)

static int     s_brightness_pct = 85;
static bool    s_boot_down      = false;
static bool    s_boot_long_sent = false;
static int64_t s_boot_down_us   = 0;

static void scan_i2c_bus(i2c_master_bus_handle_t bus) {
    if (!bus) return;
    char found[128] = {0};
    size_t pos      = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            if (pos + 6 < sizeof(found)) {
                pos += (size_t)snprintf(found + pos, sizeof(found) - pos, "0x%02X ", addr);
            }
        }
    }
    ESP_LOGI(TAG, "I2C scan (SDA=15, SCL=14): %s", pos ? found : "none");
}

static void reset_panel_and_touch(void) {
    esp_io_expander_handle_t io = bsp_io_expander_init();
    if (!io) {
        ESP_LOGW(TAG, "TCA9554 IO expander not found; skipping reset pulse");
        return;
    }
    const uint32_t rst_mask = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2;
    esp_io_expander_set_dir(io, rst_mask, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, rst_mask, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_io_expander_set_level(io, rst_mask, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void co5300_rounder_cb(lv_area_t *area) {
    uint16_t x1 = (area->x1 > 0) ? (uint16_t)area->x1 : 0;
    uint16_t y1 = (area->y1 > 0) ? (uint16_t)area->y1 : 0;
    uint16_t x2 = (area->x2 > 0) ? (uint16_t)area->x2 : 0;
    uint16_t y2 = (area->y2 > 0) ? (uint16_t)area->y2 : 0;
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
    if (area->x2 >= (int32_t)BSP_LCD_H_RES) area->x2 = BSP_LCD_H_RES - 1;
    if (area->y2 >= (int32_t)BSP_LCD_V_RES) area->y2 = BSP_LCD_V_RES - 1;
}

esp_err_t board_hw_init(void) {
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);

    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    scan_i2c_bus(i2c_bus);

    reset_panel_and_touch();
    board_pmu_init(i2c_bus);

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const bsp_display_config_t disp_config = {0};
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(bsp_display_new(&disp_config, &panel_handle, &io_handle));

    /* Probe touch before LVGL disp registration so bsp_touch_new sets CO5300 x_gap=0x10 */
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(bsp_touch_new(NULL, &tp));
    if (BSP_LCD_TOUCH_INT != GPIO_NUM_NC) {
        gpio_pullup_en(BSP_LCD_TOUCH_INT);
    }
    if (tp && tp->io) {
        const uint8_t dis_auto_sleep = 0xFF;
        const uint8_t irq_ctl        = 0x60;
        esp_err_t reg_err = esp_lcd_panel_io_tx_param(tp->io, 0xFE, &dis_auto_sleep, 1);
        if (reg_err != ESP_OK) {
            ESP_LOGW(TAG, "CST820 reg 0xFE write failed: %s", esp_err_to_name(reg_err));
        }
        reg_err = esp_lcd_panel_io_tx_param(tp->io, 0xFA, &irq_ctl, 1);
        if (reg_err != ESP_OK) {
            ESP_LOGW(TAG, "CST820 reg 0xFA write failed: %s", esp_err_to_name(reg_err));
        }
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = panel_handle,
        .buffer_size   = BSP_LCD_H_RES * 24,
        .double_buffer = false,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation      = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .rounder_cb    = co5300_rounder_cb,
        .flags         = {
            .buff_dma    = true,
            .buff_spiram = false,
            .sw_rotate   = false,
            .swap_bytes  = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = tp,
    };
    if (!lvgl_port_add_touch(&touch_cfg)) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(bsp_display_brightness_init());
    board_hw_set_brightness(85);
    ESP_LOGI(TAG, "Internal DMA heap after disp: free=%u B, largest=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Waveshare V2 display + CST820 touch + LVGL 9 ready");
    return ESP_OK;
}

void board_hw_set_brightness(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_brightness_pct = pct;
    bsp_display_brightness_set(pct);
}

int board_hw_get_brightness(void) {
    return s_brightness_pct;
}

board_boot_btn_event_t board_hw_poll_boot_button(void) {
    const bool    pressed = (gpio_get_level(BOOT_BTN_GPIO) == 0);
    const int64_t now_us  = esp_timer_get_time();

    if (pressed && !s_boot_down) {
        s_boot_down      = true;
        s_boot_long_sent = false;
        s_boot_down_us   = now_us;
        return BOARD_BOOT_BTN_NONE;
    }

    if (pressed && s_boot_down && !s_boot_long_sent && (now_us - s_boot_down_us) >= BOOT_LONG_PRESS_US) {
        s_boot_long_sent = true;
        return BOARD_BOOT_BTN_LONG;
    }

    if (!pressed && s_boot_down) {
        const int64_t held_us = now_us - s_boot_down_us;
        const bool    was_long = s_boot_long_sent;
        s_boot_down      = false;
        s_boot_long_sent = false;
        if (!was_long && held_us >= BOOT_DEBOUNCE_US) {
            return BOARD_BOOT_BTN_SHORT;
        }
    }
    return BOARD_BOOT_BTN_NONE;
}

void board_hw_poll_pmu(board_pmu_telemetry_t *pmu_out, board_pwr_btn_event_t *pwr_btn_out) {
    board_pmu_poll(pmu_out, pwr_btn_out);
}

esp_err_t board_hw_shutdown(void) {
    return board_pmu_shutdown();
}
