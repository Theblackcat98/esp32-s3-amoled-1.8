#include "ui_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_prefs.h"
#include "audio_engine.h"
#include "audio_guard.h"
#include "board_hw.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "ui_app";

/* Screen geometry & feed memory bounds */
#define SCREEN_W           368
#define SCREEN_H           448
#define WAVE_BAR_COUNT     14
#define MAX_FEED_TURNS     8
#define USER_TEXT_MAX      256
#define AGENT_TEXT_MAX     2048
#define FEED_OP_QUEUE_CAP  24
#define FEED_OP_TEXT_MAX   512

/* Palette: True-black AMOLED & warm stone neutrals (ZERO cyan anywhere) */
#define COL_BLACK          0x000000
#define COL_SURFACE_SHEET  0x0A0A09
#define COL_SURFACE_DOCK   0x121210
#define COL_SURFACE_CARD   0x131311
#define COL_SURFACE_PILL   0x141412
#define COL_SURFACE_TABS   0x161513
#define COL_SURFACE_USER   0x171614
#define COL_SURFACE_BTN    0x1B1A18
#define COL_SURFACE_TAB_ON 0x292724
#define COL_BORDER_SUBTLE  0x1A1917
#define COL_BORDER_CARD    0x262420
#define COL_BORDER_USER    0x282623
#define COL_BORDER_DOCK    0x292724
#define COL_BORDER_PILL    0x2A2825
#define COL_BORDER_TOAST   0x3B3834

#define COL_TEXT_PRIMARY   0xF5F5F4
#define COL_TEXT_BRIGHT    0xFFFFFF
#define COL_TEXT_BODY      0xE7E5E4
#define COL_TEXT_CHIP      0xD6D3D1
#define COL_TEXT_DIM       0xA8A29E
#define COL_TEXT_MUTED     0x78716C

/* Pure semantic state colors */
#define COL_STATE_READY    0xD6D3D1
#define COL_STATE_CONNECT  0xD97706
#define COL_STATE_LISTEN   0xF59E0B
#define COL_STATE_THINK    0xEAB308
#define COL_STATE_SPEAK    0x10B981
#define COL_STATE_INTR     0xFFFFFF
#define COL_STATE_TOOL     0x8B5CF6
#define COL_STATE_ERROR    0xEF4444

typedef enum {
    UI_OP_CLEAR_FEED = 0,
    UI_OP_USER_TURN,
    UI_OP_AGENT_NEW,
    UI_OP_AGENT_DELTA,
    UI_OP_MARK_INTERRUPTED,
} ui_feed_op_type_t;

typedef struct {
    ui_feed_op_type_t type;
    uint32_t          dur_ms;
    char              text[FEED_OP_TEXT_MAX];
} ui_feed_op_t;

typedef struct {
    bool      used;
    bool      has_user;
    bool      has_agent;
    bool      is_active;
    lv_obj_t *container;
    lv_obj_t *user_row;
    lv_obj_t *user_chip;
    lv_obj_t *user_lbl;
    lv_obj_t *agent_hdr;
    lv_obj_t *agent_body;
    char     *user_buf;
    char     *agent_buf;
} ui_turn_t;

typedef struct {
    bool dirty_state;
    bool dirty_agent;
    bool dirty_mute;
    bool dirty_telem;
    bool dirty_audio;
    bool dirty_toast;
    bool dirty_confirm;
    bool dirty_tool_banner;
    bool dirty_shutdown;

    app_state_t             state;
    char                    status_text[80];
    char                    agent_name[32];
    char                    agent_role[48];
    bool                    muted;
    int                     vol_pct;
    audio_echo_guard_mode_t guard_mode;
    int                     shield_vol_pct;
    char                    toast_msg[48];

    bool                  wifi_ok;
    int                   rssi;
    uint32_t              rtt_ms;
    board_pmu_telemetry_t pmu;

    bool tool_running;
    char tool_name[48];

    bool confirm_pending;
    char confirm_call_id[64];
    char confirm_name[48];
    char confirm_summary[128];

    int64_t flash_until_us;
} ui_snapshot_t;

static SemaphoreHandle_t s_snap_mux = NULL;
static ui_snapshot_t     s_snap;

/* PSRAM-backed feed op FIFO & bounded 8-turn ring */
static ui_feed_op_t *s_op_queue   = NULL;
static size_t        s_op_head    = 0;
static size_t        s_op_tail    = 0;
static size_t        s_op_count   = 0;
static ui_turn_t     s_turns[MAX_FEED_TURNS];
static int           s_turn_head  = 0;
static int           s_turn_count = 0;

static bool     s_dimmed             = false;
static bool     s_updating_ui        = false;
static int      s_active_tab         = 0;
static int64_t  s_last_activity_us   = 0;
static int64_t  s_toast_hide_us      = 0;
static bool     s_user_detached      = false;
static bool     s_auto_scrolling     = false;
static bool     s_jumping_to_bottom  = false;
static uint32_t s_last_rendered_rgb  = 0xFFFFFFFFU;
static int      s_last_wash_opa      = -1;
static int      s_last_bar_h[WAVE_BAR_COUNT];
static float    s_smooth_rms         = 0.0f;

/* Living Top Edge Aura */
static lv_obj_t *s_edge_wash  = NULL;
static lv_obj_t *s_aura_track = NULL;
static lv_obj_t *s_aura_fill  = NULL;

/* Compact Header */
static lv_obj_t *s_state_dot  = NULL;
static lv_obj_t *s_agent_lbl  = NULL;
static lv_obj_t *s_state_lbl  = NULL;
static lv_obj_t *s_vol_hit    = NULL;
static lv_obj_t *s_vol_pill   = NULL;
static lv_obj_t *s_vol_lbl    = NULL;
static lv_obj_t *s_shield_dot = NULL;
static lv_obj_t *s_batt_lbl   = NULL;

/* Sub-Header Spectrum & Telemetry */
static lv_obj_t *s_wave_bars[WAVE_BAR_COUNT] = {NULL};
static lv_obj_t *s_muted_pill                = NULL;
static lv_obj_t *s_meta_lbl                  = NULL;

/* Main Stage & Multi-Turn Feed */
static lv_obj_t *s_stage         = NULL;
static lv_obj_t *s_tool_banner   = NULL;
static lv_obj_t *s_tool_name_lbl = NULL;
static lv_obj_t *s_tool_bar_fill = NULL;
static lv_obj_t *s_feed          = NULL;
static lv_obj_t *s_idle_hero     = NULL;
static lv_obj_t *s_idle_title    = NULL;
static lv_obj_t *s_idle_sub      = NULL;
static lv_obj_t *s_jump_pill     = NULL;
static lv_obj_t *s_jump_dot      = NULL;
static lv_obj_t *s_jump_lbl      = NULL;

/* Unified Bottom Capsule Dock */
static lv_obj_t *s_mute_btn   = NULL;
static lv_obj_t *s_mute_lbl   = NULL;
static lv_obj_t *s_action_btn = NULL;
static lv_obj_t *s_action_lbl = NULL;
static lv_obj_t *s_right_btn  = NULL;
static lv_obj_t *s_right_lbl  = NULL;

/* Settings (2-Tab: Audio & Guard, Buttons), Confirm Modal, Toast */
static lv_obj_t *s_settings_modal                    = NULL;
static lv_obj_t *s_tab_btns[2]                       = {NULL};
static lv_obj_t *s_tab_lbls[2]                       = {NULL};
static lv_obj_t *s_tab_panels[2]                     = {NULL};
static lv_obj_t *s_vol_val_lbl                       = NULL;
static lv_obj_t *s_vol_slider                        = NULL;
static lv_obj_t *s_guard_badge                       = NULL;
static lv_obj_t *s_guard_btns[AUDIO_GUARD_MODE_COUNT] = {NULL};
static lv_obj_t *s_guard_lbls[AUDIO_GUARD_MODE_COUNT] = {NULL};
static lv_obj_t *s_shield_row                        = NULL;
static lv_obj_t *s_shield_val_lbl                    = NULL;
static lv_obj_t *s_guard_hint_lbl                    = NULL;
static lv_obj_t *s_btn_act_lbls[BTN_SLOT_COUNT]      = {NULL};
static lv_obj_t *s_confirm_modal                     = NULL;
static lv_obj_t *s_confirm_title                     = NULL;
static lv_obj_t *s_confirm_body                      = NULL;
static lv_obj_t *s_toast_pill                        = NULL;
static lv_obj_t *s_toast_lbl                         = NULL;
static lv_obj_t *s_shutdown_lbl                      = NULL;

static void wake_if_dimmed(void) {
    s_last_activity_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        board_hw_set_brightness(85);
    }
}

static uint32_t semantic_state_rgb(app_state_t st, int64_t flash_until_us) {
    switch (st) {
        case APP_STATE_READY:        return COL_STATE_READY;
        case APP_STATE_BOOTING:
        case APP_STATE_WIFI_WAIT:
        case APP_STATE_CONNECTING:
        case APP_STATE_RECONNECTING: return COL_STATE_CONNECT;
        case APP_STATE_LISTENING:    return COL_STATE_LISTEN;
        case APP_STATE_THINKING:     return COL_STATE_THINK;
        case APP_STATE_SPEAKING:     return COL_STATE_SPEAK;
        case APP_STATE_INTERRUPTED:
            return (esp_timer_get_time() < flash_until_us) ? COL_STATE_INTR : COL_STATE_LISTEN;
        case APP_STATE_TOOL_CALLING: return COL_STATE_TOOL;
        case APP_STATE_ERROR:        return COL_STATE_ERROR;
        default:                     return COL_STATE_READY;
    }
}

static const char *state_badge_str(app_state_t st) {
    switch (st) {
        case APP_STATE_BOOTING:      return "BOOTING";
        case APP_STATE_WIFI_WAIT:    return "WIFI WAIT";
        case APP_STATE_READY:        return "READY";
        case APP_STATE_CONNECTING:   return "CONNECTING";
        case APP_STATE_LISTENING:    return "LISTENING";
        case APP_STATE_THINKING:     return "THINKING...";
        case APP_STATE_SPEAKING:     return "SPEAKING";
        case APP_STATE_INTERRUPTED:  return "INTERRUPTED";
        case APP_STATE_TOOL_CALLING: return "TOOL CALL";
        case APP_STATE_ERROR:        return "OFFLINE";
        case APP_STATE_RECONNECTING: return "RECONNECT";
        default:                     return "READY";
    }
}

static bool is_idle_like_state(app_state_t st) {
    return (st == APP_STATE_READY || st == APP_STATE_ERROR ||
            st == APP_STATE_BOOTING || st == APP_STATE_WIFI_WAIT);
}

static bool is_sweeping_state(app_state_t st) {
    return (st == APP_STATE_BOOTING || st == APP_STATE_WIFI_WAIT ||
            st == APP_STATE_CONNECTING || st == APP_STATE_RECONNECTING ||
            st == APP_STATE_THINKING || st == APP_STATE_TOOL_CALLING);
}

static bool is_shield_armed(int vol_pct, audio_echo_guard_mode_t mode, int shield_vol_pct) {
    return (mode == AUDIO_GUARD_STRICT) || (mode == AUDIO_GUARD_AUTO && vol_pct >= shield_vol_pct);
}

/* ==================== PSRAM OP QUEUE HELPERS (caller holds s_snap_mux) ==================== */
static void enqueue_op_locked(ui_feed_op_type_t type, const char *text, uint32_t dur_ms) {
    if (!s_op_queue) return;

    /* Coalesce consecutive streaming text deltas to avoid overflowing queue */
    if (type == UI_OP_AGENT_DELTA && s_op_count > 0 && text) {
        const size_t last_idx = (s_op_tail + FEED_OP_QUEUE_CAP - 1U) % FEED_OP_QUEUE_CAP;
        ui_feed_op_t *last    = &s_op_queue[last_idx];
        if (last->type == UI_OP_AGENT_DELTA) {
            const size_t cur = strlen(last->text);
            const size_t add = strlen(text);
            if (cur + add < FEED_OP_TEXT_MAX) {
                memcpy(last->text + cur, text, add + 1);
                return;
            }
        }
    }

    if (s_op_count >= FEED_OP_QUEUE_CAP) {
        /* Drop oldest op to keep queue bounded */
        s_op_head = (s_op_head + 1U) % FEED_OP_QUEUE_CAP;
        --s_op_count;
    }

    ui_feed_op_t *op = &s_op_queue[s_op_tail];
    op->type         = type;
    op->dur_ms       = dur_ms;
    if (text) {
        snprintf(op->text, sizeof(op->text), "%s", text);
    } else {
        op->text[0] = '\0';
    }
    s_op_tail = (s_op_tail + 1U) % FEED_OP_QUEUE_CAP;
    ++s_op_count;
}

static bool dequeue_op_locked(ui_feed_op_t *out) {
    if (!s_op_queue || s_op_count == 0) return false;
    *out      = s_op_queue[s_op_head];
    s_op_head = (s_op_head + 1U) % FEED_OP_QUEUE_CAP;
    --s_op_count;
    return true;
}

/* ==================== MULTI-TURN FEED HELPERS (LVGL lock held) ==================== */
static ui_turn_t *get_turn_by_age(int age_from_oldest) {
    if (age_from_oldest < 0 || age_from_oldest >= s_turn_count) return NULL;
    return &s_turns[(s_turn_head + age_from_oldest) % MAX_FEED_TURNS];
}

static ui_turn_t *get_latest_turn(void) {
    return (s_turn_count > 0) ? get_turn_by_age(s_turn_count - 1) : NULL;
}

static void demote_turn(ui_turn_t *turn, const char *agent_name) {
    if (!turn || !turn->used || !turn->is_active) return;
    turn->is_active = false;

    if (turn->container) {
        lv_obj_set_style_border_side(turn->container, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(turn->container, 1, 0);
        lv_obj_set_style_border_color(turn->container, lv_color_hex(COL_BORDER_SUBTLE), 0);
        lv_obj_set_style_pad_bottom(turn->container, 12, 0);
    }
    if (turn->agent_hdr) {
        lv_label_set_text_fmt(turn->agent_hdr, "%s " LV_SYMBOL_BULLET " EARLIER", agent_name);
        lv_obj_set_style_text_color(turn->agent_hdr, lv_color_hex(COL_TEXT_MUTED), 0);
    }
    if (turn->agent_body) {
        lv_obj_set_style_text_font(turn->agent_body, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(turn->agent_body, lv_color_hex(COL_TEXT_DIM), 0);
    }
}

static void free_oldest_turn(void) {
    if (s_turn_count <= 0) return;
    ui_turn_t *oldest = &s_turns[s_turn_head];
    if (oldest->container) {
        lv_obj_delete(oldest->container);
    }
    oldest->container  = NULL;
    oldest->user_row   = NULL;
    oldest->user_chip  = NULL;
    oldest->user_lbl   = NULL;
    oldest->agent_hdr  = NULL;
    oldest->agent_body = NULL;
    oldest->used       = false;
    oldest->has_user   = false;
    oldest->has_agent  = false;
    oldest->is_active  = false;
    if (oldest->user_buf) oldest->user_buf[0] = '\0';
    if (oldest->agent_buf) oldest->agent_buf[0] = '\0';

    s_turn_head = (s_turn_head + 1) % MAX_FEED_TURNS;
    --s_turn_count;
}

static ui_turn_t *alloc_new_turn(const char *agent_name) {
    ui_turn_t *prev = get_latest_turn();
    if (prev) {
        demote_turn(prev, agent_name);
    }
    if (s_turn_count >= MAX_FEED_TURNS) {
        free_oldest_turn();
    }

    const int slot_idx = (s_turn_head + s_turn_count) % MAX_FEED_TURNS;
    ui_turn_t *t       = &s_turns[slot_idx];
    ++s_turn_count;

    t->used       = true;
    t->has_user   = false;
    t->has_agent  = false;
    t->is_active  = true;
    t->user_row   = NULL;
    t->user_chip  = NULL;
    t->user_lbl   = NULL;
    t->agent_hdr  = NULL;
    t->agent_body = NULL;
    if (t->user_buf) t->user_buf[0] = '\0';
    if (t->agent_buf) t->agent_buf[0] = '\0';

    t->container = lv_obj_create(s_feed);
    lv_obj_set_width(t->container, lv_pct(100));
    lv_obj_set_height(t->container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(t->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t->container, 0, 0);
    lv_obj_set_style_pad_all(t->container, 0, 0);
    lv_obj_set_style_pad_row(t->container, 8, 0);
    lv_obj_set_flex_flow(t->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(t->container, false);
    lv_obj_set_clickable(t->container, false);

    if (s_idle_hero) {
        lv_obj_set_hidden(s_idle_hero, true);
    }
    return t;
}

static void attach_user_chip_to_turn(ui_turn_t *t, const char *text, uint32_t dur_ms) {
    if (!t || !t->container || !t->user_buf) return;

    if (text && text[0] != '\0') {
        snprintf(t->user_buf, USER_TEXT_MAX, "%s", text);
    } else {
        const uint32_t tenths = (dur_ms + 50U) / 100U;
        snprintf(
            t->user_buf,
            USER_TEXT_MAX,
            "You " LV_SYMBOL_BULLET " %lu.%lus",
            (unsigned long)(tenths / 10U),
            (unsigned long)(tenths % 10U));
    }

    if (!t->has_user) {
        t->has_user = true;
        t->user_row = lv_obj_create(t->container);
        lv_obj_set_width(t->user_row, lv_pct(100));
        lv_obj_set_height(t->user_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(t->user_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(t->user_row, 0, 0);
        lv_obj_set_style_pad_all(t->user_row, 0, 0);
        lv_obj_set_flex_flow(t->user_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t->user_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(t->user_row, false);
        lv_obj_set_clickable(t->user_row, false);

        t->user_chip = lv_obj_create(t->user_row);
        lv_obj_set_size(t->user_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(t->user_chip, 286, 0);
        lv_obj_set_style_bg_color(t->user_chip, lv_color_hex(COL_SURFACE_USER), 0);
        lv_obj_set_style_border_color(t->user_chip, lv_color_hex(COL_BORDER_USER), 0);
        lv_obj_set_style_border_width(t->user_chip, 1, 0);
        lv_obj_set_style_radius(t->user_chip, 12, 0);
        lv_obj_set_style_pad_hor(t->user_chip, 11, 0);
        lv_obj_set_style_pad_ver(t->user_chip, 6, 0);
        lv_obj_set_scrollable(t->user_chip, false);
        lv_obj_set_clickable(t->user_chip, false);

        t->user_lbl = lv_label_create(t->user_chip);
        lv_obj_set_size(t->user_lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(t->user_lbl, 260, 0);
        lv_label_set_long_mode(t->user_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(t->user_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(t->user_lbl, lv_color_hex(COL_TEXT_CHIP), 0);
    }

    lv_label_set_text_static(t->user_lbl, t->user_buf);
}

static void ensure_agent_block_on_turn(ui_turn_t *t, const char *agent_name, uint32_t state_rgb) {
    if (!t || !t->container || t->has_agent) return;
    t->has_agent = true;

    lv_obj_t *box = lv_obj_create(t->container);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, 3, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);

    t->agent_hdr = lv_label_create(box);
    lv_obj_set_style_text_font(t->agent_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t->agent_hdr, lv_color_hex(state_rgb), 0);
    lv_label_set_text_fmt(t->agent_hdr, "%s " LV_SYMBOL_BULLET " NOW", agent_name);

    t->agent_body = lv_label_create(box);
    lv_obj_set_width(t->agent_body, lv_pct(100));
    lv_label_set_long_mode(t->agent_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(t->agent_body, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t->agent_body, lv_color_hex(COL_TEXT_BRIGHT), 0);
    lv_obj_set_style_text_line_space(t->agent_body, 3, 0);
    lv_label_set_text_static(t->agent_body, t->agent_buf);
}

static void scroll_feed_to_bottom_if_attached(bool animate) {
    if (!s_feed || s_user_detached) return;
    s_auto_scrolling = true;
    lv_obj_update_layout(s_feed);
    lv_obj_scroll_to_y(s_feed, LV_COORD_MAX, animate ? LV_ANIM_ON : LV_ANIM_OFF);
    s_auto_scrolling = false;
}

static void update_jump_pill_label(app_state_t st) {
    if (!s_jump_lbl) return;
    lv_label_set_text(
        s_jump_lbl,
        (st == APP_STATE_SPEAKING) ? LV_SYMBOL_DOWN " Live" : LV_SYMBOL_DOWN " Bottom");
}

static void clear_all_turns(void) {
    while (s_turn_count > 0) {
        free_oldest_turn();
    }
    s_user_detached     = false;
    s_jumping_to_bottom = false;
    if (s_jump_pill) lv_obj_set_hidden(s_jump_pill, true);
    if (s_idle_hero) lv_obj_set_hidden(s_idle_hero, false);
}

static void process_feed_ops(const ui_snapshot_t *local, uint32_t state_rgb) {
    bool did_update = false;

    while (true) {
        ui_feed_op_t op;
        xSemaphoreTake(s_snap_mux, portMAX_DELAY);
        const bool has_op = dequeue_op_locked(&op);
        xSemaphoreGive(s_snap_mux);
        if (!has_op) break;

        did_update = true;
        switch (op.type) {
            case UI_OP_CLEAR_FEED:
                clear_all_turns();
                break;

            case UI_OP_USER_TURN: {
                ui_turn_t *cur = get_latest_turn();
                if (op.text[0] != '\0' && cur && cur->has_user && !cur->has_agent) {
                    /* Enrich placeholder duration chip with Phase 2 ASR text */
                    attach_user_chip_to_turn(cur, op.text, 0);
                } else {
                    ui_turn_t *t = alloc_new_turn(local->agent_name);
                    attach_user_chip_to_turn(t, op.text, op.dur_ms);
                }
                break;
            }

            case UI_OP_AGENT_NEW: {
                ui_turn_t *t = get_latest_turn();
                if (!t || t->has_agent) {
                    t = alloc_new_turn(local->agent_name);
                }
                if (t->agent_buf) t->agent_buf[0] = '\0';
                ensure_agent_block_on_turn(t, local->agent_name, state_rgb);
                break;
            }

            case UI_OP_AGENT_DELTA: {
                ui_turn_t *t = get_latest_turn();
                if (!t || !t->has_agent) {
                    if (!t) t = alloc_new_turn(local->agent_name);
                    ensure_agent_block_on_turn(t, local->agent_name, state_rgb);
                }
                if (t && t->agent_buf && t->agent_body) {
                    const size_t cur_len = strlen(t->agent_buf);
                    const size_t add_len = strlen(op.text);
                    if (cur_len + add_len < AGENT_TEXT_MAX) {
                        memcpy(t->agent_buf + cur_len, op.text, add_len + 1);
                    } else if (cur_len + 4 < AGENT_TEXT_MAX) {
                        const size_t room = AGENT_TEXT_MAX - 1 - cur_len;
                        memcpy(t->agent_buf + cur_len, op.text, room);
                        t->agent_buf[AGENT_TEXT_MAX - 1] = '\0';
                    }
                    lv_label_set_text_static(t->agent_body, t->agent_buf);
                }
                break;
            }

            case UI_OP_MARK_INTERRUPTED: {
                ui_turn_t *t = get_latest_turn();
                if (t && t->has_agent && t->agent_buf && t->agent_body && t->agent_buf[0] != '\0') {
                    const size_t cur_len = strlen(t->agent_buf);
                    static const char kTag[] = " [Interrupted]";
                    if (cur_len + sizeof(kTag) < AGENT_TEXT_MAX) {
                        memcpy(t->agent_buf + cur_len, kTag, sizeof(kTag));
                        lv_label_set_text_static(t->agent_body, t->agent_buf);
                    }
                }
                break;
            }
        }
    }

    if (did_update) {
        scroll_feed_to_bottom_if_attached(false);
    }
}

/* ==================== UI PANEL REFRESHERS ==================== */
static void refresh_idle_hero(app_state_t st, const char *status_text, uint32_t state_rgb) {
    if (!s_idle_title || !s_idle_sub) return;
    switch (st) {
        case APP_STATE_BOOTING:
        case APP_STATE_WIFI_WAIT:
            lv_label_set_text(s_idle_title, "Joining Wi-Fi...");
            lv_obj_set_style_text_color(s_idle_title, lv_color_hex(state_rgb), 0);
            lv_label_set_text(s_idle_sub, (status_text && status_text[0]) ? status_text : "Connecting to 2.4 GHz network...");
            break;
        case APP_STATE_CONNECTING:
        case APP_STATE_RECONNECTING:
            lv_label_set_text(s_idle_title, (st == APP_STATE_CONNECTING) ? "Connecting..." : "Reconnecting...");
            lv_obj_set_style_text_color(s_idle_title, lv_color_hex(state_rgb), 0);
            lv_label_set_text(s_idle_sub, (status_text && status_text[0]) ? status_text : "Opening /v1/realtime WebSocket...");
            break;
        case APP_STATE_LISTENING:
            lv_label_set_text(s_idle_title, "Listening...");
            lv_obj_set_style_text_color(s_idle_title, lv_color_hex(state_rgb), 0);
            lv_label_set_text(s_idle_sub, "Full-duplex AEC active. Speak naturally - no button hold needed.");
            break;
        case APP_STATE_ERROR:
            lv_label_set_text(s_idle_title, "Server Unreachable");
            lv_obj_set_style_text_color(s_idle_title, lv_color_hex(state_rgb), 0);
            lv_label_set_text(s_idle_sub, (status_text && status_text[0]) ? status_text : "Could not reach LAN host. Tap Retry LAN.");
            break;
        default:
            lv_label_set_text(s_idle_title, "Ready when you are.");
            lv_obj_set_style_text_color(s_idle_title, lv_color_hex(COL_TEXT_PRIMARY), 0);
            lv_label_set_text(s_idle_sub, "Tap Start Talking or press the side BOOT button to connect to your LAN server.");
            break;
    }
}

static void refresh_meta_line(const ui_snapshot_t *local) {
    if (!s_meta_lbl) return;
    const bool shielded = is_shield_armed(local->vol_pct, local->guard_mode, local->shield_vol_pct);
    const char *guard_s = shielded ? "Shield ON" : audio_guard_mode_name(local->guard_mode);
    if (!local->wifi_ok) {
        lv_label_set_text_fmt(s_meta_lbl, "Wi-Fi... " LV_SYMBOL_BULLET " %s", guard_s);
    } else if (local->rtt_ms > 0) {
        lv_label_set_text_fmt(
            s_meta_lbl,
            "%lums " LV_SYMBOL_BULLET " %s%s",
            (unsigned long)local->rtt_ms,
            shielded ? "" : "Guard ",
            guard_s);
    } else {
        lv_label_set_text_fmt(
            s_meta_lbl,
            "%ddBm " LV_SYMBOL_BULLET " %s%s",
            local->rssi,
            shielded ? "" : "Guard ",
            guard_s);
    }
}

static void refresh_audio_panel(int vol_pct, audio_echo_guard_mode_t mode, int shield_vol_pct) {
    const bool shielded = is_shield_armed(vol_pct, mode, shield_vol_pct);

    if (s_vol_lbl) {
        const char *sym = (vol_pct == 0) ? LV_SYMBOL_MUTE
                                         : ((vol_pct < 50) ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
        lv_label_set_text_fmt(s_vol_lbl, "%s %d%%", sym, vol_pct);
    }
    if (s_shield_dot) {
        lv_obj_set_hidden(s_shield_dot, !shielded);
    }
    if (s_vol_pill) {
        lv_obj_set_style_border_color(
            s_vol_pill,
            lv_color_hex(shielded ? COL_STATE_LISTEN : COL_BORDER_PILL),
            0);
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
        lv_obj_set_style_text_color(
            s_guard_badge,
            lv_color_hex(shielded ? COL_STATE_LISTEN : COL_STATE_SPEAK),
            0);
    }
    for (int i = 0; i < AUDIO_GUARD_MODE_COUNT; ++i) {
        if (!s_guard_btns[i]) continue;
        const bool sel = ((int)mode == i);
        lv_obj_set_style_bg_color(s_guard_btns[i], lv_color_hex(sel ? 0x332207 : COL_SURFACE_BTN), 0);
        lv_obj_set_style_border_color(s_guard_btns[i], lv_color_hex(sel ? COL_STATE_LISTEN : COL_BORDER_CARD), 0);
        if (s_guard_lbls[i]) {
            lv_obj_set_style_text_color(s_guard_lbls[i], lv_color_hex(sel ? COL_TEXT_BRIGHT : COL_TEXT_DIM), 0);
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
                ? "Speaker blocks mic barge-in at any volume (tap Interrupt)."
                : ((mode == AUDIO_GUARD_ALWAYS)
                       ? "Full-duplex barge-in at all volumes using dynamic echo gate."
                       : "Full-duplex AEC below threshold; shields playback above.");
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
    if (tab_idx < 0 || tab_idx >= 2) tab_idx = 0;
    s_active_tab = tab_idx;
    for (int i = 0; i < 2; ++i) {
        const bool active = (i == s_active_tab);
        if (s_tab_panels[i]) {
            lv_obj_set_hidden(s_tab_panels[i], !active);
        }
        if (s_tab_btns[i]) {
            lv_obj_set_style_bg_color(
                s_tab_btns[i],
                lv_color_hex(active ? COL_SURFACE_TAB_ON : COL_SURFACE_TABS),
                0);
        }
        if (s_tab_lbls[i]) {
            lv_obj_set_style_text_color(
                s_tab_lbls[i],
                lv_color_hex(active ? COL_TEXT_BRIGHT : COL_TEXT_DIM),
                0);
        }
    }
}

static void apply_state_colors_and_dock(app_state_t st, uint32_t rgb) {
    if (rgb != s_last_rendered_rgb) {
        s_last_rendered_rgb = rgb;
        const lv_color_t c  = lv_color_hex(rgb);

        if (s_state_dot) lv_obj_set_style_bg_color(s_state_dot, c, 0);
        if (s_state_lbl) lv_obj_set_style_text_color(s_state_lbl, c, 0);
        if (s_aura_fill) lv_obj_set_style_bg_color(s_aura_fill, c, 0);
        if (s_edge_wash) lv_obj_set_style_bg_color(s_edge_wash, c, 0);
        if (s_jump_pill) lv_obj_set_style_border_color(s_jump_pill, c, 0);
        if (s_jump_dot) lv_obj_set_style_bg_color(s_jump_dot, c, 0);

        for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
            if (s_wave_bars[i]) lv_obj_set_style_bg_color(s_wave_bars[i], c, 0);
        }
        ui_turn_t *active = get_latest_turn();
        if (active && active->is_active && active->agent_hdr) {
            lv_obj_set_style_text_color(active->agent_hdr, c, 0);
        }
    }

    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, state_badge_str(st));
    }

    /* Unified Bottom Capsule Dock contextual buttons */
    if (s_action_btn && s_action_lbl) {
        const char *center_txt = "Start Talking";
        uint32_t bg_hex        = COL_STATE_READY;
        uint32_t fg_hex        = 0x090908;
        uint32_t bd_hex        = COL_STATE_READY;
        int bd_w               = 0;

        switch (st) {
            case APP_STATE_READY:
                center_txt = "Start Talking";
                bg_hex     = COL_STATE_READY;
                break;
            case APP_STATE_BOOTING:
            case APP_STATE_WIFI_WAIT:
                center_txt = "Start Talking";
                bg_hex     = COL_STATE_CONNECT;
                break;
            case APP_STATE_CONNECTING:
            case APP_STATE_RECONNECTING:
                center_txt = "Cancel";
                bg_hex     = COL_STATE_CONNECT;
                break;
            case APP_STATE_LISTENING:
                center_txt = "Listening";
                bg_hex     = 0x241806;
                fg_hex     = 0xFCD34D;
                bd_hex     = COL_STATE_LISTEN;
                bd_w       = 1;
                break;
            case APP_STATE_THINKING:
                center_txt = "Cancel";
                bg_hex     = COL_STATE_THINK;
                break;
            case APP_STATE_SPEAKING:
                center_txt = "Interrupt";
                bg_hex     = COL_STATE_SPEAK;
                break;
            case APP_STATE_INTERRUPTED:
                center_txt = "Listening...";
                bg_hex     = COL_TEXT_BRIGHT;
                break;
            case APP_STATE_TOOL_CALLING:
                center_txt = "Cancel";
                bg_hex     = COL_STATE_TOOL;
                fg_hex     = COL_TEXT_BRIGHT;
                break;
            case APP_STATE_ERROR:
                center_txt = "Retry LAN";
                bg_hex     = COL_STATE_ERROR;
                fg_hex     = COL_TEXT_BRIGHT;
                break;
        }

        lv_label_set_text(s_action_lbl, center_txt);
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_border_color(s_action_btn, lv_color_hex(bd_hex), 0);
        lv_obj_set_style_border_width(s_action_btn, bd_w, 0);
        lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(fg_hex), 0);
    }

    if (s_right_btn && s_right_lbl) {
        if (is_idle_like_state(st)) {
            lv_label_set_text(s_right_lbl, LV_SYMBOL_SETTINGS " Setup");
            lv_obj_set_style_bg_color(s_right_btn, lv_color_hex(COL_SURFACE_BTN), 0);
            lv_obj_set_style_text_color(s_right_lbl, lv_color_hex(COL_TEXT_DIM), 0);
        } else {
            lv_label_set_text(s_right_lbl, LV_SYMBOL_CLOSE " End");
            lv_obj_set_style_bg_color(s_right_btn, lv_color_hex(0x261010), 0);
            lv_obj_set_style_text_color(s_right_lbl, lv_color_hex(0xFCA5A5), 0);
        }
    }
}

static void animate_aura_and_wave(app_state_t st, bool muted, uint8_t raw_level, uint32_t tick) {
    /* Smooth target RMS so bar & aura motion feels calm and liquid at 30 FPS */
    float target = 0.06f;
    if (st == APP_STATE_SPEAKING || (st == APP_STATE_LISTENING && !muted)) {
        target = (float)raw_level / 100.0f;
        if (target < 0.08f) target = 0.08f;
    } else if (is_sweeping_state(st)) {
        target = 0.35f;
    } else if (st == APP_STATE_INTERRUPTED) {
        target = 0.85f;
    } else if (st == APP_STATE_READY || st == APP_STATE_ERROR || muted) {
        target = 0.0f;
    }
    s_smooth_rms = s_smooth_rms * 0.62f + target * 0.38f;

    /* 1. Top edge light bar */
    if (s_aura_fill) {
        if (is_sweeping_state(st)) {
            const int32_t track_w = 316;
            const int32_t bar_w   = 136;
            const int32_t phase   = (int32_t)((tick * 7U) % 200U);
            const int32_t tri     = (phase < 100) ? phase : (200 - phase); /* 0..100 */
            const int32_t x       = -14 + ((track_w - bar_w + 28) * tri) / 100;
            lv_obj_set_width(s_aura_fill, bar_w);
            lv_obj_set_x(s_aura_fill, x);
            lv_obj_set_style_bg_opa(s_aura_fill, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_width(s_aura_fill, 316);
            lv_obj_set_x(s_aura_fill, 0);
            lv_opa_t opa = LV_OPA_30;
            if (st == APP_STATE_ERROR) {
                opa = LV_OPA_90;
            } else if (st == APP_STATE_LISTENING || st == APP_STATE_SPEAKING || st == APP_STATE_INTERRUPTED) {
                int v = 115 + (int)(s_smooth_rms * 140.0f);
                if (v > 255) v = 255;
                opa = (lv_opa_t)v;
            }
            lv_obj_set_style_bg_opa(s_aura_fill, opa, 0);
        }
    }

    /* 2. Top gradient wash (quantized to reduce QSPI invalidations) */
    if (s_edge_wash) {
        int want_opa = 0;
        if (st == APP_STATE_LISTENING || st == APP_STATE_SPEAKING) {
            want_opa = 28 + (int)(s_smooth_rms * 56.0f);
        } else if (is_sweeping_state(st)) {
            want_opa = 40;
        } else if (st == APP_STATE_INTERRUPTED) {
            want_opa = 95;
        } else if (st == APP_STATE_ERROR) {
            want_opa = 48;
        }
        if (abs(want_opa - s_last_wash_opa) >= 6) {
            s_last_wash_opa = want_opa;
            lv_obj_set_style_bg_main_opa(s_edge_wash, (lv_opa_t)want_opa, 0);
        }
    }

    /* 3. Inline violet tool banner progress bar */
    if (s_tool_banner && !lv_obj_is_hidden(s_tool_banner) && s_tool_bar_fill) {
        const int32_t phase = (int32_t)((tick * 9U) % 160U);
        const int32_t x     = -100 + ((410 * phase) / 160);
        lv_obj_set_x(s_tool_bar_fill, x);
    }

    /* 4. 14-bar sub-header voice spectrum */
    static const uint8_t kShape[WAVE_BAR_COUNT] = {
        36, 52, 72, 92, 68, 100, 84, 58, 96, 74, 88, 62, 46, 32
    };
    const bool active_wave =
        (st == APP_STATE_SPEAKING) || (st == APP_STATE_LISTENING && !muted) || is_sweeping_state(st);
    for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
        if (!s_wave_bars[i]) continue;
        int h = 3;
        if (active_wave) {
            const uint32_t p = (tick * 3U + (uint32_t)i * 5U) % 24U;
            const int tri    = (p < 12U) ? (int)p : (24 - (int)p); /* 0..12 */
            const float mod  = 0.40f + 0.60f * ((float)tri / 12.0f);
            const float lvl  = (s_smooth_rms < 0.14f) ? 0.14f : s_smooth_rms;
            h = 3 + (int)(lvl * 15.0f * ((float)kShape[i] / 100.0f) * mod);
            if (h > 18) h = 18;
        }
        if (h != s_last_bar_h[i]) {
            s_last_bar_h[i] = h;
            lv_obj_set_height(s_wave_bars[i], h);
        }
    }
}

/* ==================== EVENT CALLBACKS ==================== */
static void open_settings_modal(int tab_idx) {
    wake_if_dimmed();
    if (!s_settings_modal) return;

    int vol_pct;
    audio_echo_guard_mode_t guard_mode;
    int shield_vol_pct;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    vol_pct        = s_snap.vol_pct;
    guard_mode     = s_snap.guard_mode;
    shield_vol_pct = s_snap.shield_vol_pct;
    xSemaphoreGive(s_snap_mux);

    refresh_audio_panel(vol_pct, guard_mode, shield_vol_pct);
    refresh_buttons_panel();
    select_settings_tab(tab_idx);
    lv_obj_set_hidden(s_settings_modal, false);
    lv_obj_move_foreground(s_settings_modal);
}

static void on_screen_any_touch(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
}

static void on_open_audio_settings(lv_event_t *e) {
    (void)e;
    open_settings_modal(0);
}

static void on_feed_event(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        wake_if_dimmed();
        s_jumping_to_bottom = false;
        return;
    }

    if (code == LV_EVENT_GESTURE) {
        wake_if_dimmed();
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT) {
            session_mgr_switch_agent("next");
        } else if (dir == LV_DIR_RIGHT) {
            session_mgr_switch_agent("prev");
        }
        return;
    }

    if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_END) {
        if (code == LV_EVENT_SCROLL_END) {
            s_jumping_to_bottom = false;
        }
        if (s_auto_scrolling || s_jumping_to_bottom) {
            return;
        }
        const int32_t dist_bottom = lv_obj_get_scroll_bottom(s_feed);
        if (dist_bottom > 24) {
            if (!s_user_detached) {
                s_user_detached = true;
                update_jump_pill_label(session_mgr_get_state());
                if (s_jump_pill) lv_obj_set_hidden(s_jump_pill, false);
            }
        } else if (dist_bottom <= 14) {
            if (s_user_detached) {
                s_user_detached = false;
                if (s_jump_pill) lv_obj_set_hidden(s_jump_pill, true);
            }
        }
    }
}

static void on_jump_live_clicked(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    s_user_detached     = false;
    s_jumping_to_bottom = true;
    if (s_jump_pill) lv_obj_set_hidden(s_jump_pill, true);
    if (s_feed) {
        lv_obj_scroll_to_y(s_feed, LV_COORD_MAX, LV_ANIM_ON);
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
    const app_state_t st = session_mgr_get_state();
    ESP_LOGI(TAG, "UI center button clicked (state=%d)", (int)st);
    if (st == APP_STATE_READY || st == APP_STATE_ERROR || st == APP_STATE_WIFI_WAIT) {
        session_mgr_start_session();
    } else if (st == APP_STATE_SPEAKING || st == APP_STATE_THINKING || st == APP_STATE_TOOL_CALLING) {
        session_mgr_force_interrupt();
    } else if (st == APP_STATE_CONNECTING || st == APP_STATE_RECONNECTING) {
        session_mgr_stop_session();
    } else if (st == APP_STATE_LISTENING) {
        session_mgr_toggle_mute();
        ui_app_show_toast(session_mgr_is_muted() ? "Mic Muted" : "Mic Live");
    }
}

static void on_right_clicked(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    const app_state_t st = session_mgr_get_state();
    if (is_idle_like_state(st)) {
        open_settings_modal(0);
    } else {
        session_mgr_stop_session();
        ui_app_show_toast("Session Ended");
    }
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
    ui_app_show_toast("Button Defaults Restored");
}

static void on_settings_done(lv_event_t *e) {
    (void)e;
    wake_if_dimmed();
    if (s_settings_modal) {
        lv_obj_set_hidden(s_settings_modal, true);
    }
}

static void on_confirm_choice(lv_event_t *e) {
    wake_if_dimmed();
    const bool approved = ((uintptr_t)lv_event_get_user_data(e)) != 0;
    char call_id[64]    = {0};
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    snprintf(call_id, sizeof(call_id), "%s", s_snap.confirm_call_id);
    s_snap.confirm_pending = false;
    xSemaphoreGive(s_snap_mux);
    if (s_confirm_modal) {
        lv_obj_set_hidden(s_confirm_modal, true);
    }
    session_mgr_confirm_tool(call_id, approved);
}

/* ==================== 30 FPS UI TIMER ==================== */
static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    static uint32_t s_tick = 0;
    ++s_tick;

    ui_snapshot_t local;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    local                 = s_snap;
    s_snap.dirty_state    = false;
    s_snap.dirty_agent    = false;
    s_snap.dirty_mute     = false;
    s_snap.dirty_telem    = false;
    s_snap.dirty_audio    = false;
    s_snap.dirty_toast    = false;
    s_snap.dirty_confirm  = false;
    s_snap.dirty_tool_banner = false;
    s_snap.dirty_shutdown = false;
    xSemaphoreGive(s_snap_mux);

    if (local.dirty_shutdown && s_shutdown_lbl) {
        lv_obj_set_hidden(s_shutdown_lbl, false);
        lv_obj_move_foreground(s_shutdown_lbl);
    }

    if (local.dirty_audio) {
        refresh_audio_panel(local.vol_pct, local.guard_mode, local.shield_vol_pct);
        refresh_meta_line(&local);
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

    const uint32_t state_rgb = semantic_state_rgb(local.state, local.flash_until_us);

    /* Apply queued multi-turn feed updates in exact arrival order */
    process_feed_ops(&local, state_rgb);

    if (local.dirty_state || state_rgb != s_last_rendered_rgb) {
        apply_state_colors_and_dock(local.state, state_rgb);
        update_jump_pill_label(local.state);
        if (s_turn_count == 0) {
            refresh_idle_hero(local.state, local.status_text, state_rgb);
        }
    }

    if (local.dirty_agent && s_agent_lbl) {
        lv_label_set_text(s_agent_lbl, local.agent_name);
    }

    if (local.dirty_tool_banner && s_tool_banner && s_tool_name_lbl) {
        if (local.tool_running) {
            lv_label_set_text(s_tool_name_lbl, local.tool_name);
            lv_obj_set_hidden(s_tool_banner, false);
        } else {
            lv_obj_set_hidden(s_tool_banner, true);
        }
    }

    if (local.dirty_mute) {
        if (s_muted_pill) {
            lv_obj_set_hidden(s_muted_pill, !local.muted);
        }
        if (s_mute_btn && s_mute_lbl) {
            if (local.muted) {
                lv_label_set_text(s_mute_lbl, LV_SYMBOL_MUTE " Muted");
                lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(0x3B1214), 0);
                lv_obj_set_style_border_color(s_mute_btn, lv_color_hex(COL_STATE_ERROR), 0);
                lv_obj_set_style_border_width(s_mute_btn, 1, 0);
                lv_obj_set_style_text_color(s_mute_lbl, lv_color_hex(0xFCA5A5), 0);
            } else {
                lv_label_set_text(s_mute_lbl, LV_SYMBOL_AUDIO " Mute");
                lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(COL_SURFACE_BTN), 0);
                lv_obj_set_style_border_width(s_mute_btn, 0, 0);
                lv_obj_set_style_text_color(s_mute_lbl, lv_color_hex(COL_TEXT_BODY), 0);
            }
        }
    }

    if (local.dirty_telem) {
        refresh_meta_line(&local);
        if (s_batt_lbl) {
            if (local.pmu.online && local.pmu.batt_present && local.pmu.batt_pct >= 0) {
                lv_label_set_text_fmt(
                    s_batt_lbl,
                    "%d%%%s",
                    local.pmu.batt_pct,
                    local.pmu.charging ? LV_SYMBOL_CHARGE : "");
                lv_obj_set_style_text_color(
                    s_batt_lbl,
                    lv_color_hex(local.pmu.batt_pct <= 20 ? COL_STATE_ERROR
                                                          : (local.pmu.charging ? COL_STATE_SPEAK : COL_TEXT_DIM)),
                    0);
            } else if (local.pmu.online) {
                lv_label_set_text(s_batt_lbl, local.pmu.vbus_present ? "USB-C" : "SYS");
                lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(COL_TEXT_DIM), 0);
            } else {
                lv_label_set_text(s_batt_lbl, "USB");
                lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(COL_TEXT_DIM), 0);
            }
        }
    }

    if (local.dirty_confirm && local.confirm_pending && s_confirm_modal) {
        lv_label_set_text(s_confirm_title, local.confirm_name);
        lv_label_set_text(s_confirm_body, local.confirm_summary);
        lv_obj_set_hidden(s_confirm_modal, false);
        lv_obj_move_foreground(s_confirm_modal);
    }

    uint8_t mic_rms = 0, spk_rms = 0;
    audio_engine_get_levels(&mic_rms, &spk_rms);
    const uint8_t level = (local.state == APP_STATE_SPEAKING) ? spk_rms : mic_rms;
    animate_aura_and_wave(local.state, local.muted, level, s_tick);

    /* Idle auto-dim after APP_IDLE_DIM_SEC in READY state */
    if (APP_IDLE_DIM_SEC > 0 && !s_dimmed && local.state == APP_STATE_READY) {
        const int64_t idle_sec = (esp_timer_get_time() - s_last_activity_us) / 1000000LL;
        if (idle_sec >= APP_IDLE_DIM_SEC) {
            s_dimmed = true;
            board_hw_set_brightness(15);
        }
    }
}

/* ==================== WIDGET BUILDERS ==================== */
static lv_obj_t *create_step_btn(
    lv_obj_t     *parent,
    int           w,
    int           h,
    const char   *txt,
    lv_event_cb_t cb,
    int           delta) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1B18), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x302E2A), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)delta);
    lv_obj_t *lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    return b;
}

static lv_obj_t *create_settings_card(lv_obj_t *parent, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, (h > 0) ? h : LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_SURFACE_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_BORDER_CARD), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 13, 0);
    lv_obj_set_style_pad_row(c, 9, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(c, false);
    return c;
}

static lv_obj_t *create_tab_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(p, false);
    lv_obj_set_hidden(p, true);
    return p;
}

static void build_audio_tab(lv_obj_t *panel) {
    /* Card 1: Speaker Volume */
    lv_obj_t *vol_card = create_settings_card(panel, 0);

    lv_obj_t *vol_top = lv_obj_create(vol_card);
    lv_obj_set_size(vol_top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(vol_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_top, 0, 0);
    lv_obj_set_style_pad_all(vol_top, 0, 0);
    lv_obj_set_scrollable(vol_top, false);

    lv_obj_t *vol_hdr = lv_label_create(vol_top);
    lv_obj_set_style_text_font(vol_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vol_hdr, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_label_set_text(vol_hdr, "Speaker Volume");
    lv_obj_align(vol_hdr, LV_ALIGN_LEFT_MID, 0, 0);

    s_vol_val_lbl = lv_label_create(vol_top);
    lv_obj_set_style_text_font(s_vol_val_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_vol_val_lbl, lv_color_hex(COL_STATE_LISTEN), 0);
    lv_label_set_text(s_vol_val_lbl, "75%");
    lv_obj_align(s_vol_val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *vol_row = lv_obj_create(vol_card);
    lv_obj_set_size(vol_row, lv_pct(100), 40);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_pad_column(vol_row, 10, 0);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(vol_row, false);

    create_step_btn(vol_row, 48, 38, "-5", on_vol_step_clicked, -5);

    s_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(s_vol_slider, 1);
    lv_obj_set_height(s_vol_slider, 10);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(COL_BORDER_USER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(COL_STATE_LISTEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(COL_TEXT_PRIMARY), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, on_vol_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, on_vol_slider_event, LV_EVENT_RELEASED, NULL);

    create_step_btn(vol_row, 48, 38, "+5", on_vol_step_clicked, 5);

    /* Card 2: High-Vol Echo Guard */
    lv_obj_t *guard_card = create_settings_card(panel, 0);

    lv_obj_t *guard_top = lv_obj_create(guard_card);
    lv_obj_set_size(guard_top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(guard_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(guard_top, 0, 0);
    lv_obj_set_style_pad_all(guard_top, 0, 0);
    lv_obj_set_scrollable(guard_top, false);

    lv_obj_t *guard_hdr = lv_label_create(guard_top);
    lv_obj_set_style_text_font(guard_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(guard_hdr, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_label_set_text(guard_hdr, "High-Vol Echo Guard");
    lv_obj_align(guard_hdr, LV_ALIGN_LEFT_MID, 0, 0);

    s_guard_badge = lv_label_create(guard_top);
    lv_obj_set_style_text_font(s_guard_badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_guard_badge, lv_color_hex(COL_STATE_SPEAK), 0);
    lv_label_set_text(s_guard_badge, "FULL DUPLEX");
    lv_obj_align(s_guard_badge, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *mode_row = lv_obj_create(guard_card);
    lv_obj_set_size(mode_row, lv_pct(100), 38);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, 0, 0);
    lv_obj_set_style_pad_column(mode_row, 6, 0);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(mode_row, false);

    for (int i = 0; i < AUDIO_GUARD_MODE_COUNT; ++i) {
        lv_obj_t *b = lv_btn_create(mode_row);
        lv_obj_set_height(b, 36);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 11, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, on_guard_mode_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(lbl, audio_guard_mode_name((audio_echo_guard_mode_t)i));
        lv_obj_center(lbl);
        s_guard_btns[i] = b;
        s_guard_lbls[i] = lbl;
    }

    s_guard_hint_lbl = lv_label_create(guard_card);
    lv_obj_set_width(s_guard_hint_lbl, lv_pct(100));
    lv_label_set_long_mode(s_guard_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_guard_hint_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_guard_hint_lbl, lv_color_hex(COL_TEXT_MUTED), 0);

    s_shield_row = lv_obj_create(guard_card);
    lv_obj_set_size(s_shield_row, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(s_shield_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_shield_row, 0, 0);
    lv_obj_set_style_pad_all(s_shield_row, 0, 0);
    lv_obj_set_scrollable(s_shield_row, false);

    lv_obj_t *sh_lbl = lv_label_create(s_shield_row);
    lv_obj_set_style_text_font(sh_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sh_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(sh_lbl, "Shield >=");
    lv_obj_align(sh_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sh_plus = create_step_btn(s_shield_row, 44, 34, "+5", on_shield_step_clicked, 5);
    lv_obj_align(sh_plus, LV_ALIGN_RIGHT_MID, 0, 0);

    s_shield_val_lbl = lv_label_create(s_shield_row);
    lv_obj_set_style_text_font(s_shield_val_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_shield_val_lbl, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_label_set_text(s_shield_val_lbl, "80%+");
    lv_obj_align(s_shield_val_lbl, LV_ALIGN_RIGHT_MID, -56, 0);

    lv_obj_t *sh_minus = create_step_btn(s_shield_row, 44, 34, "-5", on_shield_step_clicked, -5);
    lv_obj_align(sh_minus, LV_ALIGN_RIGHT_MID, -110, 0);
}

static void build_buttons_tab(lv_obj_t *panel) {
    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_MUTED), 0);
    lv_label_set_text(hint, "Tap a row to cycle hardware button action");

    for (int i = 0; i < BTN_SLOT_COUNT; ++i) {
        lv_obj_t *row = lv_btn_create(panel);
        lv_obj_set_size(row, lv_pct(100), 46);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_SURFACE_CARD), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER_CARD), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, on_btn_row_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *slot_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(slot_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(slot_lbl, lv_color_hex(COL_TEXT_CHIP), 0);
        lv_label_set_text(slot_lbl, app_prefs_btn_slot_name((app_btn_slot_t)i));
        lv_obj_align(slot_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x241806), 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_set_style_pad_hor(badge, 10, 0);
        lv_obj_set_style_pad_ver(badge, 4, 0);
        lv_obj_set_clickable(badge, false);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_t *act_lbl = lv_label_create(badge);
        lv_obj_set_style_text_font(act_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(act_lbl, lv_color_hex(COL_STATE_LISTEN), 0);
        lv_label_set_text(act_lbl, "Talk / Mute");
        s_btn_act_lbls[i] = act_lbl;
    }

    lv_obj_t *rst_btn = lv_btn_create(panel);
    lv_obj_set_size(rst_btn, lv_pct(100), 38);
    lv_obj_set_style_radius(rst_btn, 12, 0);
    lv_obj_set_style_bg_color(rst_btn, lv_color_hex(COL_SURFACE_BTN), 0);
    lv_obj_set_style_border_color(rst_btn, lv_color_hex(COL_BORDER_CARD), 0);
    lv_obj_set_style_border_width(rst_btn, 1, 0);
    lv_obj_set_style_shadow_width(rst_btn, 0, 0);
    lv_obj_add_event_cb(rst_btn, on_btn_reset_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rst_lbl = lv_label_create(rst_btn);
    lv_obj_set_style_text_font(rst_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rst_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(rst_lbl, "Reset Button Defaults");
    lv_obj_center(rst_lbl);
}

static void build_settings_modal(lv_obj_t *parent) {
    static const char *const kTabNames[2] = {"Audio & Guard", "Buttons"};

    s_settings_modal = lv_obj_create(parent);
    lv_obj_set_size(s_settings_modal, SCREEN_W, SCREEN_H);
    lv_obj_align(s_settings_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_settings_modal, lv_color_hex(COL_SURFACE_SHEET), 0);
    lv_obj_set_style_bg_opa(s_settings_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings_modal, 0, 0);
    lv_obj_set_style_radius(s_settings_modal, 0, 0);
    lv_obj_set_style_pad_all(s_settings_modal, 16, 0);
    lv_obj_set_style_pad_row(s_settings_modal, 12, 0);
    lv_obj_set_flex_flow(s_settings_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_settings_modal, false);
    lv_obj_set_hidden(s_settings_modal, true);

    /* Sheet Header */
    lv_obj_t *hdr = lv_obj_create(s_settings_modal);
    lv_obj_set_size(hdr, lv_pct(100), 24);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_scrollable(hdr, false);

    lv_obj_t *title = lv_label_create(hdr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_label_set_text(title, "Device Settings");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *nvs_tag = lv_label_create(hdr);
    lv_obj_set_style_text_font(nvs_tag, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(nvs_tag, lv_color_hex(COL_TEXT_MUTED), 0);
    lv_label_set_text(nvs_tag, "NVS Saved");
    lv_obj_align(nvs_tag, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 2-Tab Bar */
    lv_obj_t *tab_bar = lv_obj_create(s_settings_modal);
    lv_obj_set_size(tab_bar, lv_pct(100), 42);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(COL_SURFACE_TABS), 0);
    lv_obj_set_style_border_color(tab_bar, lv_color_hex(COL_BORDER_CARD), 0);
    lv_obj_set_style_border_width(tab_bar, 1, 0);
    lv_obj_set_style_radius(tab_bar, 14, 0);
    lv_obj_set_style_pad_all(tab_bar, 4, 0);
    lv_obj_set_style_pad_column(tab_bar, 6, 0);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(tab_bar, false);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *b = lv_btn_create(tab_bar);
        lv_obj_set_height(b, 32);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, on_tab_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(lbl, kTabNames[i]);
        lv_obj_center(lbl);
        s_tab_btns[i] = b;
        s_tab_lbls[i] = lbl;
    }

    /* Tab Content Area */
    lv_obj_t *tab_body = lv_obj_create(s_settings_modal);
    lv_obj_set_width(tab_body, lv_pct(100));
    lv_obj_set_flex_grow(tab_body, 1);
    lv_obj_set_style_bg_opa(tab_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_body, 0, 0);
    lv_obj_set_style_pad_all(tab_body, 0, 0);
    lv_obj_set_scrollable(tab_body, false);

    for (int i = 0; i < 2; ++i) {
        s_tab_panels[i] = create_tab_panel(tab_body);
    }
    build_audio_tab(s_tab_panels[0]);
    build_buttons_tab(s_tab_panels[1]);

    /* Done Pill */
    lv_obj_t *done_btn = lv_btn_create(s_settings_modal);
    lv_obj_set_size(done_btn, lv_pct(100), 44);
    lv_obj_set_style_radius(done_btn, 22, 0);
    lv_obj_set_style_bg_color(done_btn, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_obj_set_style_shadow_width(done_btn, 0, 0);
    lv_obj_add_event_cb(done_btn, on_settings_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_lbl = lv_label_create(done_btn);
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(0x090908), 0);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_center(done_lbl);
}

static void build_confirm_modal(lv_obj_t *parent) {
    s_confirm_modal = lv_obj_create(parent);
    lv_obj_set_size(s_confirm_modal, SCREEN_W, SCREEN_H);
    lv_obj_align(s_confirm_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_confirm_modal, lv_color_hex(COL_BLACK), 0);
    lv_obj_set_style_bg_opa(s_confirm_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_confirm_modal, 0, 0);
    lv_obj_set_style_radius(s_confirm_modal, 0, 0);
    lv_obj_set_style_pad_all(s_confirm_modal, 20, 0);
    lv_obj_set_scrollable(s_confirm_modal, false);
    lv_obj_set_hidden(s_confirm_modal, true);

    lv_obj_t *card = lv_obj_create(s_confirm_modal);
    lv_obj_set_size(card, 328, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_SURFACE_PILL), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x33302B), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *badge = lv_label_create(card);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(badge, lv_color_hex(COL_STATE_TOOL), 0);
    lv_label_set_text(badge, "HARNESS TOOL PERMISSION");

    s_confirm_title = lv_label_create(card);
    lv_obj_set_width(s_confirm_title, lv_pct(100));
    lv_label_set_long_mode(s_confirm_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_confirm_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_confirm_title, lv_color_hex(COL_TEXT_BRIGHT), 0);

    s_confirm_body = lv_label_create(card);
    lv_obj_set_width(s_confirm_body, lv_pct(100));
    lv_label_set_long_mode(s_confirm_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_confirm_body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_confirm_body, lv_color_hex(COL_TEXT_DIM), 0);

    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_set_size(btn_row, lv_pct(100), 46);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_top(btn_row, 4, 0);
    lv_obj_set_style_pad_column(btn_row, 10, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(btn_row, false);

    lv_obj_t *deny_btn = lv_btn_create(btn_row);
    lv_obj_set_height(deny_btn, 44);
    lv_obj_set_flex_grow(deny_btn, 1);
    lv_obj_set_style_bg_color(deny_btn, lv_color_hex(0x262422), 0);
    lv_obj_set_style_radius(deny_btn, 14, 0);
    lv_obj_set_style_shadow_width(deny_btn, 0, 0);
    lv_obj_add_event_cb(deny_btn, on_confirm_choice, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *dl = lv_label_create(deny_btn);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(COL_TEXT_BODY), 0);
    lv_label_set_text(dl, "Deny");
    lv_obj_center(dl);

    lv_obj_t *allow_btn = lv_btn_create(btn_row);
    lv_obj_set_height(allow_btn, 44);
    lv_obj_set_flex_grow(allow_btn, 1);
    lv_obj_set_style_bg_color(allow_btn, lv_color_hex(COL_STATE_SPEAK), 0);
    lv_obj_set_style_radius(allow_btn, 14, 0);
    lv_obj_set_style_shadow_width(allow_btn, 0, 0);
    lv_obj_add_event_cb(allow_btn, on_confirm_choice, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *al = lv_label_create(allow_btn);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(al, lv_color_hex(0x041D13), 0);
    lv_label_set_text(al, "Allow");
    lv_obj_center(al);
}

/* ==================== PUBLIC INIT & HOOKS ==================== */
esp_err_t ui_app_init(void) {
    s_snap_mux = xSemaphoreCreateMutex();
    if (!s_snap_mux) return ESP_ERR_NO_MEM;

    s_op_queue = (ui_feed_op_t *)heap_caps_calloc(
        FEED_OP_QUEUE_CAP,
        sizeof(ui_feed_op_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_op_queue) return ESP_ERR_NO_MEM;

    memset(s_turns, 0, sizeof(s_turns));
    for (int i = 0; i < MAX_FEED_TURNS; ++i) {
        s_turns[i].user_buf  = (char *)heap_caps_calloc(1, USER_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_turns[i].agent_buf = (char *)heap_caps_calloc(1, AGENT_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_turns[i].user_buf || !s_turns[i].agent_buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
        s_last_bar_h[i] = 3;
    }

    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = APP_STATE_BOOTING;
    snprintf(s_snap.agent_name, sizeof(s_snap.agent_name), "%s", APP_AGENT_NAME);
    snprintf(s_snap.agent_role, sizeof(s_snap.agent_role), "%s", APP_AGENT_ROLE);
    snprintf(s_snap.status_text, sizeof(s_snap.status_text), "Initializing hardware...");
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    s_snap.vol_pct        = prefs.speaker_volume_pct;
    s_snap.guard_mode     = prefs.echo_guard_mode;
    s_snap.shield_vol_pct = prefs.auto_shield_vol_pct;
    s_snap.dirty_state    = true;
    s_snap.dirty_agent    = true;
    s_snap.dirty_telem    = true;
    s_snap.dirty_audio    = true;
    s_snap.dirty_mute     = true;
    s_last_activity_us    = esp_timer_get_time();

    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "Failed locking LVGL display");
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BLACK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);
    lv_obj_add_event_cb(scr, on_screen_any_touch, LV_EVENT_PRESSED, NULL);

    /* 1. Living Top Edge Aura: Lightweight vertical gradient wash + 4px light bar */
    s_edge_wash = lv_obj_create(scr);
    lv_obj_set_size(s_edge_wash, SCREEN_W, 96);
    lv_obj_align(s_edge_wash, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_edge_wash, lv_color_hex(COL_STATE_CONNECT), 0);
    lv_obj_set_style_bg_opa(s_edge_wash, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(s_edge_wash, 32, 0);
    lv_obj_set_style_bg_grad_color(s_edge_wash, lv_color_hex(COL_BLACK), 0);
    lv_obj_set_style_bg_grad_opa(s_edge_wash, LV_OPA_0, 0);
    lv_obj_set_style_bg_grad_dir(s_edge_wash, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(s_edge_wash, 0, 0);
    lv_obj_set_style_bg_grad_stop(s_edge_wash, 230, 0);
    lv_obj_set_style_border_width(s_edge_wash, 0, 0);
    lv_obj_set_style_radius(s_edge_wash, 0, 0);
    lv_obj_set_clickable(s_edge_wash, false);
    lv_obj_set_scrollable(s_edge_wash, false);

    s_aura_track = lv_obj_create(scr);
    lv_obj_set_size(s_aura_track, 316, 4);
    lv_obj_align(s_aura_track, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_aura_track, lv_color_hex(COL_SURFACE_PILL), 0);
    lv_obj_set_style_bg_opa(s_aura_track, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_aura_track, 0, 0);
    lv_obj_set_style_radius(s_aura_track, 4, 0);
    lv_obj_set_style_pad_all(s_aura_track, 0, 0);
    lv_obj_set_style_clip_corner(s_aura_track, true, 0);
    lv_obj_set_clickable(s_aura_track, false);
    lv_obj_set_scrollable(s_aura_track, false);

    s_aura_fill = lv_obj_create(s_aura_track);
    lv_obj_set_size(s_aura_fill, 316, 4);
    lv_obj_set_pos(s_aura_fill, 0, 0);
    lv_obj_set_style_bg_color(s_aura_fill, lv_color_hex(COL_STATE_CONNECT), 0);
    lv_obj_set_style_bg_opa(s_aura_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_aura_fill, 0, 0);
    lv_obj_set_style_radius(s_aura_fill, 4, 0);
    lv_obj_set_clickable(s_aura_fill, false);

    /* 2. Compact Header (y = 0..54) */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, SCREEN_W, 54);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_left(hdr, 16, 0);
    lv_obj_set_style_pad_right(hdr, 14, 0);
    lv_obj_set_style_pad_top(hdr, 8, 0);
    lv_obj_set_style_pad_bottom(hdr, 2, 0);
    lv_obj_set_scrollable(hdr, false);

    lv_obj_t *hdr_left = lv_obj_create(hdr);
    lv_obj_set_size(hdr_left, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_max_width(hdr_left, 198, 0);
    lv_obj_align(hdr_left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(hdr_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr_left, 0, 0);
    lv_obj_set_style_pad_all(hdr_left, 0, 0);
    lv_obj_set_style_pad_column(hdr_left, 7, 0);
    lv_obj_set_flex_flow(hdr_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(hdr_left, false);

    s_state_dot = lv_obj_create(hdr_left);
    lv_obj_set_size(s_state_dot, 8, 8);
    lv_obj_set_style_radius(s_state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_state_dot, 0, 0);
    lv_obj_set_style_bg_color(s_state_dot, lv_color_hex(COL_STATE_CONNECT), 0);

    s_agent_lbl = lv_label_create(hdr_left);
    lv_obj_set_style_text_font(s_agent_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_agent_lbl, lv_color_hex(COL_TEXT_BRIGHT), 0);
    lv_label_set_text(s_agent_lbl, APP_AGENT_NAME);

    s_state_lbl = lv_label_create(hdr_left);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(COL_STATE_CONNECT), 0);
    lv_label_set_text(s_state_lbl, "BOOTING");

    /* Right group: Enlarged Invisible Volume Hitbox (94x48) + Battery */
    s_batt_lbl = lv_label_create(hdr);
    lv_obj_set_width(s_batt_lbl, 44);
    lv_obj_set_style_text_align(s_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_batt_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_batt_lbl, "USB");
    lv_obj_align(s_batt_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    s_vol_hit = lv_obj_create(hdr);
    lv_obj_set_size(s_vol_hit, 94, 48);
    lv_obj_align(s_vol_hit, LV_ALIGN_RIGHT_MID, -46, 0);
    lv_obj_set_style_bg_opa(s_vol_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_vol_hit, 0, 0);
    lv_obj_set_style_pad_all(s_vol_hit, 0, 0);
    lv_obj_set_scrollable(s_vol_hit, false);
    lv_obj_set_clickable(s_vol_hit, true);
    lv_obj_set_ext_click_area(s_vol_hit, 4);
    lv_obj_add_event_cb(s_vol_hit, on_open_audio_settings, LV_EVENT_CLICKED, NULL);

    s_vol_pill = lv_obj_create(s_vol_hit);
    lv_obj_set_size(s_vol_pill, LV_SIZE_CONTENT, 24);
    lv_obj_align(s_vol_pill, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(s_vol_pill, lv_color_hex(COL_SURFACE_PILL), 0);
    lv_obj_set_style_border_color(s_vol_pill, lv_color_hex(COL_BORDER_PILL), 0);
    lv_obj_set_style_border_width(s_vol_pill, 1, 0);
    lv_obj_set_style_radius(s_vol_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_vol_pill, 9, 0);
    lv_obj_set_style_pad_ver(s_vol_pill, 0, 0);
    lv_obj_set_style_pad_column(s_vol_pill, 5, 0);
    lv_obj_set_flex_flow(s_vol_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_vol_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_vol_pill, false);
    lv_obj_set_clickable(s_vol_pill, false);

    s_vol_lbl = lv_label_create(s_vol_pill);
    lv_obj_set_style_text_font(s_vol_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(COL_TEXT_BODY), 0);
    lv_label_set_text_fmt(s_vol_lbl, LV_SYMBOL_VOLUME_MAX " %d%%", s_snap.vol_pct);

    s_shield_dot = lv_obj_create(s_vol_pill);
    lv_obj_set_size(s_shield_dot, 5, 5);
    lv_obj_set_style_radius(s_shield_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_shield_dot, 0, 0);
    lv_obj_set_style_bg_color(s_shield_dot, lv_color_hex(COL_STATE_LISTEN), 0);
    lv_obj_set_clickable(s_shield_dot, false);
    lv_obj_set_hidden(s_shield_dot, true);

    /* 3. Sub-Header Spectrum & Telemetry Strip (y = 54..78) */
    lv_obj_t *wave_row = lv_obj_create(scr);
    lv_obj_set_size(wave_row, SCREEN_W, 24);
    lv_obj_align(wave_row, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_opa(wave_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_row, 0, 0);
    lv_obj_set_style_pad_hor(wave_row, 18, 0);
    lv_obj_set_style_pad_ver(wave_row, 0, 0);
    lv_obj_set_scrollable(wave_row, false);

    lv_obj_t *wave_box = lv_obj_create(wave_row);
    lv_obj_set_size(wave_box, 90, 20);
    lv_obj_align(wave_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(wave_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_box, 0, 0);
    lv_obj_set_style_pad_all(wave_box, 0, 0);
    lv_obj_set_style_pad_column(wave_box, 3, 0);
    lv_obj_set_flex_flow(wave_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(wave_box, false);

    for (int i = 0; i < WAVE_BAR_COUNT; ++i) {
        lv_obj_t *bar = lv_obj_create(wave_box);
        lv_obj_set_size(bar, 3, 3);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_STATE_CONNECT), 0);
        lv_obj_set_clickable(bar, false);
        s_wave_bars[i] = bar;
    }

    lv_obj_t *meta_box = lv_obj_create(wave_row);
    lv_obj_set_size(meta_box, LV_SIZE_CONTENT, 20);
    lv_obj_align(meta_box, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(meta_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta_box, 0, 0);
    lv_obj_set_style_pad_all(meta_box, 0, 0);
    lv_obj_set_style_pad_column(meta_box, 6, 0);
    lv_obj_set_flex_flow(meta_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(meta_box, false);

    s_muted_pill = lv_obj_create(meta_box);
    lv_obj_set_size(s_muted_pill, LV_SIZE_CONTENT, 16);
    lv_obj_set_style_bg_color(s_muted_pill, lv_color_hex(0x3B1214), 0);
    lv_obj_set_style_border_width(s_muted_pill, 0, 0);
    lv_obj_set_style_radius(s_muted_pill, 6, 0);
    lv_obj_set_style_pad_hor(s_muted_pill, 6, 0);
    lv_obj_set_style_pad_ver(s_muted_pill, 0, 0);
    lv_obj_set_scrollable(s_muted_pill, false);
    lv_obj_set_hidden(s_muted_pill, true);
    lv_obj_t *m_lbl = lv_label_create(s_muted_pill);
    lv_obj_set_style_text_font(m_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(m_lbl, lv_color_hex(0xFCA5A5), 0);
    lv_label_set_text(m_lbl, "MUTED");
    lv_obj_center(m_lbl);

    s_meta_lbl = lv_label_create(meta_box);
    lv_obj_set_style_text_font(s_meta_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_meta_lbl, lv_color_hex(COL_TEXT_MUTED), 0);
    lv_label_set_text(s_meta_lbl, "Wi-Fi... " LV_SYMBOL_BULLET " Guard Auto");

    /* 4. Main Stage — Multi-Turn Scrollable Transcript Feed (y = 78..374, height = 296) */
    s_stage = lv_obj_create(scr);
    lv_obj_set_size(s_stage, SCREEN_W, 296);
    lv_obj_align(s_stage, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_stage, 0, 0);
    lv_obj_set_style_pad_all(s_stage, 0, 0);
    lv_obj_set_style_pad_row(s_stage, 4, 0);
    lv_obj_set_flex_flow(s_stage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_stage, false);

    /* Inline Violet Tool-Calling Progress Banner */
    s_tool_banner = lv_obj_create(s_stage);
    lv_obj_set_size(s_tool_banner, 336, 44);
    lv_obj_set_style_margin_left(s_tool_banner, 16, 0);
    lv_obj_set_style_margin_top(s_tool_banner, 2, 0);
    lv_obj_set_style_bg_color(s_tool_banner, lv_color_hex(0x181128), 0);
    lv_obj_set_style_border_color(s_tool_banner, lv_color_hex(COL_STATE_TOOL), 0);
    lv_obj_set_style_border_opa(s_tool_banner, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_tool_banner, 1, 0);
    lv_obj_set_style_radius(s_tool_banner, 13, 0);
    lv_obj_set_style_pad_hor(s_tool_banner, 12, 0);
    lv_obj_set_style_pad_ver(s_tool_banner, 7, 0);
    lv_obj_set_scrollable(s_tool_banner, false);
    lv_obj_set_hidden(s_tool_banner, true);

    s_tool_name_lbl = lv_label_create(s_tool_banner);
    lv_obj_set_width(s_tool_name_lbl, 220);
    lv_label_set_long_mode(s_tool_name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_tool_name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_tool_name_lbl, lv_color_hex(0xDDD6FE), 0);
    lv_label_set_text(s_tool_name_lbl, "tool");
    lv_obj_align(s_tool_name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *tool_run_lbl = lv_label_create(s_tool_banner);
    lv_obj_set_style_text_font(tool_run_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tool_run_lbl, lv_color_hex(COL_STATE_TOOL), 0);
    lv_label_set_text(tool_run_lbl, "RUNNING");
    lv_obj_align(tool_run_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *tool_track = lv_obj_create(s_tool_banner);
    lv_obj_set_size(tool_track, lv_pct(100), 4);
    lv_obj_align(tool_track, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(tool_track, lv_color_hex(0x2B1D48), 0);
    lv_obj_set_style_border_width(tool_track, 0, 0);
    lv_obj_set_style_radius(tool_track, 4, 0);
    lv_obj_set_style_pad_all(tool_track, 0, 0);
    lv_obj_set_style_clip_corner(tool_track, true, 0);
    lv_obj_set_scrollable(tool_track, false);

    s_tool_bar_fill = lv_obj_create(tool_track);
    lv_obj_set_size(s_tool_bar_fill, 110, 4);
    lv_obj_set_pos(s_tool_bar_fill, 0, 0);
    lv_obj_set_style_bg_color(s_tool_bar_fill, lv_color_hex(COL_STATE_TOOL), 0);
    lv_obj_set_style_border_width(s_tool_bar_fill, 0, 0);
    lv_obj_set_style_radius(s_tool_bar_fill, 4, 0);

    /* Scrollable Multi-Turn Feed */
    s_feed = lv_obj_create(s_stage);
    lv_obj_set_width(s_feed, SCREEN_W);
    lv_obj_set_flex_grow(s_feed, 1);
    lv_obj_set_style_bg_opa(s_feed, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_feed, 0, 0);
    lv_obj_set_style_pad_left(s_feed, 20, 0);
    lv_obj_set_style_pad_right(s_feed, 20, 0);
    lv_obj_set_style_pad_top(s_feed, 8, 0);
    lv_obj_set_style_pad_bottom(s_feed, 26, 0);
    lv_obj_set_style_pad_row(s_feed, 14, 0);
    lv_obj_set_flex_flow(s_feed, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_feed, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_feed, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_feed, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_feed, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_feed, lv_color_hex(COL_TEXT_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_feed, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_feed, 4, LV_PART_SCROLLBAR);

    lv_obj_add_event_cb(s_feed, on_feed_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_feed, on_feed_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_feed, on_feed_event, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_feed, on_feed_event, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(s_feed, on_feed_event, LV_EVENT_GESTURE, NULL);

    /* Idle Hero shown when 0 turns exist */
    s_idle_hero = lv_obj_create(s_feed);
    lv_obj_set_width(s_idle_hero, lv_pct(100));
    lv_obj_set_height(s_idle_hero, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_idle_hero, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_idle_hero, 0, 0);
    lv_obj_set_style_pad_all(s_idle_hero, 0, 0);
    lv_obj_set_style_pad_top(s_idle_hero, 40, 0);
    lv_obj_set_style_pad_row(s_idle_hero, 10, 0);
    lv_obj_set_flex_flow(s_idle_hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_idle_hero, false);
    lv_obj_set_clickable(s_idle_hero, false);

    s_idle_title = lv_label_create(s_idle_hero);
    lv_obj_set_width(s_idle_title, lv_pct(100));
    lv_label_set_long_mode(s_idle_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_idle_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_idle_title, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_label_set_text(s_idle_title, "Joining Wi-Fi...");

    s_idle_sub = lv_label_create(s_idle_hero);
    lv_obj_set_width(s_idle_sub, lv_pct(100));
    lv_label_set_long_mode(s_idle_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_idle_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_idle_sub, lv_color_hex(COL_TEXT_MUTED), 0);
    lv_label_set_text(s_idle_sub, "Initializing hardware...");

    /* Floating "↓ Live" / "↓ Bottom" Jump Pill */
    s_jump_pill = lv_btn_create(s_stage);
    lv_obj_set_floating(s_jump_pill, true);
    lv_obj_set_size(s_jump_pill, LV_SIZE_CONTENT, 30);
    lv_obj_align(s_jump_pill, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_jump_pill, lv_color_hex(COL_SURFACE_PILL), 0);
    lv_obj_set_style_bg_opa(s_jump_pill, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_jump_pill, lv_color_hex(COL_STATE_SPEAK), 0);
    lv_obj_set_style_border_width(s_jump_pill, 1, 0);
    lv_obj_set_style_radius(s_jump_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_jump_pill, 14, 0);
    lv_obj_set_style_pad_ver(s_jump_pill, 0, 0);
    lv_obj_set_style_pad_column(s_jump_pill, 6, 0);
    lv_obj_set_style_shadow_width(s_jump_pill, 0, 0);
    lv_obj_set_flex_flow(s_jump_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_jump_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_jump_pill, on_jump_live_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_jump_pill, true);

    s_jump_dot = lv_obj_create(s_jump_pill);
    lv_obj_set_size(s_jump_dot, 6, 6);
    lv_obj_set_style_radius(s_jump_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_jump_dot, 0, 0);
    lv_obj_set_style_bg_color(s_jump_dot, lv_color_hex(COL_STATE_SPEAK), 0);
    lv_obj_set_clickable(s_jump_dot, false);

    s_jump_lbl = lv_label_create(s_jump_pill);
    lv_obj_set_style_text_font(s_jump_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_jump_lbl, lv_color_hex(COL_TEXT_BRIGHT), 0);
    lv_label_set_text(s_jump_lbl, LV_SYMBOL_DOWN " Live");

    /* 5. Unified Bottom Capsule Dock (344x54px, centered, 10px bottom margin, radius=27) */
    lv_obj_t *dock = lv_obj_create(scr);
    lv_obj_set_size(dock, 344, 54);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(dock, lv_color_hex(COL_SURFACE_DOCK), 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(COL_BORDER_DOCK), 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_radius(dock, 27, 0);
    lv_obj_set_style_pad_all(dock, 4, 0);
    lv_obj_set_style_pad_column(dock, 6, 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(dock, false);

    s_mute_btn = lv_btn_create(dock);
    lv_obj_set_size(s_mute_btn, 82, 44);
    lv_obj_set_style_radius(s_mute_btn, 22, 0);
    lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(COL_SURFACE_BTN), 0);
    lv_obj_set_style_border_width(s_mute_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_mute_btn, 0, 0);
    lv_obj_add_event_cb(s_mute_btn, on_mute_clicked, LV_EVENT_CLICKED, NULL);
    s_mute_lbl = lv_label_create(s_mute_btn);
    lv_obj_set_style_text_font(s_mute_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mute_lbl, lv_color_hex(COL_TEXT_BODY), 0);
    lv_label_set_text(s_mute_lbl, LV_SYMBOL_AUDIO " Mute");
    lv_obj_center(s_mute_lbl);

    s_action_btn = lv_btn_create(dock);
    lv_obj_set_height(s_action_btn, 44);
    lv_obj_set_flex_grow(s_action_btn, 1);
    lv_obj_set_style_radius(s_action_btn, 22, 0);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(COL_STATE_CONNECT), 0);
    lv_obj_set_style_border_width(s_action_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_action_btn, 0, 0);
    lv_obj_add_event_cb(s_action_btn, on_action_clicked, LV_EVENT_CLICKED, NULL);
    s_action_lbl = lv_label_create(s_action_btn);
    lv_obj_set_style_text_font(s_action_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0x090908), 0);
    lv_label_set_text(s_action_lbl, "Start Talking");
    lv_obj_center(s_action_lbl);

    s_right_btn = lv_btn_create(dock);
    lv_obj_set_size(s_right_btn, 82, 44);
    lv_obj_set_style_radius(s_right_btn, 22, 0);
    lv_obj_set_style_bg_color(s_right_btn, lv_color_hex(COL_SURFACE_BTN), 0);
    lv_obj_set_style_border_width(s_right_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_right_btn, 0, 0);
    lv_obj_add_event_cb(s_right_btn, on_right_clicked, LV_EVENT_CLICKED, NULL);
    s_right_lbl = lv_label_create(s_right_btn);
    lv_obj_set_style_text_font(s_right_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_right_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_right_lbl, LV_SYMBOL_SETTINGS " Setup");
    lv_obj_center(s_right_lbl);

    /* 6. Modals & Toast */
    build_settings_modal(scr);
    build_confirm_modal(scr);

    s_toast_pill = lv_obj_create(scr);
    lv_obj_set_size(s_toast_pill, LV_SIZE_CONTENT, 28);
    lv_obj_align(s_toast_pill, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_radius(s_toast_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_toast_pill, lv_color_hex(COL_SURFACE_BTN), 0);
    lv_obj_set_style_bg_opa(s_toast_pill, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_toast_pill, lv_color_hex(COL_BORDER_TOAST), 0);
    lv_obj_set_style_border_width(s_toast_pill, 1, 0);
    lv_obj_set_style_pad_hor(s_toast_pill, 14, 0);
    lv_obj_set_style_pad_ver(s_toast_pill, 0, 0);
    lv_obj_set_scrollable(s_toast_pill, false);
    lv_obj_set_clickable(s_toast_pill, false);
    lv_obj_set_hidden(s_toast_pill, true);
    s_toast_lbl = lv_label_create(s_toast_pill);
    lv_obj_set_style_text_font(s_toast_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(COL_TEXT_PRIMARY), 0);
    lv_obj_center(s_toast_lbl);

    s_shutdown_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_shutdown_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_shutdown_lbl, lv_color_hex(COL_STATE_LISTEN), 0);
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
    const app_state_t prev = s_snap.state;
    s_snap.state           = state;
    s_snap.dirty_state     = true;
    if (status_text) {
        snprintf(s_snap.status_text, sizeof(s_snap.status_text), "%s", status_text);
    }
    if (state == APP_STATE_INTERRUPTED && prev != APP_STATE_INTERRUPTED) {
        s_snap.flash_until_us = esp_timer_get_time() + 400000LL;
        enqueue_op_locked(UI_OP_MARK_INTERRUPTED, NULL, 0);
    }
    if (state != APP_STATE_TOOL_CALLING && s_snap.tool_running) {
        s_snap.tool_running      = false;
        s_snap.dirty_tool_banner = true;
    }
    xSemaphoreGive(s_snap_mux);
}

void ui_app_set_agent_info(
    const char *name,
    const char *role,
    uint32_t    accent_rgb,
    bool        apply_accent,
    const char *orb_style) {
    /* Pure semantic state colors only: custom orb color/style are ignored */
    (void)accent_rgb;
    (void)apply_accent;
    (void)orb_style;
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    if (name && name[0] != '\0') {
        snprintf(s_snap.agent_name, sizeof(s_snap.agent_name), "%s", name);
    }
    if (role && role[0] != '\0') {
        snprintf(s_snap.agent_role, sizeof(s_snap.agent_role), "%s", role);
    }
    s_snap.dirty_agent = true;
    xSemaphoreGive(s_snap_mux);
}

void ui_app_set_caption(const char *text, bool append) {
    if (!s_snap_mux || !text) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    if (!append) {
        enqueue_op_locked(UI_OP_AGENT_NEW, NULL, 0);
    }
    if (text[0] != '\0') {
        enqueue_op_locked(UI_OP_AGENT_DELTA, text, 0);
    }
    xSemaphoreGive(s_snap_mux);
}

void ui_app_clear_caption(void) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_op_head    = 0;
    s_op_tail    = 0;
    s_op_count   = 0;
    enqueue_op_locked(UI_OP_CLEAR_FEED, NULL, 0);
    xSemaphoreGive(s_snap_mux);
}

void ui_app_add_user_turn(const char *text, uint32_t duration_ms) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    enqueue_op_locked(UI_OP_USER_TURN, text, duration_ms);
    xSemaphoreGive(s_snap_mux);
}

void ui_app_set_tool_status(const char *name, const char *state) {
    if (!s_snap_mux) return;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    const bool running       = (state && strcmp(state, "running") == 0);
    s_snap.tool_running      = running;
    s_snap.dirty_tool_banner = true;
    if (running) {
        snprintf(s_snap.tool_name, sizeof(s_snap.tool_name), "%s", (name && name[0]) ? name : "tool");
    }
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
