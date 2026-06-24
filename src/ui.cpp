// UI: tileview (radar / list / stats) + tap-to-inspect vessel detail card.
// Pure LVGL, portable. Taps hit-test via radar::hitTest; selection lives in radar.
// Adapted from the aircraft Capsule Radar: aircraft -> vessel, altitude -> nav status,
// aircraft photo / route lookup removed (AIS already carries name + destination).
#include "ui.h"
#include "radar_view.h"
#include "config.h"
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define UI_GREEN lv_color_hex(0x1DFF86)
#define UI_INK   lv_color_hex(0xEAFFF3)
#define UI_SOFT  lv_color_hex(0x9AFFC8)
#define UI_DIM   lv_color_hex(0x5F7A6C)
#define UI_PANEL lv_color_hex(0x0C160F)
#define UI_ALERT lv_color_hex(0xFF5A3C)

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileList = nullptr, *s_tileStats = nullptr;
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardBody = nullptr, *s_cardSub = nullptr;
static lv_obj_t *s_hudWifi = nullptr, *s_hudCount = nullptr, *s_hudClock = nullptr, *s_hudBatt = nullptr, *s_hudDate = nullptr;
static lv_obj_t *s_hudBars[4] = { nullptr, nullptr, nullptr, nullptr };   // WiFi signal-strength bars
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_statsLbl = nullptr;
static lv_obj_t *s_statsNet = nullptr;
static lv_obj_t *s_hudGps   = nullptr;   // HUD satellite icon (hidden unless GPS auto-location is on)
static lv_obj_t *s_statsGps = nullptr;   // Stats view GPS status line

// --------------------------------------------------------------------- formatting
// Fold Latin-1 accents / drop any other non-ASCII so the Montserrat font never hits a
// missing glyph (which renders as an empty box). Belt-and-suspenders for card text.
static void fold_ascii(char *s) {
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if (*p == 0xC3 && p[1]) {                       // Latin-1 Supplement (U+00C0..U+00FF)
            const unsigned char d = p[1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else                             r = '?';
            *o++ = r; p += 2; continue;
        }
        ++p;                                            // skip other multibyte lead + continuation
        while (*p >= 0x80 && *p < 0xC0) ++p;
    }
    *o = 0;
}

// ----------------------------------------------------------------- detail card
static void refresh_card(void) {
    ShipInfo in;
    if (!radar::selected(in)) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    // reference-style readout: name on top, Dist + SOG (Knots) below, a dim type/status line.
    char title[28];
    snprintf(title, sizeof(title), "%s", in.name[0] ? in.name : "(no name)");
    fold_ascii(title);
    lv_label_set_text(s_cardTitle, title);
    lv_obj_set_style_text_color(s_cardTitle, in.alert ? UI_ALERT : UI_INK, 0);

    char body[64];
    if (isnan(in.sogKt)) snprintf(body, sizeof(body), "Dist: %.1f nm\nSOG: -", in.distNm);
    else                 snprintf(body, sizeof(body), "Dist: %.1f nm\nSOG: %.1f Knots", in.distNm, in.sogKt);
    lv_label_set_text(s_cardBody, body);

    // dim secondary line: type · size · draught · destination · ETA (omit unknowns).
    // dest is folded separately; the rest is ASCII, so the bullet/arrow symbols survive.
    char dest[24]; snprintf(dest, sizeof(dest), "%s", in.dest); fold_ascii(dest);
    char sub[128];
    int p = snprintf(sub, sizeof(sub), "%s", in.type[0] ? in.type : "Vessel");
    if (in.lengthM > 0)
        p += snprintf(sub + p, sizeof(sub) - p, "  " LV_SYMBOL_BULLET "  %ux%u m", in.lengthM, in.beamM);
    if (!isnan(in.draughtM) && in.draughtM > 0.0f)
        p += snprintf(sub + p, sizeof(sub) - p, "  " LV_SYMBOL_BULLET "  %.1f m draught", (double)in.draughtM);
    if (dest[0] && strcmp(dest, "-") != 0)
        p += snprintf(sub + p, sizeof(sub) - p, "  " LV_SYMBOL_BULLET "  " LV_SYMBOL_RIGHT " %s", dest);
    else
        p += snprintf(sub + p, sizeof(sub) - p, "  " LV_SYMBOL_BULLET "  %s", in.status);
    if (in.eta[0])
        snprintf(sub + p, sizeof(sub) - p, "  " LV_SYMBOL_BULLET "  ETA %s", in.eta);
    lv_label_set_text(s_cardSub, sub);
}

// --------------------------------------------------------------------- input
static bool s_longPressed = false;
static int s_rangeIdx = -1;
static float s_rangeNm = RANGE_NM_DEFAULT;   // current display range (nm), for the stats view
static void (*s_rangeCb)(float) = nullptr;
static void (*s_colorCb)(int) = nullptr;
static lv_obj_t *s_zoomBtn = nullptr, *s_zoomLbl = nullptr;

void ui_set_range_cb(void (*cb)(float)) { s_rangeCb = cb; }
void ui_set_color_mode_cb(void (*cb)(int)) { s_colorCb = cb; }

static void zoom_cb(lv_event_t *e) {   // fires on PRESS (robust vs scroll-cancel on the tileview)
    (void)e;
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;      // debounce repeated/held presses
    last = now;
    if (!s_rangeCb) return;
    const int n = (int)(sizeof(RANGE_STEPS_NM) / sizeof(RANGE_STEPS_NM[0]));
    s_rangeIdx = (s_rangeIdx + 1) % n;
    s_rangeCb(RANGE_STEPS_NM[s_rangeIdx]);
}

void ui_set_range_nm(float nm) {
    s_rangeNm = nm;
    if (s_zoomLbl) {
        char b[20];
        snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %.0f nm", (double)nm);
        lv_label_set_text(s_zoomLbl, b);
    }
    int best = 0; float bd = 1e9f;                 // sync the cycle index to the shown range
    const int n = (int)(sizeof(RANGE_STEPS_NM) / sizeof(RANGE_STEPS_NM[0]));
    for (int i = 0; i < n; ++i) { float d = nm - RANGE_STEPS_NM[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    s_rangeIdx = best;
}

static void radar_press_cb(lv_event_t *e) { (void)e; s_longPressed = false; }

static void radar_longpress_cb(lv_event_t *e) {   // long-press cycles the visual theme
    (void)e;
    radar::cycleTheme();
    s_longPressed = true;
}

static void radar_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_longPressed) { s_longPressed = false; return; }   // ignore the click after a long-press
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    radar::select(radar::hitTest(p.x, p.y));   // hit -> select; miss -> clear
    refresh_card();
}

static void list_btn_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    const int idx = (int)(intptr_t)lv_obj_get_user_data(b);
    radar::select(idx);
    refresh_card();
    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);   // jump back to the radar
}

// ----------------------------------------------------------------- list/stats
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock) {
    int level;
    if      (!wifiUp)     level = 0;
    else if (rssi >= -55) level = 4;   // excellent
    else if (rssi >= -67) level = 3;   // good
    else if (rssi >= -75) level = 2;   // ok
    else                  level = 1;   // weak (connected but marginal)
    // colour: red = no WiFi, amber = connected but feed stale (no fresh data), white = healthy
    const lv_color_t col = !wifiUp ? UI_ALERT : (feedOk ? UI_INK : lv_color_hex(0xFFB23C));
    for (int i = 0; i < 4; ++i) {
        if (!s_hudBars[i]) continue;
        lv_obj_set_style_bg_color(s_hudBars[i], col, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], (i < level) ? LV_OPA_COVER : 45, 0);
    }
    if (s_hudClock && clock) lv_label_set_text(s_hudClock, clock);
}

void ui_set_battery(int pct, bool charging, bool present) {
    if (!s_hudBatt) return;
    if (!present || pct < 0) { lv_label_set_text(s_hudBatt, ""); return; }   // USB-only -> hide
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                      pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 35 ? LV_SYMBOL_BATTERY_2 :
                      pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s%d", charging ? LV_SYMBOL_CHARGE : "", sym, pct);
    lv_label_set_text(s_hudBatt, buf);
    lv_obj_set_style_text_color(s_hudBatt, (pct <= 15 && !charging) ? UI_ALERT : UI_INK, 0);
}

void ui_set_date(const char *date) {
    if (s_hudDate && date) lv_label_set_text(s_hudDate, date);
}

void ui_set_netinfo(const char *line) {
    if (s_statsNet && line) lv_label_set_text(s_statsNet, line);
}

// GPS indicator. state: 0 = off / no module (hidden), 1 = acquiring (amber), 2 = fix (green).
void ui_set_gps(int state, int sats) {
    if (state <= 0) {                                 // hidden when GPS auto-location is off
        if (s_hudGps)   lv_label_set_text(s_hudGps, "");
        if (s_statsGps) lv_label_set_text(s_statsGps, "");
        return;
    }
    const bool fix = (state >= 2);
    const lv_color_t col = fix ? UI_GREEN : lv_color_hex(0xFFB23C);   // amber while acquiring
    if (s_hudGps) {
        char b[16];
        snprintf(b, sizeof(b), LV_SYMBOL_GPS "%d", sats);
        lv_label_set_text(s_hudGps, b);
        lv_obj_set_style_text_color(s_hudGps, col, 0);
    }
    if (s_statsGps) {
        char s[40];
        if (fix) snprintf(s, sizeof(s), LV_SYMBOL_GPS " fix  " LV_SYMBOL_BULLET "  %d sats", sats);
        else     snprintf(s, sizeof(s), LV_SYMBOL_GPS " acquiring  (%d sats)", sats);
        lv_label_set_text(s_statsGps, s);
        lv_obj_set_style_text_color(s_statsGps, col, 0);
    }
}

// Rebuild the scrollable contact list. Costly (deletes+recreates LVGL buttons), so we
// only call it when the list tile is actually visible — not on every update.
static void build_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    const int n = radar::count();
    for (int i = 0; i < n; ++i) {
        ShipInfo in;
        radar::info(i, in);
        char spdS[12], name[14], txt[64];
        if (isnan(in.sogKt)) snprintf(spdS, sizeof(spdS), "  -");
        else                 snprintf(spdS, sizeof(spdS), "%4.1f", in.sogKt);
        snprintf(name, sizeof(name), "%s", in.name[0] ? in.name : "(no name)");
        fold_ascii(name);
        snprintf(txt, sizeof(txt), "%-12.12s %s kt %4.1f nm", name, spdS, in.distNm);
        lv_obj_t *b = lv_list_add_btn(s_list, NULL, txt);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(b, in.alert ? UI_ALERT : UI_SOFT, 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, list_btn_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void build_stats(void) {
    if (!s_statsLbl) return;
    const int n = radar::count();
    int alerts = 0, moving = 0;
    float nearest = 1e9f;
    char nearestName[16] = "-";
    for (int i = 0; i < n; ++i) {
        ShipInfo in;
        radar::info(i, in);
        if (in.alert) alerts++;
        if (!isnan(in.sogKt) && in.sogKt > 0.5f) moving++;
        if (in.distNm < nearest) {
            nearest = in.distNm;
            snprintf(nearestName, sizeof(nearestName), "%s", in.name[0] ? in.name : "-");
            fold_ascii(nearestName);
        }
    }
    char st[220];
    snprintf(st, sizeof(st),
             "Vessels    %d\n"
             "Moving     %d\n"
             "Nearest    %s\n"
             "           %.1f nm\n"
             "Alerts     %d\n"
             "Range      %.0f nm",
             n, moving, n ? nearestName : "-", (n ? nearest : 0.0f), alerts, (double)s_rangeNm);
    lv_label_set_text(s_statsLbl, st);
}

// Rebuild whichever of list/stats is currently on screen (called on update and on swipe).
static void refresh_active_tile(void) {
    if (!s_tv) return;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)  build_list();
    else if (act == s_tileStats) build_stats();
}

void ui_on_data_updated(void) {
    refresh_card();
    if (s_hudCount) {
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%d", radar::countInRange());
        lv_label_set_text(s_hudCount, cbuf);
    }
    refresh_active_tile();   // only the visible tile pays the rebuild cost
}

// ------------------------------------------------------------------- building
static lv_obj_t *make_tile_title(lv_obj_t *tile, const char *txt) {
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, UI_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

// A full-screen round panel that clips its content to the circle (for list/stats views).
static lv_obj_t *make_round_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 462, 462);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, UI_GREEN, 0);
    lv_obj_set_style_border_opa(p, 50, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void build_card(void) {
    s_card = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 272, 120);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 78);
    lv_obj_set_style_bg_color(s_card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, 225, 0);
    lv_obj_set_style_radius(s_card, 14, 0);
    lv_obj_set_style_border_color(s_card, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_card, 130, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // consume taps (don't deselect)
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_cardTitle = lv_label_create(s_card);            // vessel name (white, bold)
    lv_obj_set_style_text_font(s_cardTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_cardTitle, UI_INK, 0);
    lv_obj_align(s_cardTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cardBody = lv_label_create(s_card);             // Dist / SOG (green, like the reference)
    lv_obj_set_style_text_font(s_cardBody, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_cardBody, UI_GREEN, 0);
    lv_obj_align(s_cardBody, LV_ALIGN_TOP_LEFT, 0, 26);

    s_cardSub = lv_label_create(s_card);              // type · size · draught · dest · ETA (dim, wraps)
    lv_obj_set_style_text_font(s_cardSub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_cardSub, UI_DIM, 0);
    lv_obj_set_width(s_cardSub, 248);
    lv_label_set_long_mode(s_cardSub, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_cardSub, LV_ALIGN_TOP_LEFT, 0, 70);
}

void ui_show_view(int idx) {
    if (s_tv && idx >= 0 && idx <= 2) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_OFF);
}

// ------------------------------------------------------------------- splash
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void splash_del_cb(lv_anim_t *a) { lv_obj_del((lv_obj_t *)a->var); }

static void splash_dismiss_cb(lv_timer_t *t) {
    lv_obj_t *cont = (lv_obj_t *)t->user_data;
    lv_timer_del(t);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 600);
    lv_anim_set_ready_cb(&a, splash_del_cb);
    lv_anim_start(&a);
}

void ui_splash_show(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // concentric rings
    const lv_coord_t dia[3] = { 210, 142, 78 };
    const lv_opa_t   op[3]  = { 90, 120, 160 };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *r = lv_obj_create(cont);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, dia[i], dia[i]);
        lv_obj_align(r, LV_ALIGN_CENTER, 0, -8);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(r, UI_GREEN, 0);
        lv_obj_set_style_border_opa(r, op[i], 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
    // rotating sweep
    lv_obj_t *sweep = lv_spinner_create(cont, 1400, 55);
    lv_obj_set_size(sweep, 210, 210);
    lv_obj_align(sweep, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_arc_opa(sweep, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sweep, UI_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sweep, 4, LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "CAPSULE RADAR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 118);

    lv_obj_t *sub = lv_label_create(cont);
    lv_label_set_text(sub, "Live AIS ship radar");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, UI_SOFT, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 150);

    lv_timer_t *t = lv_timer_create(splash_dismiss_cb, 2200, cont);   // hold, then fade out
    lv_timer_set_repeat_count(t, 1);
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    s_tileRadar = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileList  = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    s_tileStats = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_LEFT);
    // Rebuild the list/stats with the latest data the moment they slide into view.
    lv_obj_add_event_cb(s_tv, [](lv_event_t *) { refresh_active_tile(); }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    radar::init(s_tileRadar);
    radar::setRangeLabelVisible(false);                     // the zoom button shows the range instead
    radar::setCompassVisible(false);                        // north arrow in the HUD replaces the rose letters
    lv_obj_add_flag(s_tileRadar, LV_OBJ_FLAG_CLICKABLE);     // receive taps (vessels / empty)
    lv_obj_add_event_cb(s_tileRadar, radar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    build_card();

    // on-screen range/zoom button (reliable single tap; bottom, above the 'S' marker)
    s_zoomBtn = lv_btn_create(s_tileRadar);
    lv_obj_set_size(s_zoomBtn, 120, 44);
    lv_obj_set_ext_click_area(s_zoomBtn, 18);   // invisibly enlarge the tap target (easier to hit)
    lv_obj_align(s_zoomBtn, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_radius(s_zoomBtn, 18, 0);
    lv_obj_set_style_bg_color(s_zoomBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_zoomBtn, 225, 0);
    lv_obj_set_style_border_color(s_zoomBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_zoomBtn, 1, 0);
    lv_obj_set_style_border_opa(s_zoomBtn, 170, 0);
    lv_obj_clear_flag(s_zoomBtn, LV_OBJ_FLAG_SCROLL_CHAIN);  // tapping it must not swipe the tileview
    lv_obj_add_event_cb(s_zoomBtn, zoom_cb, LV_EVENT_PRESSED, NULL);  // fire on touch-down, not release
    s_zoomLbl = lv_label_create(s_zoomBtn);
    lv_label_set_text(s_zoomLbl, LV_SYMBOL_LOOP " 10 nm");
    lv_obj_set_style_text_font(s_zoomLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_zoomLbl, UI_GREEN, 0);
    lv_obj_center(s_zoomLbl);

    // --- top HUD: a big centred clock + date (reference style), a north marker
    //     above it, and small wifi / battery / gps / count indicators flanking it. ---
    lv_obj_t *north = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(north, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(north, UI_GREEN, 0);
    lv_label_set_text(north, LV_SYMBOL_UP);
    lv_obj_align(north, LV_ALIGN_TOP_MID, 0, 12);

    s_hudClock = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudClock, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_hudClock, UI_INK, 0);
    lv_label_set_text(s_hudClock, "--:--");
    lv_obj_align(s_hudClock, LV_ALIGN_TOP_MID, 0, 36);

    s_hudDate = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudDate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudDate, UI_SOFT, 0);
    lv_label_set_text(s_hudDate, "");
    lv_obj_align(s_hudDate, LV_ALIGN_TOP_MID, 0, 72);

    // WiFi 4-bar meter (small, left of the clock)
    s_hudWifi = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_hudWifi);
    lv_obj_set_size(s_hudWifi, 21, 14);
    lv_obj_align(s_hudWifi, LV_ALIGN_TOP_MID, -78, 44);
    lv_obj_clear_flag(s_hudWifi, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; ++i) {
        s_hudBars[i] = lv_obj_create(s_hudWifi);
        lv_obj_remove_style_all(s_hudBars[i]);
        lv_obj_set_size(s_hudBars[i], 3, (lv_coord_t)(4 + i * 3));   // 4, 7, 10, 13 px tall
        lv_obj_align(s_hudBars[i], LV_ALIGN_BOTTOM_LEFT, (lv_coord_t)(i * 5), 0);
        lv_obj_set_style_radius(s_hudBars[i], 1, 0);
        lv_obj_set_style_bg_color(s_hudBars[i], UI_INK, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_hudBars[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_hudGps = lv_label_create(s_tileRadar);     // GPS icon (below the wifi bars)
    lv_obj_set_style_text_font(s_hudGps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudGps, UI_GREEN, 0);
    lv_label_set_text(s_hudGps, "");             // hidden until ui_set_gps() says GPS is on
    lv_obj_align(s_hudGps, LV_ALIGN_TOP_MID, -78, 64);

    s_hudBatt = lv_label_create(s_tileRadar);    // battery (right of the clock)
    lv_obj_set_style_text_font(s_hudBatt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudBatt, UI_INK, 0);
    lv_label_set_text(s_hudBatt, "");
    lv_obj_align(s_hudBatt, LV_ALIGN_TOP_MID, 74, 44);
    // (vessel count is shown in the Stats view; the radar HUD stays clean, like the reference)

    // --- list tile (circular panel, clipped to the round screen) ---
    lv_obj_t *lp = make_round_panel(s_tileList);
    make_tile_title(lp, "VESSELS");
    s_list = lv_list_create(lp);
    lv_obj_set_size(s_list, 320, 372);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);

    // --- stats tile (circular panel) ---
    lv_obj_t *sp = make_round_panel(s_tileStats);
    make_tile_title(sp, "STATS");
    s_statsLbl = lv_label_create(sp);
    lv_obj_set_style_text_font(s_statsLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_statsLbl, UI_SOFT, 0);
    lv_label_set_text(s_statsLbl, "Vessels   0");
    lv_obj_align(s_statsLbl, LV_ALIGN_CENTER, 0, -16);

    s_statsGps = lv_label_create(sp);               // GPS status line (hidden unless GPS is on)
    lv_obj_set_style_text_font(s_statsGps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsGps, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_statsGps, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsGps, "");
    lv_obj_align(s_statsGps, LV_ALIGN_CENTER, 0, 90);

    // footer: where to reach the configuration page (IP / hostname / setup AP)
    s_statsNet = lv_label_create(sp);
    lv_obj_set_width(s_statsNet, 320);
    lv_obj_set_style_text_font(s_statsNet, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsNet, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_statsNet, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsNet, "");
    lv_obj_align(s_statsNet, LV_ALIGN_CENTER, 0, 132);

    lv_obj_t *ver = lv_label_create(sp);            // firmware version (so users can tell what's flashed)
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ver, UI_DIM, 0);
    lv_label_set_text(ver, "Capsule Radar Marine v" FW_VERSION);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 170);

    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_OFF);

    ui_splash_show();   // branded boot splash on top (auto-fades)
}
