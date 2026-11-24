#include "board_pmu.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "board_pmu";

#define AXP2101_I2C_ADDR          0x34
#define AXP2101_REG_STATUS1       0x00
#define AXP2101_REG_STATUS2       0x01
#define AXP2101_REG_COMMON_CFG    0x10
#define AXP2101_REG_ADC_CTRL      0x30
#define AXP2101_REG_VBAT_H        0x34
#define AXP2101_REG_VBAT_L        0x35
#define AXP2101_REG_INTEN2        0x41
#define AXP2101_REG_INTSTS2       0x49
#define AXP2101_REG_BAT_DET_CTRL  0x68
#define AXP2101_REG_BAT_PERCENT   0xA4

#define AXP2101_PKEY_LONG_MASK    (1U << 2)
#define AXP2101_PKEY_SHORT_MASK   (1U << 3)

static i2c_master_dev_handle_t s_pmu_dev = NULL;
static bool s_pmu_online = false;

static esp_err_t pmu_read_reg(uint8_t reg, uint8_t *val) {
    if (!s_pmu_dev || !val) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_pmu_dev, &reg, 1, val, 1, 50);
}

static esp_err_t pmu_write_reg(uint8_t reg, uint8_t val) {
    if (!s_pmu_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pmu_dev, buf, sizeof(buf), 50);
}

static esp_err_t pmu_set_bits(uint8_t reg, uint8_t mask) {
    uint8_t val = 0;
    esp_err_t err = pmu_read_reg(reg, &val);
    if (err != ESP_OK) return err;
    return pmu_write_reg(reg, (uint8_t)(val | mask));
}

esp_err_t board_pmu_init(i2c_master_bus_handle_t i2c_bus) {
    if (!i2c_bus) return ESP_ERR_INVALID_ARG;

    if (i2c_master_probe(i2c_bus, AXP2101_I2C_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not detected at 0x%02X", AXP2101_I2C_ADDR);
        s_pmu_online = false;
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_pmu_dev);
    if (err != ESP_OK) {
        s_pmu_online = false;
        return err;
    }

    /* Enable battery detection + VBAT/VBUS/VSYS ADCs without modifying OTP voltage rails. */
    pmu_set_bits(AXP2101_REG_BAT_DET_CTRL, 0x01);
    pmu_set_bits(AXP2101_REG_ADC_CTRL, 0x0D);
    pmu_set_bits(AXP2101_REG_INTEN2, AXP2101_PKEY_SHORT_MASK | AXP2101_PKEY_LONG_MASK);
    pmu_write_reg(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT_MASK | AXP2101_PKEY_LONG_MASK);

    s_pmu_online = true;
    ESP_LOGI(TAG, "AXP2101 initialized (telemetry + PWR key IRQ, OTP rails untouched)");
    return ESP_OK;
}

bool board_pmu_is_online(void) {
    return s_pmu_online;
}

void board_pmu_poll(board_pmu_telemetry_t *out, board_pwr_btn_event_t *btn_event) {
    if (btn_event) *btn_event = BOARD_PWR_BTN_NONE;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->batt_pct = -1;
    out->online   = s_pmu_online;
    if (!s_pmu_online) return;

    uint8_t st1 = 0, st2 = 0;
    if (pmu_read_reg(AXP2101_REG_STATUS1, &st1) != ESP_OK ||
        pmu_read_reg(AXP2101_REG_STATUS2, &st2) != ESP_OK) {
        return;
    }

    const bool vbus_good = (st1 & (1U << 5)) != 0;
    out->vbus_present    = vbus_good && ((st2 & (1U << 3)) == 0);
    out->batt_present    = (st1 & (1U << 3)) != 0;
    out->charging        = ((st2 >> 5) & 0x07U) == 0x01U;

    if (out->batt_present) {
        uint8_t vh = 0, vl = 0, soc = 0;
        if (pmu_read_reg(AXP2101_REG_VBAT_H, &vh) == ESP_OK &&
            pmu_read_reg(AXP2101_REG_VBAT_L, &vl) == ESP_OK) {
            out->batt_mv = (uint16_t)(((uint16_t)(vh & 0x1FU) << 8) | vl);
        }
        if (pmu_read_reg(AXP2101_REG_BAT_PERCENT, &soc) == ESP_OK && soc <= 100U) {
            out->batt_pct = (int)soc;
        }
    }

    if (btn_event) {
        uint8_t irq2 = 0;
        if (pmu_read_reg(AXP2101_REG_INTSTS2, &irq2) == ESP_OK) {
            const uint8_t pkey_bits = irq2 & (AXP2101_PKEY_SHORT_MASK | AXP2101_PKEY_LONG_MASK);
            if (pkey_bits) {
                pmu_write_reg(AXP2101_REG_INTSTS2, pkey_bits);
                if (pkey_bits & AXP2101_PKEY_LONG_MASK) {
                    *btn_event = BOARD_PWR_BTN_LONG;
                } else if (pkey_bits & AXP2101_PKEY_SHORT_MASK) {
                    *btn_event = BOARD_PWR_BTN_SHORT;
                }
            }
        }
    }
}

esp_err_t board_pmu_shutdown(void) {
    if (!s_pmu_online) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "Commanding AXP2101 clean software power-off");
    return pmu_set_bits(AXP2101_REG_COMMON_CFG, 0x01);
}
