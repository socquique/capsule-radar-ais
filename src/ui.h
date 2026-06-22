#pragma once
// UI: swipeable views (radar / list / stats) + tap-to-inspect vessel detail card.
// Pure LVGL, portable (device + SDL simulator). Builds on top of radar_view.
void ui_create(void);            // build the whole UI on the active screen
void ui_on_data_updated(void);   // refresh card/list/stats after radar::update()
void ui_show_view(int idx);      // 0 = radar, 1 = list, 2 = stats
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock);  // HUD: signal bars (count=RSSI, colour: red=down, amber=stale feed, white=ok) + clock
void ui_set_battery(int pct, bool charging, bool present);  // top HUD battery indicator
void ui_set_date(const char *date);  // top HUD date line (e.g. "22 Jun 2026")
void ui_set_netinfo(const char *line);  // stats view footer: how to reach the config page
void ui_set_gps(int state, int sats);   // GPS indicator: state 0=off/hidden 1=acquiring 2=fix; HUD icon + Stats line
void ui_splash_show(void);  // branded boot splash (auto-fades, covers init time)
void ui_set_range_cb(void (*cb)(float nm));  // on-screen zoom button -> notify main (range in nm)
void ui_set_range_nm(float nm);              // update the zoom button label / sync the cycle
void ui_set_color_mode_cb(void (*cb)(int mode));  // colour-mode toggle button -> notify main
