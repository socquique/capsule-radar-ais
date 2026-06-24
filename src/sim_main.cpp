// Native (Mac/Linux) LVGL simulator for Capsule Radar — Marine (AIS).
// Runs the SAME LVGL UI (ui_create + radar_view) in an SDL2 window — no hardware,
// no Arduino_GFX, no network. Feeds mock vessels near Dénia so the ship glyphs,
// status/type colours, trails, HUD and detail card can be eyeballed off-device.
//
//   pio run -e native            # build
//   pio run -e native -t exec    # build + run
//   .pio/build/native/program --shot out   # headless: save BMP screenshots and exit
//
// Keys:  T = cycle theme   C = toggle colour mode (status<->type)   R = cycle range
//
// Note: the real panel is a 466x466 *round* AMOLED; this square window shows the
// full buffer, so the corners (hidden on the device) are visible here.
#include <SDL.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "ship.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SIM_W SCREEN_W   // 466
#define SIM_H SCREEN_H

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;

static void sdl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    SDL_Rect r = { area->x1, area->y1, w, h };
    SDL_UpdateTexture(s_tex, &r, px, w * (int)sizeof(lv_color_t));
    if (lv_disp_flush_is_last(drv)) {
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }
    lv_disp_flush_ready(drv);
}

static void sdl_mouse_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int x, y;
    Uint32 btn = SDL_GetMouseState(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = (btn & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED
                                                      : LV_INDEV_STATE_RELEASED;
}

// ---- mock AIS data: a mix of vessels near Dénia that drift along their COG -------
static std::vector<Ship> g_ships;
static std::vector<Ship> g_init;
static RadarSettings g_set;
static int g_rangeIdx = 3;   // index into RANGE_STEPS_NM (20 nm)

static Ship mk(uint32_t mmsi, const char *name, double distNm, double brgDeg,
               float sog, float cog, uint8_t nav, uint16_t type, const char *dest) {
    Ship s;
    s.mmsi = mmsi;
    s.name = name;
    s.dest = dest;
    const double distKm = distNm * 1.852;
    const double br = brgDeg * M_PI / 180.0;
    const double latR = HOME_LAT_DEFAULT * M_PI / 180.0;
    s.lat = HOME_LAT_DEFAULT + (distKm * cos(br)) / 111.0;
    s.lon = HOME_LON_DEFAULT + (distKm * sin(br)) / (111.0 * cos(latR));
    s.sogKt = sog;
    s.cogDeg = cog;
    s.headingDeg = cog;
    s.navStatus = nav;
    s.type = type;
    return s;
}

static void mock_init() {
    g_set.homeLat = HOME_LAT_DEFAULT;
    g_set.homeLon = HOME_LON_DEFAULT;
    g_set.rangeNm = RANGE_STEPS_NM[g_rangeIdx];
    g_set.colorMode = COLOR_MODE_DEFAULT;
    g_set.rotationDeg = 0.0;
    g_ships.clear();
    // A real snapshot of AIS traffic around Dénia (captured from aisstream), so the
    // demo looks like the device does in the field. mk(): distNm + bearing from home.
    //              mmsi        name              distNm  brg   sog   cog  nav                    type dest
    g_ships.push_back(mk(224071970, "L'ILLA PLANA",    0.4,  57,  0.0,   0, NAV_MOORED,            0,  "DENIA"));
    g_ships.push_back(mk(538072104, "DONA FRANCISCA",  0.6,  67,  0.0,   0, NAV_AT_ANCHOR,         36, "-"));
    g_ships.push_back(mk(225990333, "CANALLA 2",      10.9, 346,  0.0,   0, NAV_AT_ANCHOR,         0,  "-"));
    g_ships.push_back(mk(275524000, "PERSEUS",        14.5, 116, 12.0, 170, NAV_UNDERWAY_ENGINE,   0,  "-"));
    g_ships.push_back(mk(268248702, "MAHALO",         15.4, 308,  0.0,   0, NAV_MOORED,            0,  "-"));
    g_ships.push_back(mk(538001657, "SUNBELT SPIRIT", 17.5,  96, 13.5, 216, NAV_UNDERWAY_ENGINE,   79, "VALENCIA"));
    g_ships.back().lengthM = 120; g_ships.back().beamM = 18; g_ships.back().draughtM = 6.5f;   // demo: rich card
    g_ships.back().etaDay = 24;   g_ships.back().etaHour = 14; g_ships.back().etaMin = 30;
    // out of range (> 20 nm) -> edge markers (orb theme) / culled (phosphor)
    g_ships.push_back(mk(224588000, "CIUDAD DE PALMA",35.3, 356, 18.0, 200, NAV_UNDERWAY_ENGINE,   69, "PALMA"));
    g_ships.push_back(mk(311045500, "AKNOUL",         36.4,  59, 12.4, 217, NAV_UNDERWAY_ENGINE,   70, "-"));
    g_ships.push_back(mk(255805844, "TROUPER",        37.2, 358,  9.5,  79, NAV_UNDERWAY_ENGINE,   0,  "-"));
    g_init = g_ships;
}

static void mock_step(double dt) {
    const double latR = HOME_LAT_DEFAULT * M_PI / 180.0;
    for (size_t i = 0; i < g_ships.size(); ++i) {
        Ship &s = g_ships[i];
        if (s.sogKt <= 0.2f) continue;                       // anchored/moored stay put
        const double stepKm = (double)s.sogKt * 1.852 * (dt / 3600.0);   // kt -> km in dt s
        const double br = s.cogDeg * M_PI / 180.0;
        s.lat += (stepKm * cos(br)) / 111.0;
        s.lon += (stepKm * sin(br)) / (111.0 * cos(s.lat * M_PI / 180.0));
        const double dLat = (s.lat - HOME_LAT_DEFAULT) * 111.0;
        const double dLon = (s.lon - HOME_LON_DEFAULT) * 111.0 * cos(latR);
        if (sqrt(dLat * dLat + dLon * dLon) > RANGE_STEPS_NM[g_rangeIdx] * 1.852 * 1.6) {
            s.lat = g_init[i].lat;     // respawn so the scene stays populated
            s.lon = g_init[i].lon;
        }
    }
}

static void sim_range_cb(float nm) {
    g_set.rangeNm = nm;
    int n = (int)(sizeof(RANGE_STEPS_NM) / sizeof(RANGE_STEPS_NM[0]));
    for (int i = 0; i < n; ++i) if (RANGE_STEPS_NM[i] == nm) g_rangeIdx = i;
    radar::update(g_ships, g_set);
    ui_on_data_updated();
    ui_set_range_nm(nm);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *shotPath = (argc >= 3 && strcmp(argv[1], "--shot") == 0) ? argv[2] : NULL;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[sim] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    s_win = SDL_CreateWindow("Capsule Radar — Marine (sim)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             SIM_W, SIM_H, SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren)   // headless / no GPU (e.g. SDL_VIDEODRIVER=dummy): fall back to software
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    if (!s_win || !s_ren) {
        printf("[sim] window/renderer creation failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(s_ren, SIM_W, SIM_H);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
    printf("[sim] SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

    lv_init();
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[SIM_W * 100];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SIM_W * 100);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = sdl_flush;
    disp_drv.hor_res  = SIM_W;
    disp_drv.ver_res  = SIM_H;
    lv_disp_drv_register(&disp_drv);
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    ui_create();
    ui_set_range_cb(sim_range_cb);
    ui_set_range_nm(RANGE_STEPS_NM[g_rangeIdx]);
    radar::setWatchMmsi(275524000);   // demo: watch PERSEUS (amber halo)
    mock_init();
    radar::update(g_ships, g_set);
    ui_on_data_updated();
    printf("[sim] Capsule Radar Marine simulator running (%dx%d) with %d mock vessels.\n",
           SIM_W, SIM_H, (int)g_ships.size());
    printf("[sim] keys: T=theme  C=colour mode  R=range\n");

    Uint32 last = SDL_GetTicks();
    Uint32 lastData = last;
    const Uint32 start = last;
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_t: radar::cycleTheme(); break;
                    case SDLK_c:
                        g_set.colorMode = !g_set.colorMode;
                        radar::setColorMode(g_set.colorMode);
                        printf("[sim] colour mode: %s\n", g_set.colorMode ? "ship type" : "nav status");
                        break;
                    case SDLK_r: {
                        int n = (int)(sizeof(RANGE_STEPS_NM)/sizeof(RANGE_STEPS_NM[0]));
                        sim_range_cb(RANGE_STEPS_NM[(g_rangeIdx + 1) % n]);
                        printf("[sim] range: %.0f nm\n", (double)g_set.rangeNm);
                        break;
                    }
                    case SDLK_w: {   // toggle "watch" on the selected vessel
                        ShipInfo in;
                        if (radar::selected(in)) { radar::setWatchMmsi(in.watched ? 0 : in.mmsi);
                            printf("[sim] watch %s -> %u\n", in.name, in.watched ? 0 : in.mmsi); }
                        break;
                    }
                }
            }
        }
        Uint32 now = SDL_GetTicks();
        lv_tick_inc(now - last);
        last = now;
        if (now - lastData >= 1000) {       // simulate ~1 Hz AIS cadence
            lastData = now;
            mock_step(1.0);
            radar::update(g_ships, g_set);
            ui_on_data_updated();
            char clk[8];
            snprintf(clk, sizeof(clk), "12:%02d", (int)((now / 1000) % 60));
            ui_set_status(true, true, -58, clk);
            ui_set_battery(82, false, true);
            ui_set_date("22 Jun 2026");
            ui_set_netinfo("Configure at\ncapsule-marine.local\n192.168.1.50");
        }
        lv_timer_handler();

        // headless screenshot mode (--shot <prefix>): settle, grab views/themes, exit
        if (shotPath && now - start > 2800) {
            for (int k = 0; k < 30; ++k) { mock_step(1.0); radar::update(g_ships, g_set); }
            radar::select(5);            // SUNBELT SPIRIT (moving cargo → VALENCIA) for the card
            ui_on_data_updated();
            int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            struct Shot { const char *name; int view; int theme; };
            const Shot shots[6] = {
                { "radar",   0, THEME_PHOSPHOR },
                { "orb",     0, THEME_ORB      },
                { "amber",   0, THEME_AMBER    },
                { "military",0, THEME_MILITARY },
                { "list",    1, THEME_PHOSPHOR },
                { "stats",   2, THEME_PHOSPHOR },
            };
            for (int v = 0; v < 6; ++v) {
                radar::setTheme(shots[v].theme);
                ui_show_view(shots[v].view);
                ui_on_data_updated();
                lv_refr_now(NULL);
                SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300]; snprintf(path, sizeof(path), "%s-%s.bmp", shotPath, shots[v].name);
                    SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                    printf("[sim] saved %s\n", path);
                }
            }
            run = false;
        }
        SDL_Delay(5);
    }

    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}
