#include "ui_app.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "audio_engine.h"
#include "board_hw.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "ui_theme.h"

static const char *TAG = "ui_app";

#define CAPTION_MAX_LEN 240

typedef struct {
    bool                  dirty_state;
    bool                  dirty_agent;
    bool                  dirty_caption;
    bool                  dirty_mute;
    bool                  dirty_telem;
    bool                  dirty_confirm;
    bool                  dirty_shutdown;

    app_state_t           state;
    char                  status_text[80];
    char                  agent_name[32];
    char                  agent_role[48];
    ui_orb_config_t       orb_cfg;
    char                  caption[CAPTION_MAX_LEN];
    bool                  muted;

    bool                  wifi_ok;
    int                   rssi;
    uint32_t              rtt_ms;
    board_pmu_telemetry_t pmu;

    bool                  confirm_pending;
    char                  confirm_call_id[64];
    char                  confirm_name[48];
    char                  confirm_summary[128];
} ui_snapshot_t;

static SemaphoreHandle_t s_snap_mux = NULL;
static ui_snapshot_t     s_snap;

static bool    s_dimmed           = false;
static int64_t s_last_activity_us = 0;

/* LVGL widgets */
static lv_obj_t *s_wifi_dot     = NULL;
static lv_obj_t *s_net_lbl      = NULL;
static lv_obj_t *s_batt_lbl     = NULL;
static lv_obj_t *s_orb_btn      = NULL;
static lv_obj_t *s_orb_ring2    = NULL;
static lv_obj_t *s_orb_ring1    = NULL;
static lv_obj_t *s_orb_arc      = NULL;
static lv_obj_t *s_orb_core     = NULL;
static lv_obj_t *s_name_lbl     = NULL;
static lv_obj_t *s_role_lbl     = NULL;
static lv_obj_t *s_state_pill   = NULL;
static lv_obj_t *s_state_lbl    = NULL;
static lv_obj_t *s_caption_lbl  = NULL;
static lv_obj_t *s_mute_btn     = NULL;
static lv_obj_t *s_mute_lbl     = NULL;
static lv_obj_t *s_action_btn   = NULL;
static lv_obj_t *s_action_lbl   = NULL;

/* Modals */
static lv_obj_t *s_custom_modal = NULL;
static lv_obj_t *s_swatch_btns[6] = {NULL};
static lv_obj_t *s_style_btns[ORB_STYLE_COUNT] = {NULL};
static lv_obj_t *s_confirm_modal = NULL;
static lv_obj_t *s_confirm_title = NULL;
static lv_obj_t *s_confirm_body  = NULL;
static lv_obj_t *s_shutdown_lbl  = NULL;

static void wake_if_dimmed(void) {
    s_last_activity_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        board_hw_set_brightness(85);
    }
}

static uint32_t state_color_rgb(app_state_t st, uint32_t accent_rgb) {
    switch (st) {
        case APP_STATE_BOOTING:      return 0x94A3B8;
        case APP_STATE_WIFI_WAIT:    return 0xF59E0B;
        case APP_STATE_READY:        return accent_rgb;
        case APP_STATE_CONNECTING:   return 0xEAB308;
        case APP_STATE_LISTENING:    return accent_rgb;
        case APP_STATE_THINKING:     return 0xEAB308;
        case APP_STATE_SPEAKING:     return 0x10B981;
        case APP_STATE_INTERRUPTED:  return 0xFFFFFF;
        case APP_STATE_TOOL_CALLING: return 0x8B5CF6;
        case APP_STATE_ERROR:
        case APP_STATE_RECONNECTING: return 0xEF4444;
        default:                     return accent_rgb;
    }
}

static const char *state_label_str(app_state_t st) {
    switch (st) {
        case APP_STATE_BOOTING:      return "BOOTING";
        case APP_STATE_WIFI_WAIT:    return "WIFI WAIT";
        case APP_STATE_READY:        return "READY";
        case APP_STATE_CONNECTING:   return "CONNECTING";
        case APP_STATE_LISTENING:    return "LISTENING";
        case APP_STATE_THINKING:     return "THINKING";
        case APP_STATE_SPEAKING:     return "SPEAKING";
        case APP_STATE_INTERRUPTED:  return "INTERRUPTED";
        case APP_STATE_TOOL_CALLING: return "TOOL CALL";
        case APP_STATE_ERROR:        return "ERROR";
        case APP_STATE_RECONNECTING: return "RECONNECT";
        default:                     return "READY";
    }
}

static void refresh_customizer_selection(const ui_orb_config_t *cfg) {
    size_t count = 0;
    const ui_orb_preset_t *presets = ui_theme_presets(&count);
    for (size_t i = 0; i < count && i < 6; ++i) {
        if (!s_swatch_btns[i]) continue;
        const bool selected = (presets[i].rgb == cfg->accent_rgb);
        lv_obj_set_style_border_width(s_swatch_btns[i], selected ? 3 : 1, 0);
        lv_obj_set_style_border_color(
            s_swatch_btns[i],
            lv_color_hex(selected ? 0xFFFFFF : 0x334155),
            0);
    }
    for (int i = 0; i < ORB_STYLE_COUNT; ++i) {
        if (!s_style_btns[i]) continue;
        const bool selected = ((int)cfg->style == i);
        lv_obj_set_style_bg_color(
            s_style_btns[i],
            lv_color_hex(selected ? cfg->accent_rgb : 0x1E293B),
            0);
        lv_obj_set_style_text_color(
            s_style_btns[i],
            lv_color_hex(selected ? 0x000000 : 0xE2E8F0),
            0);
    }
}

static void on_screen_any_touch(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
}

static void on_center_gesture(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        session_mgr_switch_agent("next");
    } else if (dir == LV_DIR_RIGHT) {
        session_mgr_switch_agent("prev");
    }
}

static void on_orb_clicked(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    const app_state_t st = session_mgr_get_state();
    ESP_LOGI(TAG, "UI orb clicked (state=%d)", (int)st);
    if (st == APP_STATE_READY || st == APP_STATE_ERROR) {
        session_mgr_start_session();
    } else if (st == APP_STATE_SPEAKING || st == APP_STATE_THINKING) {
        session_mgr_force_interrupt();
    }
}

static void on_open_customizer(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    if (!s_custom_modal) return;
    ui_orb_config_t cfg;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    cfg = s_snap.orb_cfg;
    xSemaphoreGive(s_snap_mux);
    refresh_customizer_selection(&cfg);
    lv_obj_set_hidden(s_custom_modal, false);
}

static void on_swatch_clicked(lv_event_t *e) {
    wake_if_dimmed();
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    size_t count = 0;
    const ui_orb_preset_t *presets = ui_theme_presets(&count);
    if (idx >= count) return;

    ui_orb_config_t cfg;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.orb_cfg.accent_rgb = presets[idx].rgb;
    s_snap.dirty_state        = true;
    cfg                       = s_snap.orb_cfg;
    xSemaphoreGive(s_snap_mux);

    ui_theme_save(&cfg);
    refresh_customizer_selection(&cfg);
}

static void on_style_clicked(lv_event_t *e) {
    wake_if_dimmed();
    const uintptr_t st_idx = (uintptr_t)lv_event_get_user_data(e);
    if (st_idx >= ORB_STYLE_COUNT) return;

    ui_orb_config_t cfg;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.orb_cfg.style = (ui_orb_style_t)st_idx;
    s_snap.dirty_state   = true;
    cfg                  = s_snap.orb_cfg;
    xSemaphoreGive(s_snap_mux);

    ui_theme_save(&cfg);
    refresh_customizer_selection(&cfg);
}

static void on_customizer_done(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    if (s_custom_modal) {
        lv_obj_set_hidden(s_custom_modal, true);
    }
}

static void on_mute_clicked(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    ESP_LOGI(TAG, "UI mute button clicked");
    session_mgr_toggle_mute();
}

static void on_action_clicked(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    ESP_LOGI(TAG, "UI action button clicked");
    session_mgr_toggle_session();
}

static void on_confirm_choice(lv_event_t *e) {
    wake_if_dimmed();
    const bool approved = ((uintptr_t)lv_event_get_user_data(e)) != 0;
    char call_id[64] = {0};
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    snprintf(call_id, sizeof(call_id), "%s", s_snap.confirm_call_id);
    s_snap.confirm_pending = false;
    xSemaphoreGive(s_snap_mux);
    if (s_confirm_modal) {
        lv_obj_set_hidden(s_confirm_modal, true);
    }
    session_mgr_confirm_tool(call_id, approved);
}

static void apply_orb_visuals(app_state_t st, const ui_orb_config_t *cfg, uint8_t level, uint32_t tick) {
    const uint32_t color_hex = state_color_rgb(st, cfg->accent_rgb);
    const lv_color_t col     = lv_color_hex(color_hex);

    /* Reactive size delta from audio RMS */
    int boost = (int)(level * 28U) / 100;
    if (st == APP_STATE_READY || st == APP_STATE_BOOTING || st == APP_STATE_WIFI_WAIT) {
        /* Gentle idle breathing */
        const int phase = (int)((tick / 2U) % 24U);
        const int tri   = (phase < 12) ? phase : (24 - phase);
        boost = tri / 2;
    }

    const int core_sz  = 64 + (boost / 2);
    const int ring1_sz = 92 + boost;
    const int ring2_sz = 122 + (boost * 5) / 4;

    lv_obj_set_size(s_orb_core, core_sz, core_sz);
    lv_obj_set_style_bg_color(s_orb_core, col, 0);

    lv_obj_set_size(s_orb_ring1, ring1_sz, ring1_sz);
    lv_obj_set_style_border_color(s_orb_ring1, col, 0);

    lv_obj_set_size(s_orb_ring2, ring2_sz, ring2_sz);
    lv_obj_set_style_border_color(s_orb_ring2, col, 0);

    lv_obj_set_style_arc_color(s_orb_arc, col, LV_PART_INDICATOR);

    if (st == APP_STATE_THINKING || st == APP_STATE_CONNECTING || st == APP_STATE_TOOL_CALLING) {
        lv_obj_set_hidden(s_orb_arc, false);
        const int32_t start = (int32_t)((tick * 12U) % 360U);
        lv_arc_set_angles(s_orb_arc, start, (start + 110) % 360);
        lv_obj_set_style_bg_opa(s_orb_core, LV_OPA_70, 0);
        lv_obj_set_style_border_opa(s_orb_ring1, LV_OPA_40, 0);
        lv_obj_set_style_border_opa(s_orb_ring2, LV_OPA_0, 0);
    } else if (cfg->style == ORB_STYLE_RINGS) {
        lv_obj_set_hidden(s_orb_arc, true);
        lv_obj_set_style_bg_opa(s_orb_core, LV_OPA_30, 0);
        lv_obj_set_style_border_width(s_orb_core, 3, 0);
        lv_obj_set_style_border_color(s_orb_core, col, 0);
        lv_obj_set_style_border_opa(s_orb_ring1, LV_OPA_80, 0);
        lv_obj_set_style_border_opa(s_orb_ring2, (level > 12) ? LV_OPA_50 : LV_OPA_20, 0);
    } else if (cfg->style == ORB_STYLE_HALO) {
        lv_obj_set_hidden(s_orb_arc, false);
        const int32_t start = (int32_t)((tick * 5U) % 360U);
        const int32_t span  = 90 + (int32_t)(level * 2U);
        lv_arc_set_angles(s_orb_arc, start, (start + span) % 360);
        lv_obj_set_style_bg_opa(s_orb_core, LV_OPA_80, 0);
        lv_obj_set_style_border_width(s_orb_core, 0, 0);
        lv_obj_set_style_border_opa(s_orb_ring1, LV_OPA_20, 0);
        lv_obj_set_style_border_opa(s_orb_ring2, LV_OPA_0, 0);
    } else {
        /* ORB_STYLE_PULSE */
        lv_obj_set_hidden(s_orb_arc, true);
        lv_obj_set_style_bg_opa(s_orb_core, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_orb_core, 0, 0);
        lv_obj_set_style_border_opa(s_orb_ring1, LV_OPA_60, 0);
        lv_obj_set_style_border_opa(s_orb_ring2, LV_OPA_20, 0);
    }
}

static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    static uint32_t s_tick = 0;
    ++s_tick;

    ui_snapshot_t local;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    local = s_snap;
    s_snap.dirty_state    = false;
    s_snap.dirty_agent    = false;
    s_snap.dirty_caption  = false;
    s_snap.dirty_mute     = false;
    s_snap.dirty_telem    = false;
    s_snap.dirty_confirm  = false;
    s_snap.dirty_shutdown = false;
    xSemaphoreGive(s_snap_mux);

    if (local.dirty_shutdown && s_shutdown_lbl) {
        lv_obj_set_hidden(s_shutdown_lbl, false);
    }

    if (local.dirty_state) {
        const uint32_t rgb = state_color_rgb(local.state, local.orb_cfg.accent_rgb);
        lv_label_set_text(s_state_lbl, state_label_str(local.state));
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(rgb), 0);
        lv_obj_set_style_border_color(s_state_pill, lv_color_hex(rgb), 0);

        if (local.state == APP_STATE_READY || local.state == APP_STATE_WIFI_WAIT || local.state == APP_STATE_BOOTING) {
            lv_label_set_text(s_action_lbl, "START TALKING");
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(local.orb_cfg.accent_rgb), 0);
            lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0x000000), 0);
        } else if (local.state == APP_STATE_ERROR) {
            lv_label_set_text(s_action_lbl, "RETRY");
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(0xEF4444), 0);
            lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_label_set_text(s_action_lbl, "END SESSION");
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(0x7F1D1D), 0);
            lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xFEE2E2), 0);
        }

        if (local.caption[0] == '\0' && local.status_text[0] != '\0') {
            lv_label_set_text(s_caption_lbl, local.status_text);
        }
    }

    if (local.dirty_agent) {
        lv_label_set_text(s_name_lbl, local.agent_name);
        lv_label_set_text(s_role_lbl, local.agent_role);
    }

    if (local.dirty_caption) {
        if (local.caption[0] != '\0') {
            lv_label_set_text(s_caption_lbl, local.caption);
        } else if (local.status_text[0] != '\0') {
            lv_label_set_text(s_caption_lbl, local.status_text);
        }
    }

    if (local.dirty_mute) {
        if (local.muted) {
            lv_label_set_text(s_mute_lbl, LV_SYMBOL_MUTE " MUTED");
            lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(0x991B1B), 0);
            lv_obj_set_style_text_color(s_mute_lbl, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_label_set_text(s_mute_lbl, LV_SYMBOL_AUDIO " MUTE");
            lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(0x1E293B), 0);
            lv_obj_set_style_text_color(s_mute_lbl, lv_color_hex(0xE2E8F0), 0);
        }
    }

    if (local.dirty_telem) {
        lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(local.wifi_ok ? 0x10B981 : 0xEF4444), 0);
        if (!local.wifi_ok) {
            lv_label_set_text(s_net_lbl, "Wi-Fi...");
        } else if (local.rtt_ms > 0) {
            lv_label_set_text_fmt(s_net_lbl, "LAN %lums", (unsigned long)local.rtt_ms);
        } else {
            lv_label_set_text_fmt(s_net_lbl, "%ddBm", local.rssi);
        }

        if (local.pmu.online && local.pmu.batt_present && local.pmu.batt_pct >= 0) {
            lv_label_set_text_fmt(s_batt_lbl, "%d%%%s", local.pmu.batt_pct, local.pmu.charging ? " " LV_SYMBOL_CHARGE : "");
            lv_obj_set_style_text_color(
                s_batt_lbl,
                lv_color_hex(local.pmu.batt_pct <= 20 ? 0xEF4444 : 0x10B981),
                0);
        } else if (local.pmu.online) {
            lv_label_set_text(s_batt_lbl, local.pmu.vbus_present ? "USB-C" : "SYS");
            lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(0x94A3B8), 0);
        } else {
            lv_label_set_text(s_batt_lbl, "USB");
            lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(0x94A3B8), 0);
        }
    }

    if (local.dirty_confirm && local.confirm_pending && s_confirm_modal) {
        lv_label_set_text_fmt(s_confirm_title, "Allow: %s", local.confirm_name);
        lv_label_set_text(s_confirm_body, local.confirm_summary);
        lv_obj_set_hidden(s_confirm_modal, false);
    }

    uint8_t mic_rms = 0, spk_rms = 0;
    audio_engine_get_levels(&mic_rms, &spk_rms);
    const uint8_t level = (local.state == APP_STATE_SPEAKING) ? spk_rms : mic_rms;
    apply_orb_visuals(local.state, &local.orb_cfg, level, s_tick);

    /* Idle auto-dim after APP_IDLE_DIM_SEC in READY state */
    if (APP_IDLE_DIM_SEC > 0 && !s_dimmed && local.state == APP_STATE_READY) {
        const int64_t idle_sec = (esp_timer_get_time() - s_last_activity_us) / 1000000LL;
        if (idle_sec >= APP_IDLE_DIM_SEC) {
            s_dimmed = true;
            board_hw_set_brightness(15);
        }
    }
}

static void build_customizer_modal(lv_obj_t *parent) {
    s_custom_modal = lv_obj_create(parent);
    lv_obj_set_size(s_custom_modal, 336, 300);
    lv_obj_center(s_custom_modal);
    lv_obj_set_style_bg_color(s_custom_modal, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_color(s_custom_modal, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_custom_modal, 1, 0);
    lv_obj_set_style_radius(s_custom_modal, 18, 0);
    lv_obj_set_style_pad_all(s_custom_modal, 16, 0);
    lv_obj_set_scrollable(s_custom_modal, false);
    lv_obj_set_hidden(s_custom_modal, true);

    lv_obj_t *ttl = lv_label_create(s_custom_modal);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(ttl, "Customize Orb");
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 0);

    size_t count = 0;
    const ui_orb_preset_t *presets = ui_theme_presets(&count);
    lv_obj_t *sw_row = lv_obj_create(s_custom_modal);
    lv_obj_set_size(sw_row, lv_pct(100), 60);
    lv_obj_align(sw_row, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_opa(sw_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sw_row, 0, 0);
    lv_obj_set_style_pad_all(sw_row, 0, 0);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < count && i < 6; ++i) {
        lv_obj_t *sw = lv_btn_create(sw_row);
        lv_obj_set_size(sw, 42, 42);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(presets[i].rgb), 0);
        lv_obj_add_event_cb(sw, on_swatch_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_swatch_btns[i] = sw;
    }

    lv_obj_t *st_row = lv_obj_create(s_custom_modal);
    lv_obj_set_size(st_row, lv_pct(100), 52);
    lv_obj_align(st_row, LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_set_style_bg_opa(st_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st_row, 0, 0);
    lv_obj_set_style_pad_all(st_row, 0, 0);
    lv_obj_set_style_pad_column(st_row, 8, 0);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);

    for (int i = 0; i < ORB_STYLE_COUNT; ++i) {
        lv_obj_t *b = lv_btn_create(st_row);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_add_event_cb(b, on_style_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, ui_theme_style_name((ui_orb_style_t)i));
        lv_obj_center(lbl);
        s_style_btns[i] = b;
    }

    lv_obj_t *done_btn = lv_btn_create(s_custom_modal);
    lv_obj_set_size(done_btn, 140, 44);
    lv_obj_align(done_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(done_btn, 12, 0);
    lv_obj_set_style_bg_color(done_btn, lv_color_hex(0x334155), 0);
    lv_obj_add_event_cb(done_btn, on_customizer_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_lbl = lv_label_create(done_btn);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_center(done_lbl);
}

static void build_confirm_modal(lv_obj_t *parent) {
    s_confirm_modal = lv_obj_create(parent);
    lv_obj_set_size(s_confirm_modal, 336, 220);
    lv_obj_center(s_confirm_modal);
    lv_obj_set_style_bg_color(s_confirm_modal, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_color(s_confirm_modal, lv_color_hex(0x8B5CF6), 0);
    lv_obj_set_style_border_width(s_confirm_modal, 2, 0);
    lv_obj_set_style_radius(s_confirm_modal, 18, 0);
    lv_obj_set_style_pad_all(s_confirm_modal, 16, 0);
    lv_obj_set_scrollable(s_confirm_modal, false);
    lv_obj_set_hidden(s_confirm_modal, true);

    s_confirm_title = lv_label_create(s_confirm_modal);
    lv_obj_set_style_text_font(s_confirm_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_confirm_title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(s_confirm_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_confirm_body = lv_label_create(s_confirm_modal);
    lv_obj_set_width(s_confirm_body, lv_pct(100));
    lv_label_set_long_mode(s_confirm_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_confirm_body, lv_color_hex(0xCBD5E1), 0);
    lv_obj_align(s_confirm_body, LV_ALIGN_TOP_LEFT, 0, 34);

    lv_obj_t *deny_btn = lv_btn_create(s_confirm_modal);
    lv_obj_set_size(deny_btn, 138, 46);
    lv_obj_align(deny_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(deny_btn, lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_radius(deny_btn, 12, 0);
    lv_obj_add_event_cb(deny_btn, on_confirm_choice, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *dl = lv_label_create(deny_btn);
    lv_label_set_text(dl, "Deny");
    lv_obj_center(dl);

    lv_obj_t *allow_btn = lv_btn_create(s_confirm_modal);
    lv_obj_set_size(allow_btn, 138, 46);
    lv_obj_align(allow_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(allow_btn, lv_color_hex(0x065F46), 0);
    lv_obj_set_style_radius(allow_btn, 12, 0);
    lv_obj_add_event_cb(allow_btn, on_confirm_choice, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *al = lv_label_create(allow_btn);
    lv_label_set_text(al, "Allow");
    lv_obj_center(al);
}

esp_err_t ui_app_init(void) {
    s_snap_mux = xSemaphoreCreateMutex();
    if (!s_snap_mux) return ESP_ERR_NO_MEM;

    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = APP_STATE_BOOTING;
    snprintf(s_snap.agent_name, sizeof(s_snap.agent_name), "%s", APP_AGENT_NAME);
    snprintf(s_snap.agent_role, sizeof(s_snap.agent_role), "%s", APP_AGENT_ROLE);
    snprintf(s_snap.status_text, sizeof(s_snap.status_text), "Initializing hardware...");
    ui_theme_load(&s_snap.orb_cfg);
    s_snap.dirty_state = true;
    s_snap.dirty_agent = true;
    s_snap.dirty_telem = true;
    s_last_activity_us = esp_timer_get_time();

    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "Failed locking LVGL display");
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_scrollable(scr, false);
    lv_obj_add_event_cb(scr, on_screen_any_touch, LV_EVENT_PRESSED, NULL);

    /* Top Status Bar (y = 0..38) */
    lv_obj_t *top_bar = lv_obj_create(scr);
    lv_obj_set_size(top_bar, lv_pct(100), 38);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x0B0E14), 0);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_radius(top_bar, 12, 0);
    lv_obj_set_style_pad_hor(top_bar, 10, 0);
    lv_obj_set_scrollable(top_bar, false);

    s_wifi_dot = lv_obj_create(top_bar);
    lv_obj_set_size(s_wifi_dot, 8, 8);
    lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_wifi_dot, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(0xEF4444), 0);
    lv_obj_align(s_wifi_dot, LV_ALIGN_LEFT_MID, 0, 0);

    s_net_lbl = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_net_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_net_lbl, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_net_lbl, "Wi-Fi...");
    lv_obj_align(s_net_lbl, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *orb_cfg_btn = lv_btn_create(top_bar);
    lv_obj_set_size(orb_cfg_btn, 66, 26);
    lv_obj_align(orb_cfg_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(orb_cfg_btn, 10, 0);
    lv_obj_set_style_bg_color(orb_cfg_btn, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(orb_cfg_btn, on_open_customizer, LV_EVENT_CLICKED, NULL);
    lv_obj_t *orb_cfg_lbl = lv_label_create(orb_cfg_btn);
    lv_obj_set_style_text_font(orb_cfg_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(orb_cfg_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(orb_cfg_lbl, LV_SYMBOL_TINT " Orb");
    lv_obj_center(orb_cfg_lbl);

    s_batt_lbl = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_batt_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_batt_lbl, "USB");
    lv_obj_align(s_batt_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Center Agent Card (y = 44..294) */
    lv_obj_t *center_card = lv_obj_create(scr);
    lv_obj_set_size(center_card, lv_pct(100), 248);
    lv_obj_align(center_card, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(center_card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(center_card, 0, 0);
    lv_obj_set_style_pad_all(center_card, 0, 0);
    lv_obj_set_scrollable(center_card, false);
    lv_obj_add_event_cb(center_card, on_center_gesture, LV_EVENT_GESTURE, NULL);

    /* Orb Interactive Hit Area (144x144) */
    s_orb_btn = lv_obj_create(center_card);
    lv_obj_set_size(s_orb_btn, 144, 144);
    lv_obj_align(s_orb_btn, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_opa(s_orb_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_btn, 0, 0);
    lv_obj_set_style_pad_all(s_orb_btn, 0, 0);
    lv_obj_set_scrollable(s_orb_btn, false);
    lv_obj_set_clickable(s_orb_btn, true);
    lv_obj_add_event_cb(s_orb_btn, on_orb_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_orb_btn, on_open_customizer, LV_EVENT_LONG_PRESSED, NULL);

    s_orb_ring2 = lv_obj_create(s_orb_btn);
    lv_obj_set_size(s_orb_ring2, 124, 124);
    lv_obj_center(s_orb_ring2);
    lv_obj_set_style_radius(s_orb_ring2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_orb_ring2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_ring2, 2, 0);
    lv_obj_set_clickable(s_orb_ring2, false);

    s_orb_ring1 = lv_obj_create(s_orb_btn);
    lv_obj_set_size(s_orb_ring1, 94, 94);
    lv_obj_center(s_orb_ring1);
    lv_obj_set_style_radius(s_orb_ring1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_orb_ring1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_ring1, 3, 0);
    lv_obj_set_clickable(s_orb_ring1, false);

    s_orb_arc = lv_arc_create(s_orb_btn);
    lv_obj_set_size(s_orb_arc, 118, 118);
    lv_obj_center(s_orb_arc);
    lv_arc_set_bg_angles(s_orb_arc, 0, 360);
    lv_arc_set_angles(s_orb_arc, 0, 120);
    lv_obj_set_style_arc_width(s_orb_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_orb_arc, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_orb_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_orb_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_clickable(s_orb_arc, false);
    lv_obj_set_hidden(s_orb_arc, true);

    s_orb_core = lv_obj_create(s_orb_btn);
    lv_obj_set_size(s_orb_core, 64, 64);
    lv_obj_center(s_orb_core);
    lv_obj_set_style_radius(s_orb_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_orb_core, 0, 0);
    lv_obj_set_clickable(s_orb_core, false);

    s_name_lbl = lv_label_create(center_card);
    lv_obj_set_style_text_font(s_name_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_name_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(s_name_lbl, APP_AGENT_NAME);
    lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, 152);

    s_role_lbl = lv_label_create(center_card);
    lv_obj_set_style_text_font(s_role_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_role_lbl, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_role_lbl, APP_AGENT_ROLE);
    lv_obj_align(s_role_lbl, LV_ALIGN_TOP_MID, 0, 184);

    s_state_pill = lv_obj_create(center_card);
    lv_obj_set_size(s_state_pill, 164, 30);
    lv_obj_align(s_state_pill, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(s_state_pill, 15, 0);
    lv_obj_set_style_bg_color(s_state_pill, lv_color_hex(0x0B0E14), 0);
    lv_obj_set_style_border_width(s_state_pill, 1, 0);
    lv_obj_set_scrollable(s_state_pill, false);

    s_state_lbl = lv_label_create(s_state_pill);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_state_lbl, "BOOTING");
    lv_obj_center(s_state_lbl);

    /* Live Caption Box (y = 298..366) */
    lv_obj_t *cap_box = lv_obj_create(scr);
    lv_obj_set_size(cap_box, lv_pct(100), 68);
    lv_obj_align(cap_box, LV_ALIGN_TOP_MID, 0, 298);
    lv_obj_set_style_bg_color(cap_box, lv_color_hex(0x0B0E14), 0);
    lv_obj_set_style_border_color(cap_box, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(cap_box, 1, 0);
    lv_obj_set_style_radius(cap_box, 12, 0);
    lv_obj_set_style_pad_all(cap_box, 8, 0);
    lv_obj_set_scrollable(cap_box, false);

    s_caption_lbl = lv_label_create(cap_box);
    lv_obj_set_width(s_caption_lbl, lv_pct(100));
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_caption_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_caption_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(s_caption_lbl, "Initializing hardware...");
    lv_obj_align(s_caption_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Bottom Touch Buttons (y = 374..428) */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, lv_pct(100), 52);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 10, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(btn_row, false);

    s_mute_btn = lv_btn_create(btn_row);
    lv_obj_set_height(s_mute_btn, lv_pct(100));
    lv_obj_set_flex_grow(s_mute_btn, 1);
    lv_obj_set_style_radius(s_mute_btn, 14, 0);
    lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(s_mute_btn, on_mute_clicked, LV_EVENT_CLICKED, NULL);
    s_mute_lbl = lv_label_create(s_mute_btn);
    lv_obj_set_style_text_font(s_mute_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_mute_lbl, LV_SYMBOL_AUDIO " MUTE");
    lv_obj_center(s_mute_lbl);

    s_action_btn = lv_btn_create(btn_row);
    lv_obj_set_height(s_action_btn, lv_pct(100));
    lv_obj_set_flex_grow(s_action_btn, 2);
    lv_obj_set_style_radius(s_action_btn, 14, 0);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(s_snap.orb_cfg.accent_rgb), 0);
    lv_obj_add_event_cb(s_action_btn, on_action_clicked, LV_EVENT_CLICKED, NULL);
    s_action_lbl = lv_label_create(s_action_btn);
    lv_obj_set_style_text_font(s_action_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_action_lbl, "START TALKING");
    lv_obj_center(s_action_lbl);

    build_customizer_modal(scr);
    build_confirm_modal(scr);

    s_shutdown_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_shutdown_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_shutdown_lbl, lv_color_hex(0xF59E0B), 0);
    lv_label_set_text(s_shutdown_lbl, "Powering off...");
    lv_obj_center(s_shutdown_lbl);
    lv_obj_set_hidden(s_shutdown_lbl, true);

    lv_timer_create(ui_timer_cb, 33, NULL);
    bsp_display_unlock();
    return ESP_OK;
}

void ui_app_set_state(app_state_t state, const char *status_text) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.state       = state;
    s_snap.dirty_state = true;
    if (status_text) {
        snprintf(s_snap.status_text, sizeof(s_snap.status_text), "%s", status_text);
    }
    xSemaphoreGive(s_snap_mux);
}

void ui_app_set_agent_info(
    const char *name,
    const char *role,
    uint32_t    accent_rgb,
    bool        apply_accent,
    const char *orb_style) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    if (name && name[0] != '\0') {
        snprintf(s_snap.agent_name, sizeof(s_snap.agent_name), "%s", name);
    }
    if (role && role[0] != '\0') {
        snprintf(s_snap.agent_role, sizeof(s_snap.agent_role), "%s", role);
    }
    if (apply_accent) {
        s_snap.orb_cfg.accent_rgb = ui_theme_sanitize_rgb(accent_rgb);
        s_snap.dirty_state        = true;
    }
    if (orb_style && orb_style[0] != '\0') {
        s_snap.orb_cfg.style = ui_theme_parse_style(orb_style);
        s_snap.dirty_state   = true;
    }
    s_snap.dirty_agent = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_set_caption(const char *text, bool append) {
    if (!s_snap_mux || !text) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    if (!append) {
        snprintf(s_snap.caption, sizeof(s_snap.caption), "%s", text);
    } else {
        const size_t cur_len = strlen(s_snap.caption);
        const size_t add_len = strlen(text);
        if (cur_len + add_len >= CAPTION_MAX_LEN) {
            const size_t keep = (add_len < CAPTION_MAX_LEN - 1) ? (CAPTION_MAX_LEN - 1 - add_len) : 0;
            if (keep > 0 && cur_len > keep) {
                memmove(s_snap.caption, s_snap.caption + (cur_len - keep), keep);
                s_snap.caption[keep] = '\0';
            } else {
                s_snap.caption[0] = '\0';
            }
        }
        strncat(s_snap.caption, text, CAPTION_MAX_LEN - 1 - strlen(s_snap.caption));
    }
    s_snap.dirty_caption = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_clear_caption(void) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.caption[0]    = '\0';
    s_snap.dirty_caption = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_set_muted(bool muted) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.muted      = muted;
    s_snap.dirty_mute = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_update_telemetry(
    bool                         wifi_ok,
    int                          rssi,
    uint32_t                     rtt_ms,
    const board_pmu_telemetry_t *pmu) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.wifi_ok = wifi_ok;
    s_snap.rssi    = rssi;
    s_snap.rtt_ms  = rtt_ms;
    if (pmu) {
        s_snap.pmu = *pmu;
    }
    s_snap.dirty_telem = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_show_tool_confirm(const char *call_id, const char *name, const char *summary) {
    if (!s_snap_mux) return;
    wake_if_dimmed();
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    snprintf(s_snap.confirm_call_id, sizeof(s_snap.confirm_call_id), "%s", call_id ? call_id : "");
    snprintf(s_snap.confirm_name, sizeof(s_snap.confirm_name), "%s", name ? name : "tool");
    snprintf(s_snap.confirm_summary, sizeof(s_snap.confirm_summary), "%s", summary ? summary : "Allow action?");
    s_snap.confirm_pending = true;
    s_snap.dirty_confirm   = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_note_user_activity(void) {
    wake_if_dimmed();
}

void ui_app_toggle_dim(void) {
    s_last_activity_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        board_hw_set_brightness(85);
    } else {
        s_dimmed = true;
        board_hw_set_brightness(0);
    }
}

void ui_app_show_shutdown_notice(void) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.dirty_shutdown = true;
    xSemaphoreGive(s_snap_mux);
}
