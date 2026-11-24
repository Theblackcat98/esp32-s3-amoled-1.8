#include "ui_theme.h"

#include <string.h>

#include "app_config.h"
#include "nvs.h"

#define NVS_NS_UI     "voice_ui"
#define NVS_KEY_RGB   "orb_rgb"
#define NVS_KEY_STYLE "orb_style"

/* Curated non-cyan orb palette */
static const ui_orb_preset_t kPresets[] = {
    {"Amber",   0xF59E0B},
    {"Emerald", 0x10B981},
    {"Violet",  0x8B5CF6},
    {"Coral",   0xFB7185},
    {"Gold",    0xEAB308},
    {"Crimson", 0xEF4444},
};

const ui_orb_preset_t *ui_theme_presets(size_t *out_count) {
    if (out_count) *out_count = sizeof(kPresets) / sizeof(kPresets[0]);
    return kPresets;
}

const char *ui_theme_style_name(ui_orb_style_t style) {
    switch (style) {
        case ORB_STYLE_PULSE: return "Pulse";
        case ORB_STYLE_RINGS: return "Rings";
        case ORB_STYLE_HALO:  return "Halo";
        default:              return "Pulse";
    }
}

ui_orb_style_t ui_theme_parse_style(const char *name) {
    if (!name) return ORB_STYLE_PULSE;
    if (strcmp(name, "rings") == 0 || strcmp(name, "Rings") == 0) return ORB_STYLE_RINGS;
    if (strcmp(name, "halo") == 0 || strcmp(name, "Halo") == 0) return ORB_STYLE_HALO;
    return ORB_STYLE_PULSE;
}

uint32_t ui_theme_sanitize_rgb(uint32_t rgb) {
    const uint8_t r = (uint8_t)((rgb >> 16) & 0xFFU);
    const uint8_t g = (uint8_t)((rgb >> 8) & 0xFFU);
    const uint8_t b = (uint8_t)(rgb & 0xFFU);
    /* Guard against cyan/aqua hues */
    if (r < 95 && g > 135 && b > 135) {
        return 0xF59E0B;
    }
    return rgb & 0xFFFFFFU;
}

void ui_theme_load(ui_orb_config_t *cfg) {
    if (!cfg) return;
    cfg->accent_rgb = ui_theme_sanitize_rgb((uint32_t)APP_ORB_DEFAULT_COLOR);
    cfg->style      = ORB_STYLE_PULSE;

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_UI, NVS_READONLY, &h) == ESP_OK) {
        uint32_t saved_rgb   = 0;
        uint8_t  saved_style = 0;
        if (nvs_get_u32(h, NVS_KEY_RGB, &saved_rgb) == ESP_OK) {
            cfg->accent_rgb = ui_theme_sanitize_rgb(saved_rgb);
        }
        if (nvs_get_u8(h, NVS_KEY_STYLE, &saved_style) == ESP_OK && saved_style < ORB_STYLE_COUNT) {
            cfg->style = (ui_orb_style_t)saved_style;
        }
        nvs_close(h);
    }
}

void ui_theme_save(const ui_orb_config_t *cfg) {
    if (!cfg) return;
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_UI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_RGB, ui_theme_sanitize_rgb(cfg->accent_rgb));
        nvs_set_u8(h, NVS_KEY_STYLE, (uint8_t)cfg->style);
        nvs_commit(h);
        nvs_close(h);
    }
}
