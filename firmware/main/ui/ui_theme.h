#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORB_STYLE_PULSE = 0,
    ORB_STYLE_RINGS = 1,
    ORB_STYLE_HALO  = 2,
    ORB_STYLE_COUNT = 3,
} ui_orb_style_t;

typedef struct {
    const char *name;
    uint32_t    rgb;
} ui_orb_preset_t;

typedef struct {
    uint32_t       accent_rgb;
    ui_orb_style_t style;
} ui_orb_config_t;

const ui_orb_preset_t *ui_theme_presets(size_t *out_count);
const char            *ui_theme_style_name(ui_orb_style_t style);
ui_orb_style_t         ui_theme_parse_style(const char *name);
uint32_t               ui_theme_sanitize_rgb(uint32_t rgb);

void ui_theme_load(ui_orb_config_t *cfg);
void ui_theme_save(const ui_orb_config_t *cfg);

#ifdef __cplusplus
}
#endif
