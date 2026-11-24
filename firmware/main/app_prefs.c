#include "app_prefs.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "app_prefs";

#define NVS_NS_PREFS    "voice_prefs"
#define NVS_KEY_VOL     "spk_vol"
#define NVS_KEY_GUARD   "guard_mode"
#define NVS_KEY_SHIELD  "shield_vol"

static const app_btn_action_t kDefaultBtnActions[BTN_SLOT_COUNT] = {
    BTN_ACT_START_OR_MUTE,  /* BTN_SLOT_BOOT_SHORT */
    BTN_ACT_TOGGLE_SESSION, /* BTN_SLOT_BOOT_LONG */
    BTN_ACT_TOGGLE_DIM,     /* BTN_SLOT_PWR_SHORT */
    BTN_ACT_POWER_OFF,      /* BTN_SLOT_PWR_LONG */
};

static SemaphoreHandle_t s_mux = NULL;
static app_prefs_t       s_prefs;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void save_u8_nvs(const char *key, uint8_t val) {
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_PREFS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t app_prefs_init(void) {
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutex();
        if (!s_mux) return ESP_ERR_NO_MEM;
    }

    s_prefs.speaker_volume_pct  = clamp_int(APP_SPEAKER_VOLUME_PCT, 0, 100);
    s_prefs.echo_guard_mode     = AUDIO_GUARD_AUTO;
    s_prefs.auto_shield_vol_pct = clamp_int(APP_ECHO_SHIELD_VOL_PCT, 60, 100);
    memcpy(s_prefs.btn_actions, kDefaultBtnActions, sizeof(kDefaultBtnActions));

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_PREFS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, NVS_KEY_VOL, &val) == ESP_OK) {
            s_prefs.speaker_volume_pct = clamp_int((int)val, 0, 100);
        }
        if (nvs_get_u8(h, NVS_KEY_GUARD, &val) == ESP_OK && val < AUDIO_GUARD_MODE_COUNT) {
            s_prefs.echo_guard_mode = (audio_echo_guard_mode_t)val;
        }
        if (nvs_get_u8(h, NVS_KEY_SHIELD, &val) == ESP_OK) {
            s_prefs.auto_shield_vol_pct = clamp_int((int)val, 60, 100);
        }
        for (int i = 0; i < BTN_SLOT_COUNT; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "btn_%d", i);
            if (nvs_get_u8(h, key, &val) == ESP_OK && val < BTN_ACT_COUNT) {
                s_prefs.btn_actions[i] = (app_btn_action_t)val;
            }
        }
        nvs_close(h);
    }

    ESP_LOGI(
        TAG,
        "Prefs loaded: vol=%d%%, guard=%s (shield>=%d%%), btns=[%s | %s | %s | %s]",
        s_prefs.speaker_volume_pct,
        audio_guard_mode_name(s_prefs.echo_guard_mode),
        s_prefs.auto_shield_vol_pct,
        app_prefs_btn_action_name(s_prefs.btn_actions[BTN_SLOT_BOOT_SHORT]),
        app_prefs_btn_action_name(s_prefs.btn_actions[BTN_SLOT_BOOT_LONG]),
        app_prefs_btn_action_name(s_prefs.btn_actions[BTN_SLOT_PWR_SHORT]),
        app_prefs_btn_action_name(s_prefs.btn_actions[BTN_SLOT_PWR_LONG]));
    return ESP_OK;
}

void app_prefs_get(app_prefs_t *out) {
    if (!out) return;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_prefs;
    if (s_mux) xSemaphoreGive(s_mux);
}

int app_prefs_set_volume(int pct, bool save_nvs) {
    const int clamped = clamp_int(pct, 0, 100);
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    s_prefs.speaker_volume_pct = clamped;
    if (s_mux) xSemaphoreGive(s_mux);
    if (save_nvs) {
        save_u8_nvs(NVS_KEY_VOL, (uint8_t)clamped);
    }
    return clamped;
}

void app_prefs_set_echo_guard(audio_echo_guard_mode_t mode, int auto_shield_vol_pct) {
    const audio_echo_guard_mode_t safe_mode =
        (mode < AUDIO_GUARD_MODE_COUNT) ? mode : AUDIO_GUARD_AUTO;
    const int safe_shield = clamp_int(auto_shield_vol_pct, 60, 100);

    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    s_prefs.echo_guard_mode     = safe_mode;
    s_prefs.auto_shield_vol_pct = safe_shield;
    if (s_mux) xSemaphoreGive(s_mux);

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_PREFS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_GUARD, (uint8_t)safe_mode);
        nvs_set_u8(h, NVS_KEY_SHIELD, (uint8_t)safe_shield);
        nvs_commit(h);
        nvs_close(h);
    }
}

void app_prefs_set_btn_action(app_btn_slot_t slot, app_btn_action_t action) {
    if (slot < 0 || slot >= BTN_SLOT_COUNT || action < 0 || action >= BTN_ACT_COUNT) return;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    s_prefs.btn_actions[slot] = action;
    if (s_mux) xSemaphoreGive(s_mux);

    char key[8];
    snprintf(key, sizeof(key), "btn_%d", (int)slot);
    save_u8_nvs(key, (uint8_t)action);
}

void app_prefs_reset_btn_defaults(void) {
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(s_prefs.btn_actions, kDefaultBtnActions, sizeof(kDefaultBtnActions));
    if (s_mux) xSemaphoreGive(s_mux);

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_PREFS, NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < BTN_SLOT_COUNT; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "btn_%d", i);
            nvs_set_u8(h, key, (uint8_t)kDefaultBtnActions[i]);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *app_prefs_btn_slot_name(app_btn_slot_t slot) {
    switch (slot) {
        case BTN_SLOT_BOOT_SHORT: return "BOOT Short";
        case BTN_SLOT_BOOT_LONG:  return "BOOT Long";
        case BTN_SLOT_PWR_SHORT:  return "PWR Short";
        case BTN_SLOT_PWR_LONG:   return "PWR Long";
        default:                  return "Unknown";
    }
}

const char *app_prefs_btn_action_name(app_btn_action_t action) {
    switch (action) {
        case BTN_ACT_NONE:           return "None";
        case BTN_ACT_START_OR_MUTE:  return "Talk / Mute";
        case BTN_ACT_TOGGLE_SESSION: return "Start / Stop";
        case BTN_ACT_TOGGLE_MUTE:    return "Mute Mic";
        case BTN_ACT_INTERRUPT:      return "Interrupt";
        case BTN_ACT_VOL_UP:         return "Vol +10%";
        case BTN_ACT_VOL_DOWN:       return "Vol -10%";
        case BTN_ACT_CYCLE_GUARD:    return "Echo Guard";
        case BTN_ACT_TOGGLE_DIM:     return "Dim Screen";
        case BTN_ACT_POWER_OFF:      return "Power Off";
        default:                     return "None";
    }
}
