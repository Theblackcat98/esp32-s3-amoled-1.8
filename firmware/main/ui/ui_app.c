#include "ui_app.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_prefs.h"
#include "audio_engine.h"
#include "audio_guard.h"
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
    bool                  dirty_audio;
    bool                  dirty_toast;
    bool                  dirty_confirm;
    bool                  dirty_shutdown;

    app_state_t             state;
    char                    status_text[80];
    char                    agent_name[32];
    char                    agent_role[48];
    ui_orb_config_t         orb_cfg;
    char                    caption[CAPTION_MAX_LEN];
    bool                    muted;
    int                     vol_pct;
    audio_echo_guard_mode_t guard_mode;
    int                     shield_vol_pct;
    char                    toast_msg[48];

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
static bool    s_updating_ui      = false;
static int     s_active_tab       = 0;
static int64_t s_last_activity_us = 0;
static int64_t s_toast_hide_us    = 0;

/* LVGL widgets */
static lv_obj_t *s_wifi_dot     = NULL;
static lv_obj_t *s_net_lbl      = NULL;
static lv_obj_t *s_vol_btn      = NULL;
static lv_obj_t *s_vol_lbl      = NULL;
static lv_obj_t *s_batt_lbl     = NULL;
static lv_obj_t *s_toast_pill   = NULL;
static lv_obj_t *s_toast_lbl    = NULL;
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

/* Settings & confirm modals */
static lv_obj_t *s_settings_modal                  = NULL;
static lv_obj_t *s_tab_btns[3]                     = {NULL};
static lv_obj_t *s_tab_lbls[3]                     = {NULL};
static lv_obj_t *s_tab_panels[3]                   = {NULL};
static lv_obj_t *s_vol_val_lbl                     = NULL;
static lv_obj_t *s_vol_slider                      = NULL;
static lv_obj_t *s_guard_badge                     = NULL;
static lv_obj_t *s_guard_btns[AUDIO_GUARD_MODE_COUNT] = {NULL};
static lv_obj_t *s_guard_lbls[AUDIO_GUARD_MODE_COUNT] = {NULL};
static lv_obj_t *s_shield_row                      = NULL;
static lv_obj_t *s_shield_val_lbl                  = NULL;
static lv_obj_t *s_guard_hint_lbl                  = NULL;
static lv_obj_t *s_btn_act_lbls[BTN_SLOT_COUNT]    = {NULL};
static lv_obj_t *s_swatch_btns[6]                  = {NULL};
static lv_obj_t *s_style_btns[ORB_STYLE_COUNT]     = {NULL};
static lv_obj_t *s_confirm_modal                   = NULL;
static lv_obj_t *s_confirm_title                   = NULL;
static lv_obj_t *s_confirm_body                    = NULL;
static lv_obj_t *s_shutdown_lbl                    = NULL;

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

static bool is_shield_armed(int vol_pct, audio_echo_guard_mode_t mode, int shield_vol_pct) {
    return (mode == AUDIO_GUARD_STRICT) || (mode == AUDIO_GUARD_AUTO && vol_pct >= shield_vol_pct);
}

static void refresh_audio_panel(int vol_pct, audio_echo_guard_mode_t mode, int shield_vol_pct) {
    const bool shielded = is_shield_armed(vol_pct, mode, shield_vol_pct);

    if (s_vol_lbl) {
        const char *sym = (vol_pct == 0) ? LV_SYMBOL_MUTE
                                         : ((vol_pct < 50) ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
        lv_label_set_text_fmt(s_vol_lbl, "%s %d%%", sym, vol_pct);
        lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(shielded ? 0xF59E0B : 0xE2E8F0), 0);
    }
    if (s_vol_btn) {
        lv_obj_set_style_border_color(s_vol_btn, lv_color_hex(shielded ? 0xF59E0B : 0x334155), 0);
        lv_obj_set_style_border_width(s_vol_btn, shielded ? 1 : 0, 0);
    }
    if (s_vol_val_lbl) {
        lv_label_set_text_fmt(s_vol_val_lbl, "%d%%", vol_pct);
    }
    if (s_vol_slider) {
        s_updating_ui = true;
        lv_slider_set_value(s_vol_slider, vol_pct, LV_ANIM_OFF);
        s_updating_ui = false;
    }
    if (s_guard_badge) {
        lv_label_set_text(s_guard_badge, shielded ? "SHIELDED" : "FULL DUPLEX");
        lv_obj_set_style_text_color(s_guard_badge, lv_color_hex(shielded ? 0xF59E0B : 0x10B981), 0);
    }
    for (int i = 0; i < AUDIO_GUARD_MODE_COUNT; ++i) {
        if (!s_guard_btns[i]) continue;
        const bool sel = ((int)mode == i);
        lv_obj_set_style_bg_color(s_guard_btns[i], lv_color_hex(sel ? 0xF59E0B : 0x1E293B), 0);
        if (s_guard_lbls[i]) {
            lv_obj_set_style_text_color(s_guard_lbls[i], lv_color_hex(sel ? 0x000000 : 0xE2E8F0), 0);
        }
    }
    if (s_shield_row) {
        lv_obj_set_hidden(s_shield_row, mode != AUDIO_GUARD_AUTO);
    }
    if (s_shield_val_lbl) {
        lv_label_set_text_fmt(s_shield_val_lbl, "%d%%+", shield_vol_pct);
    }
    if (s_guard_hint_lbl) {
        const char *hint =
            (mode == AUDIO_GUARD_STRICT)
                ? "Speaker blocks mic barge-in at any volume (tap orb to interrupt)."
                : ((mode == AUDIO_GUARD_ALWAYS)
                       ? "Full-duplex barge-in at all volumes using dynamic echo gate."
                       : "Blocks mic self-trigger above threshold; dynamic gate below.");
        lv_label_set_text(s_guard_hint_lbl, hint);
    }
}

static void refresh_buttons_panel(void) {
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    for (int i = 0; i < BTN_SLOT_COUNT; ++i) {
        if (!s_btn_act_lbls[i]) continue;
        lv_label_set_text(s_btn_act_lbls[i], app_prefs_btn_action_name(prefs.btn_actions[i]));
    }
}

static void select_settings_tab(int tab_idx) {
    if (tab_idx < 0 || tab_idx >= 3) tab_idx = 0;
    s_active_tab = tab_idx;
    for (int i = 0; i < 3; ++i) {
        const bool active = (i == s_active_tab);
        if (s_tab_panels[i]) {
            lv_obj_set_hidden(s_tab_panels[i], !active);
        }
        if (s_tab_btns[i]) {
            lv_obj_set_style_bg_color(s_tab_btns[i], lv_color_hex(active ? 0xF59E0B : 0x1E293B), 0);
        }
        if (s_tab_lbls[i]) {
            lv_obj_set_style_text_color(s_tab_lbls[i], lv_color_hex(active ? 0x000000 : 0xE2E8F0), 0);
        }
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

static void open_settings_modal(int tab_idx) {
    wake_if_dimmed();
    if (!s_settings_modal) return;

    ui_orb_config_t cfg;
    int vol_pct;
    audio_echo_guard_mode_t guard_mode;
    int shield_vol_pct;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    cfg            = s_snap.orb_cfg;
    vol_pct        = s_snap.vol_pct;
    guard_mode     = s_snap.guard_mode;
    shield_vol_pct = s_snap.shield_vol_pct;
    xSemaphoreGive(s_snap_mux);

    refresh_audio_panel(vol_pct, guard_mode, shield_vol_pct);
    refresh_buttons_panel();
    refresh_customizer_selection(&cfg);
    select_settings_tab(tab_idx);
    lv_obj_set_hidden(s_settings_modal, false);
}

static void on_open_audio_settings(lv_event_t *e) {
    (void)e;
    open_settings_modal(0);
}

static void on_open_customizer(lv_event_t *e) {
    (void)e;
    open_settings_modal(2);
}

static void on_tab_clicked(lv_event_t *e) {
    wake_if_dimmed();
    select_settings_tab((int)(uintptr_t)lv_event_get_user_data(e));
}

static void on_vol_step_clicked(lv_event_t *e) {
    wake_if_dimmed();
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    session_mgr_adjust_volume(delta, false);
}

static void on_vol_slider_event(lv_event_t *e) {
    if (s_updating_ui) return;
    wake_if_dimmed();
    const lv_event_code_t code = lv_event_get_code(e);
    const int val              = (int)lv_slider_get_value(s_vol_slider);
    session_mgr_set_volume(val, (code == LV_EVENT_RELEASED), false);
}

static void on_guard_mode_clicked(lv_event_t *e) {
    wake_if_dimmed();
    const audio_echo_guard_mode_t mode =
        (audio_echo_guard_mode_t)(uintptr_t)lv_event_get_user_data(e);
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    session_mgr_set_echo_guard(mode, prefs.auto_shield_vol_pct, false);
}

static void on_shield_step_clicked(lv_event_t *e) {
    wake_if_dimmed();
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    session_mgr_set_echo_guard(AUDIO_GUARD_AUTO, prefs.auto_shield_vol_pct + delta, false);
}

static void on_btn_row_clicked(lv_event_t *e) {
    wake_if_dimmed();
    const app_btn_slot_t slot = (app_btn_slot_t)(uintptr_t)lv_event_get_user_data(e);
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    const app_btn_action_t next =
        (app_btn_action_t)(((int)prefs.btn_actions[slot] + 1) % BTN_ACT_COUNT);
    app_prefs_set_btn_action(slot, next);
    refresh_buttons_panel();
}

static void on_btn_reset_clicked(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    app_prefs_reset_btn_defaults();
    refresh_buttons_panel();
    ui_app_show_toast("Buttons Reset");
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

static void on_settings_done(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    if (s_settings_modal) {
        lv_obj_set_hidden(s_settings_modal, true);
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
    s_snap.dirty_audio    = false;
    s_snap.dirty_toast    = false;
    s_snap.dirty_confirm  = false;
    s_snap.dirty_shutdown = false;
    xSemaphoreGive(s_snap_mux);

    if (local.dirty_shutdown && s_shutdown_lbl) {
        lv_obj_set_hidden(s_shutdown_lbl, false);
    }

    if (local.dirty_audio) {
        refresh_audio_panel(local.vol_pct, local.guard_mode, local.shield_vol_pct);
    }

    if (local.dirty_toast && s_toast_pill && s_toast_lbl) {
        lv_label_set_text(s_toast_lbl, local.toast_msg);
        lv_obj_set_hidden(s_toast_pill, false);
        lv_obj_move_foreground(s_toast_pill);
        s_toast_hide_us = esp_timer_get_time() + 1600000LL;
    }

    if (s_toast_pill && s_toast_hide_us > 0 && esp_timer_get_time() >= s_toast_hide_us) {
        s_toast_hide_us = 0;
        lv_obj_set_hidden(s_toast_pill, true);
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

static lv_obj_t *create_step_btn(
    lv_obj_t     *parent,
    int           w,
    int           h,
    const char   *txt,
    lv_event_cb_t cb,
    int           delta) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)delta);
    lv_obj_t *lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    return b;
}

static lv_obj_t *create_tab_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scrollable(p, false);
    lv_obj_set_hidden(p, true);
    return p;
}

static void build_audio_tab(lv_obj_t *panel) {
    lv_obj_t *vol_hdr = lv_label_create(panel);
    lv_obj_set_style_text_font(vol_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vol_hdr, lv_color_hex(0xCBD5E1), 0);
    lv_label_set_text(vol_hdr, "Speaker Volume");
    lv_obj_align(vol_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    s_vol_val_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(s_vol_val_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_vol_val_lbl, lv_color_hex(0xF59E0B), 0);
    lv_label_set_text(s_vol_val_lbl, "75%");
    lv_obj_align(s_vol_val_lbl, LV_ALIGN_TOP_RIGHT, 0, -1);

    lv_obj_t *vol_row = lv_obj_create(panel);
    lv_obj_set_size(vol_row, lv_pct(100), 42);
    lv_obj_align(vol_row, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_pad_column(vol_row, 10, 0);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(vol_row, false);

    create_step_btn(vol_row, 48, 42, "-", on_vol_step_clicked, -5);

    s_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(s_vol_slider, 1);
    lv_obj_set_height(s_vol_slider, 14);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0xF8FAFC), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, on_vol_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, on_vol_slider_event, LV_EVENT_RELEASED, NULL);

    create_step_btn(vol_row, 48, 42, "+", on_vol_step_clicked, 5);

    lv_obj_t *guard_hdr = lv_label_create(panel);
    lv_obj_set_style_text_font(guard_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(guard_hdr, lv_color_hex(0xCBD5E1), 0);
    lv_label_set_text(guard_hdr, "High-Vol Guard");
    lv_obj_align(guard_hdr, LV_ALIGN_TOP_LEFT, 0, 82);

    s_guard_badge = lv_label_create(panel);
    lv_obj_set_style_text_font(s_guard_badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_guard_badge, lv_color_hex(0x10B981), 0);
    lv_label_set_text(s_guard_badge, "FULL DUPLEX");
    lv_obj_align(s_guard_badge, LV_ALIGN_TOP_RIGHT, 0, 84);

    lv_obj_t *mode_row = lv_obj_create(panel);
    lv_obj_set_size(mode_row, lv_pct(100), 40);
    lv_obj_align(mode_row, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, 0, 0);
    lv_obj_set_style_pad_column(mode_row, 8, 0);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(mode_row, false);

    for (int i = 0; i < AUDIO_GUARD_MODE_COUNT; ++i) {
        lv_obj_t *b = lv_btn_create(mode_row);
        lv_obj_set_height(b, 40);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_guard_mode_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_label_set_text(lbl, audio_guard_mode_name((audio_echo_guard_mode_t)i));
        lv_obj_center(lbl);
        s_guard_btns[i] = b;
        s_guard_lbls[i] = lbl;
    }

    s_shield_row = lv_obj_create(panel);
    lv_obj_set_size(s_shield_row, lv_pct(100), 38);
    lv_obj_align(s_shield_row, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_set_style_bg_opa(s_shield_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_shield_row, 0, 0);
    lv_obj_set_style_pad_all(s_shield_row, 0, 0);
    lv_obj_set_scrollable(s_shield_row, false);

    lv_obj_t *sh_lbl = lv_label_create(s_shield_row);
    lv_obj_set_style_text_font(sh_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sh_lbl, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(sh_lbl, "Shield at:");
    lv_obj_align(sh_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sh_plus = create_step_btn(s_shield_row, 42, 34, "+", on_shield_step_clicked, 5);
    lv_obj_align(sh_plus, LV_ALIGN_RIGHT_MID, 0, 0);

    s_shield_val_lbl = lv_label_create(s_shield_row);
    lv_obj_set_style_text_font(s_shield_val_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_shield_val_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(s_shield_val_lbl, "80%+");
    lv_obj_align(s_shield_val_lbl, LV_ALIGN_RIGHT_MID, -56, 0);

    lv_obj_t *sh_minus = create_step_btn(s_shield_row, 42, 34, "-", on_shield_step_clicked, -5);
    lv_obj_align(sh_minus, LV_ALIGN_RIGHT_MID, -110, 0);

    s_guard_hint_lbl = lv_label_create(panel);
    lv_obj_set_width(s_guard_hint_lbl, lv_pct(100));
    lv_label_set_long_mode(s_guard_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_guard_hint_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_guard_hint_lbl, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(s_guard_hint_lbl, LV_ALIGN_TOP_LEFT, 0, 202);
}

static void build_buttons_tab(lv_obj_t *panel) {
    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(hint, "Tap a row to cycle action");
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 0);

    for (int i = 0; i < BTN_SLOT_COUNT; ++i) {
        lv_obj_t *row = lv_btn_create(panel);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 22 + i * 50);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x334155), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_add_event_cb(row, on_btn_row_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *slot_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(slot_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(slot_lbl, lv_color_hex(0x94A3B8), 0);
        lv_label_set_text(slot_lbl, app_prefs_btn_slot_name((app_btn_slot_t)i));
        lv_obj_align(slot_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *act_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(act_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(act_lbl, lv_color_hex(0xF59E0B), 0);
        lv_label_set_text(act_lbl, "Talk / Mute");
        lv_obj_align(act_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        s_btn_act_lbls[i] = act_lbl;
    }

    lv_obj_t *rst_btn = lv_btn_create(panel);
    lv_obj_set_size(rst_btn, 150, 32);
    lv_obj_align(rst_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(rst_btn, 10, 0);
    lv_obj_set_style_bg_color(rst_btn, lv_color_hex(0x334155), 0);
    lv_obj_add_event_cb(rst_btn, on_btn_reset_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rst_lbl = lv_label_create(rst_btn);
    lv_obj_set_style_text_font(rst_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rst_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(rst_lbl, "Reset Defaults");
    lv_obj_center(rst_lbl);
}

static void build_orb_tab(lv_obj_t *panel) {
    size_t count = 0;
    const ui_orb_preset_t *presets = ui_theme_presets(&count);
    lv_obj_t *sw_row = lv_obj_create(panel);
    lv_obj_set_size(sw_row, lv_pct(100), 60);
    lv_obj_align(sw_row, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_bg_opa(sw_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sw_row, 0, 0);
    lv_obj_set_style_pad_all(sw_row, 0, 0);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(sw_row, false);

    for (size_t i = 0; i < count && i < 6; ++i) {
        lv_obj_t *sw = lv_btn_create(sw_row);
        lv_obj_set_size(sw, 42, 42);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(presets[i].rgb), 0);
        lv_obj_add_event_cb(sw, on_swatch_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_swatch_btns[i] = sw;
    }

    lv_obj_t *st_row = lv_obj_create(panel);
    lv_obj_set_size(st_row, lv_pct(100), 52);
    lv_obj_align(st_row, LV_ALIGN_TOP_MID, 0, 102);
    lv_obj_set_style_bg_opa(st_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st_row, 0, 0);
    lv_obj_set_style_pad_all(st_row, 0, 0);
    lv_obj_set_style_pad_column(st_row, 8, 0);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(st_row, false);

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
}

static void build_settings_modal(lv_obj_t *parent) {
    static const char *const kTabNames[3] = {"Audio", "Buttons", "Orb"};

    s_settings_modal = lv_obj_create(parent);
    lv_obj_set_size(s_settings_modal, 344, 390);
    lv_obj_center(s_settings_modal);
    lv_obj_set_style_bg_color(s_settings_modal, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_color(s_settings_modal, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_settings_modal, 1, 0);
    lv_obj_set_style_radius(s_settings_modal, 18, 0);
    lv_obj_set_style_pad_all(s_settings_modal, 12, 0);
    lv_obj_set_scrollable(s_settings_modal, false);
    lv_obj_set_hidden(s_settings_modal, true);

    lv_obj_t *tab_bar = lv_obj_create(s_settings_modal);
    lv_obj_set_size(tab_bar, lv_pct(100), 38);
    lv_obj_align(tab_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 0, 0);
    lv_obj_set_style_pad_column(tab_bar, 6, 0);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(tab_bar, false);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t *b = lv_btn_create(tab_bar);
        lv_obj_set_height(b, 38);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_tab_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_label_set_text(lbl, kTabNames[i]);
        lv_obj_center(lbl);
        s_tab_btns[i] = b;
        s_tab_lbls[i] = lbl;
    }

    lv_obj_t *tab_body = lv_obj_create(s_settings_modal);
    lv_obj_set_size(tab_body, lv_pct(100), 266);
    lv_obj_align(tab_body, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_opa(tab_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_body, 0, 0);
    lv_obj_set_style_pad_all(tab_body, 0, 0);
    lv_obj_set_scrollable(tab_body, false);

    for (int i = 0; i < 3; ++i) {
        s_tab_panels[i] = create_tab_panel(tab_body);
    }
    build_audio_tab(s_tab_panels[0]);
    build_buttons_tab(s_tab_panels[1]);
    build_orb_tab(s_tab_panels[2]);

    lv_obj_t *done_btn = lv_btn_create(s_settings_modal);
    lv_obj_set_size(done_btn, 160, 42);
    lv_obj_align(done_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(done_btn, 12, 0);
    lv_obj_set_style_bg_color(done_btn, lv_color_hex(0x334155), 0);
    lv_obj_add_event_cb(done_btn, on_settings_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_lbl = lv_label_create(done_btn);
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_14, 0);
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
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    s_snap.vol_pct        = prefs.speaker_volume_pct;
    s_snap.guard_mode     = prefs.echo_guard_mode;
    s_snap.shield_vol_pct = prefs.auto_shield_vol_pct;
    s_snap.dirty_state    = true;
    s_snap.dirty_agent    = true;
    s_snap.dirty_telem    = true;
    s_snap.dirty_audio    = true;
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

    s_vol_btn = lv_btn_create(top_bar);
    lv_obj_set_size(s_vol_btn, 94, 26);
    lv_obj_align(s_vol_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_vol_btn, 10, 0);
    lv_obj_set_style_bg_color(s_vol_btn, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(s_vol_btn, on_open_audio_settings, LV_EVENT_CLICKED, NULL);
    s_vol_lbl = lv_label_create(s_vol_btn);
    lv_obj_set_style_text_font(s_vol_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text_fmt(s_vol_lbl, LV_SYMBOL_VOLUME_MAX " %d%%", s_snap.vol_pct);
    lv_obj_center(s_vol_lbl);

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

    build_settings_modal(scr);
    build_confirm_modal(scr);

    s_toast_pill = lv_obj_create(scr);
    lv_obj_set_size(s_toast_pill, 236, 30);
    lv_obj_align(s_toast_pill, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_radius(s_toast_pill, 15, 0);
    lv_obj_set_style_bg_color(s_toast_pill, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_color(s_toast_pill, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_border_width(s_toast_pill, 1, 0);
    lv_obj_set_style_pad_all(s_toast_pill, 0, 0);
    lv_obj_set_scrollable(s_toast_pill, false);
    lv_obj_set_hidden(s_toast_pill, true);
    s_toast_lbl = lv_label_create(s_toast_pill);
    lv_obj_set_style_text_font(s_toast_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_obj_center(s_toast_lbl);

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

void ui_app_sync_audio_prefs(int vol_pct, audio_echo_guard_mode_t mode, int shield_vol_pct) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap.vol_pct        = vol_pct;
    s_snap.guard_mode     = mode;
    s_snap.shield_vol_pct = shield_vol_pct;
    s_snap.dirty_audio    = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_show_toast(const char *msg) {
    if (!s_snap_mux || !msg) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    snprintf(s_snap.toast_msg, sizeof(s_snap.toast_msg), "%s", msg);
    s_snap.dirty_toast = true;
    xSemaphoreGive(s_snap_mux);
}
