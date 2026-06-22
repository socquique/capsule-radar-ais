// Radar scope + vessels (AIS) + selection + selectable themes.
// Pure LVGL, portable. Adapted from the aircraft Capsule Radar:
//   THEME_PHOSPHOR : green-on-black radar scope (rings, sweep, ship glyphs)
//   THEME_ORB      : Orb scope: green gradient, square grid, nearest vessels as
//                    yellow balls (emitting waves) + off-range arrows.
// Vessels are drawn as ship glyphs rotated by COG, coloured by navigation status
// (or ship type), with fading trails. Anchored/moored vessels are quiet dots.
#include "radar_view.h"
#include "config.h"
#include "geo.h"
#include "coastline.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <algorithm>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- phosphor palette (mockup) ----
#define COL_GREEN  lv_color_hex(0x1DFF86)
#define COL_LEAD   lv_color_hex(0x3DFF9A)
#define COL_INK    lv_color_hex(0xEAFFF3)
#define COL_SOFT   lv_color_hex(0x9AFFC8)
#define COL_ALERT  lv_color_hex(0xFF5A3C)
// coastline outline — steel blue, deliberately off the vessel-colour palette so
// land never reads as a vessel track. Even more relevant for ships than planes.
#define COAST_COLOR lv_color_hex(0x4E86C6)
// ---- orb palette (Orb) ----
#define ORB_BLIP   lv_color_hex(0xFFE11A)
#define ORB_ALERT  lv_color_hex(0xFF4D2E)
#define ORB_ACCENT lv_color_hex(0xFF8A1E)
#define ORB_GRID   lv_color_hex(0x3F8B30)
#define ORB_BG_TOP lv_color_hex(0x18540F)
#define ORB_BG_BOT lv_color_hex(0x09250A)
#define ORB_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config ----
#define SWEEP_PERIOD_MS   8000
#define SWEEP_FRAME_MS    30
#define SWEEP_TRAIL_DEG   38.0f
#define SWEEP_TRAIL_STEPS 20
#define SWEEP_TRAIL_OPA   72

// ---- vessel / flow / orb config ----
#define TRAIL_MAX         7
#define TAP_RADIUS_PX     40    // generous finger-tap catch radius (picks the nearest glyph within it)
#define FLOW_MAX          700
#define FLOW_REDRAW_EVERY 80
#define FLOW_OPA          55
#define ORB_BLIPS         7
#define ORB_ARROWS        8
#define BALL_R            9
#define WAVE_EXPAND       28.0f
#define UPDATE_FALLBACK_MS 2000  // glide-clock fallback on the first update (AIS cadence varies)

static int        s_theme    = THEME_PHOSPHOR;
static int        s_colorMode = COLOR_MODE_DEFAULT;
static void      (*s_themeCb)(int) = nullptr;
// scope "chrome" palette (rings/sweep/crosshair/labels) — retinted per theme
static lv_color_t s_cRing = COL_GREEN, s_cLead = COL_LEAD, s_cInk = COL_INK, s_cSoft = COL_SOFT;
static lv_obj_t  *s_parent   = nullptr;
static lv_obj_t  *s_gridLayer = nullptr;
static lv_obj_t  *s_sweep     = nullptr;
static lv_obj_t  *s_shipLayer = nullptr;
static lv_obj_t  *s_flowCanvas = nullptr;
static lv_color_t *s_flowBuf  = nullptr;
static lv_obj_t  *s_rose[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t  *s_centerDot = nullptr;
static lv_obj_t  *s_pulse     = nullptr;
static lv_obj_t  *s_rangeLbl  = nullptr;
static bool       s_rangeLblVisible = true;
static bool       s_sweepEnabled    = true;
static int        s_trailMax        = TRAIL_MAX;   // per-vessel trail length (0 = off)
static int        s_flowMax         = FLOW_MAX;    // persistent flow-layer segments, count cap (0 = off)
static int        s_flowGenMax      = 14;          // ...and an age cap in updates so tracks fade out
static lv_timer_t *s_timer    = nullptr;
static float       s_sweepDeg = 0.0f;
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = UPDATE_FALLBACK_MS;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static std::string s_selMmsi;

struct FlowSeg { lv_point_t a, b; uint16_t gen; };   // gen = the update it was laid down on
static std::deque<FlowSeg> s_flow;
static int s_flowRedrawCtr = 0;
static uint16_t s_flowGen = 0;        // ++ each update(); flow segments fade out after s_flowGenMax updates

struct ShipDraw {
    lv_point_t pos;            // current (animated) screen position — what gets drawn
    lv_point_t from, to;       // smooth-motion glide endpoints
    float      rotDeg;         // glyph rotation (cog, fallback heading)
    lv_color_t color;
    bool       alert;          // not-under-command / aground / SART
    bool       stationary;     // anchored / moored / SOG~0 -> quiet dot
    bool       inRange;
    char       mmsi[12];       // decimal MMSI string (stable key)
    char       label1[24];     // name (or MMSI)
    char       label2[16];     // "12.3 kt" or status text
    // raw fields for the detail card / list
    char       name[24];
    char       type[16];
    char       status[16];
    char       dest[24];
    float      sogKt, cogDeg, headingDeg, distNm, bearingDeg;
    uint8_t    navStatus;      // raw, kept so setColorMode() can recolour in place
    uint16_t   shipType;       // raw AIS type code
    bool       hasAlertFlag;
    std::vector<lv_point_t> trail;
};
static std::vector<ShipDraw> s_ships;
static std::map<std::string, std::vector<lv_point_t>> s_trails;

// Ship glyph (local coords, bow pointing up / -y). A simple elongated hull.
static const float SHX[5] = {  0.0f,  5.0f,  4.0f, -4.0f, -5.0f };
static const float SHY[5] = { -12.0f, -3.0f, 9.0f,  9.0f, -3.0f };

static inline bool orb() { return s_theme == THEME_ORB; }

static void show(lv_obj_t *o, bool v) {
    if (!o) return;
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Vessel colour for the active colour mode. Mirrors ship.h's palette as lv_color_t.
static lv_color_t ship_color(uint8_t navStatus, uint16_t type, int mode) {
    if (mode == 1) {
        switch (shipCategory(type)) {
            case CAT_CARGO:     return lv_color_hex(0x66D17A);
            case CAT_TANKER:    return lv_color_hex(0xFF5A3C);
            case CAT_PASSENGER: return lv_color_hex(0x3CE0FF);
            case CAT_HIGHSPEED: return lv_color_hex(0xFFB23C);
            case CAT_FISHING:   return lv_color_hex(0xFF8A1E);
            case CAT_SAILING:   return lv_color_hex(0xC8FF3C);
            case CAT_SERVICE:   return lv_color_hex(0x8A93A6);
            default:            return lv_color_hex(0x9AFFC8);
        }
    }
    switch (navStatus) {
        case NAV_UNDERWAY_ENGINE:    return lv_color_hex(0x39FF14);
        case NAV_UNDERWAY_SAILING:   return lv_color_hex(0x3CE0FF);
        case NAV_AT_ANCHOR:          return lv_color_hex(0xFFB23C);
        case NAV_MOORED:             return lv_color_hex(0x8A93A6);
        case NAV_FISHING:            return lv_color_hex(0xFF8A1E);
        case NAV_NOT_UNDER_COMMAND:
        case NAV_RESTRICTED_MANOEUV:
        case NAV_CONSTRAINED_DRAUGHT:
        case NAV_AGROUND:
        case NAV_AIS_SART:           return lv_color_hex(0xFF5A3C);
        default:                     return lv_color_hex(0x9AFFC8);
    }
}

static inline lv_point_t rim_point(float bearingDeg, float r) {
    const float a = bearingDeg * (float)M_PI / 180.0f;
    lv_point_t p;
    p.x = (lv_coord_t)lroundf((float)s_cx + r * sinf(a));
    p.y = (lv_coord_t)lroundf((float)s_cy - r * cosf(a));
    return p;
}

// rotate the local point (px,py) by `deg` (clockwise, screen coords) and offset to (ox,oy)
static inline lv_point_t rot_pt(float px, float py, float deg, lv_coord_t ox, lv_coord_t oy) {
    const float a = deg * (float)M_PI / 180.0f;
    const float c = cosf(a), s = sinf(a);
    lv_point_t p;
    p.x = (lv_coord_t)(ox + (lv_coord_t)lroundf(px * c - py * s));
    p.y = (lv_coord_t)(oy + (lv_coord_t)lroundf(px * s + py * c));
    return p;
}

// =============================== flow map ====================================
static void flow_draw_seg(const FlowSeg &s) {
    if (!s_flowCanvas) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = orb() ? ORB_FLOW : s_cRing;
    d.width = 2;
    d.opa = FLOW_OPA;
    lv_point_t pts[2] = { s.a, s.b };
    lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);
}

static void flow_redraw_all(void) {
    if (!s_flowCanvas) return;
    lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    for (const FlowSeg &s : s_flow) flow_draw_seg(s);
}

// =============================== grid ========================================
static void grid_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const lv_point_t c = { s_cx, s_cy };

    if (orb()) {
        lv_draw_line_dsc_t gl;
        lv_draw_line_dsc_init(&gl);
        gl.color = ORB_GRID;
        gl.width = 1;
        gl.opa = 120;
        const int step = 38;
        for (int x = s_cx % step; x < SCREEN_W; x += step) {
            lv_point_t p1 = { (lv_coord_t)x, 0 }, p2 = { (lv_coord_t)x, SCREEN_H - 1 };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        for (int y = s_cy % step; y < SCREEN_H; y += step) {
            lv_point_t p1 = { 0, (lv_coord_t)y }, p2 = { SCREEN_W - 1, (lv_coord_t)y };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        // center "you are here" triangle (orange, pointing up)
        lv_point_t tri[3] = { rot_pt(0, -11, 0, s_cx, s_cy),
                              rot_pt(10, 8, 0, s_cx, s_cy),
                              rot_pt(-10, 8, 0, s_cx, s_cy) };
        lv_draw_rect_dsc_t td;
        lv_draw_rect_dsc_init(&td);
        td.bg_color = ORB_ACCENT;
        td.bg_opa = LV_OPA_COVER;
        td.border_color = lv_color_hex(0x8A4A00);
        td.border_width = 1;
        td.border_opa = 160;
        coastline_draw(d, COAST_COLOR, 170, 2);    // landmass outline under the triangle
        lv_draw_polygon(d, &td, tri, 3);
        return;
    }

    // coastline first, so the rings/crosshair sit cleanly on top of it.
    coastline_draw(d, COAST_COLOR, 165, 2);

    // phosphor: concentric rings + crosshair
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = s_cRing;
    ad.width = 2;
    const lv_coord_t rr[4] = { 50, 104, 160, RADAR_R_OUTER_PX };
    const lv_opa_t   ro[4] = { 66, 66, 66, 87 };
    for (int i = 0; i < 4; ++i) { ad.opa = ro[i]; lv_draw_arc(d, &ad, &c, rr[i], 0, 360); }

    lv_draw_line_dsc_t ll;
    lv_draw_line_dsc_init(&ll);
    ll.color = s_cRing;
    ll.width = 2;
    ll.opa = 41;
    lv_point_t h1 = { (lv_coord_t)(s_cx - 211), s_cy }, h2 = { (lv_coord_t)(s_cx + 211), s_cy };
    lv_point_t v1 = { s_cx, (lv_coord_t)(s_cy - 211) }, v2 = { s_cx, (lv_coord_t)(s_cy + 211) };
    lv_draw_line(d, &ll, &h1, &h2);
    lv_draw_line(d, &ll, &v1, &v2);
}

// =============================== sweep =======================================
static void sweep_draw_cb(lv_event_t *e) {
    if (orb()) return;
    lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);
    const lv_point_t center = { s_cx, s_cy };
    const float R = (float)RADAR_R_OUTER_PX;

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = s_cRing;
    ld.width = 5;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int i = SWEEP_TRAIL_STEPS; i >= 1; --i) {
        const float frac = 1.0f - (float)i / (float)SWEEP_TRAIL_STEPS;
        const float ang  = s_sweepDeg - (float)i * (SWEEP_TRAIL_DEG / (float)SWEEP_TRAIL_STEPS);
        ld.opa = (lv_opa_t)(frac * frac * (float)SWEEP_TRAIL_OPA);
        if (ld.opa < 2) continue;
        lv_point_t p2 = rim_point(ang, R);
        lv_draw_line(dctx, &ld, &center, &p2);
    }
    lv_draw_line_dsc_t le;
    lv_draw_line_dsc_init(&le);
    le.color = s_cLead;
    le.width = 2;
    le.opa = 217;
    le.round_start = 1;
    le.round_end = 1;
    lv_point_t lead = rim_point(s_sweepDeg, R);
    lv_draw_line(dctx, &le, &center, &lead);
}

static void wedge_bbox(float deg, lv_area_t *out) {
    lv_coord_t minx = s_cx, maxx = s_cx, miny = s_cy, maxy = s_cy;
    const int steps = 10;
    for (int i = 0; i <= steps; ++i) {
        const float a = deg - SWEEP_TRAIL_DEG * (float)i / (float)steps;
        const lv_point_t p = rim_point(a, (float)RADAR_R_OUTER_PX);
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    const lv_coord_t pad = 6;
    out->x1 = minx - pad; out->y1 = miny - pad;
    out->x2 = maxx + pad; out->y2 = maxy + pad;
}

// glyph + label bounding box (for partial invalidation during the glide)
static inline lv_area_t glyph_bbox(lv_point_t p) {
    lv_area_t a;
    if (orb()) { a.x1 = p.x - 30; a.y1 = p.y - 30; a.x2 = p.x + 30;  a.y2 = p.y + 30; }
    else       { a.x1 = p.x - 22; a.y1 = p.y - 22; a.x2 = p.x + 148; a.y2 = p.y + 26; }
    return a;
}
static inline void area_union(lv_area_t &d, const lv_area_t &s) {
    d.x1 = LV_MIN(d.x1, s.x1); d.y1 = LV_MIN(d.y1, s.y1);
    d.x2 = LV_MAX(d.x2, s.x2); d.y2 = LV_MAX(d.y2, s.y2);
}

// Advance each glyph from its previous position toward the new target (ease-out),
// invalidating only the small region each one occupies.
static void interp_step(void) {
#if MOTION_INTERP
    if (!s_shipLayer || s_ships.empty()) return;
    const uint32_t now = lv_tick_get();
    float t = s_pollMs ? (float)(now - s_animStartMs) / (float)s_pollMs : 1.0f;
    if (t > 1.0f) t = 1.0f;
    const float e = t * (2.0f - t);                  // ease-out quad
    for (ShipDraw &sh : s_ships) {
        const lv_coord_t nx = sh.from.x + (lv_coord_t)lroundf((float)(sh.to.x - sh.from.x) * e);
        const lv_coord_t ny = sh.from.y + (lv_coord_t)lroundf((float)(sh.to.y - sh.from.y) * e);
        if (nx == sh.pos.x && ny == sh.pos.y) continue;
        lv_point_t np; np.x = nx; np.y = ny;
        lv_area_t inv = glyph_bbox(sh.pos);
        area_union(inv, glyph_bbox(np));
        sh.pos = np;
        lv_obj_invalidate_area(s_shipLayer, &inv);
    }
#endif
}

static void sweep_timer_cb(lv_timer_t *t) {
    (void)t;
    if (++s_frameCtr % 3 == 0) interp_step();         // smooth glyph motion (~90 ms cadence)
    if (orb()) {
        // animate the blip waves (invalidate only the ball areas)
        s_wavePhase += 0.05f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_shipLayer) return;
        int balls = 0;
        for (const ShipDraw &sh : s_ships) {
            if (!sh.inRange) continue;
            if (balls >= ORB_BLIPS) break;
            balls++;
            lv_area_t a = { (lv_coord_t)(sh.pos.x - 44), (lv_coord_t)(sh.pos.y - 44),
                            (lv_coord_t)(sh.pos.x + 44), (lv_coord_t)(sh.pos.y + 44) };
            lv_obj_invalidate_area(s_shipLayer, &a);
        }
        return;
    }
    if (!s_sweepEnabled) return;          // sweep disabled: glyph interpolation above still runs
    s_prevSweepDeg = s_sweepDeg;
    s_sweepDeg += 360.0f * (float)SWEEP_FRAME_MS / (float)SWEEP_PERIOD_MS;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
    if (!s_sweep) return;
    lv_area_t a, b, area;
    wedge_bbox(s_prevSweepDeg, &a);
    wedge_bbox(s_sweepDeg, &b);
    area.x1 = LV_MIN(a.x1, b.x1);
    area.y1 = LV_MIN(a.y1, b.y1);
    area.x2 = LV_MAX(a.x2, b.x2);
    area.y2 = LV_MAX(a.y2, b.y2);
    lv_obj_invalidate_area(s_sweep, &area);
}

// =============================== vessels =====================================
static void draw_trail(lv_draw_ctx_t *d, const ShipDraw &sh, lv_color_t col) {
    const int n = (int)sh.trail.size();
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 2;
    for (int i = 1; i < n; ++i) {
        t.opa = (lv_opa_t)(10 + 45 * i / n);
        lv_point_t a = sh.trail[i - 1], b = sh.trail[i];
        lv_draw_line(d, &t, &a, &b);
    }
}

static void draw_ball(lv_draw_ctx_t *d, const ShipDraw &sh) {
    // emitted waves: several expanding rings (sonar-ping look)
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = ORB_ACCENT;
    w.width = 3;
    for (int wv = 0; wv < 3; ++wv) {
        float ph = s_wavePhase + (float)wv * 0.34f;
        if (ph >= 1.0f) ph -= 1.0f;
        w.opa = (lv_opa_t)((1.0f - ph) * 245.0f);
        if (w.opa > 6) lv_draw_arc(d, &w, &sh.pos, (uint16_t)(BALL_R + 3 + ph * WAVE_EXPAND), 0, 360);
    }
    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = sh.alert ? ORB_ALERT : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    b.border_color = lv_color_hex(0x7A5A00);
    b.border_width = 1;
    b.border_opa = 150;
    lv_area_t r = { (lv_coord_t)(sh.pos.x - BALL_R), (lv_coord_t)(sh.pos.y - BALL_R),
                    (lv_coord_t)(sh.pos.x + BALL_R), (lv_coord_t)(sh.pos.y + BALL_R) };
    lv_draw_rect(d, &b, &r);
    // glossy highlight
    lv_draw_rect_dsc_t hl;
    lv_draw_rect_dsc_init(&hl);
    hl.bg_color = lv_color_hex(0xFFFBCC);
    hl.bg_opa = 170;
    hl.radius = LV_RADIUS_CIRCLE;
    lv_area_t hr = { (lv_coord_t)(sh.pos.x - 5), (lv_coord_t)(sh.pos.y - 6),
                     (lv_coord_t)(sh.pos.x - 1), (lv_coord_t)(sh.pos.y - 2) };
    lv_draw_rect(d, &hl, &hr);
}

static void draw_offrange(lv_draw_ctx_t *d, const ShipDraw &sh) {
    // small ball at the rim
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = sh.alert ? ORB_ALERT : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    lv_area_t r = { (lv_coord_t)(sh.pos.x - 5), (lv_coord_t)(sh.pos.y - 5),
                    (lv_coord_t)(sh.pos.x + 5), (lv_coord_t)(sh.pos.y + 5) };
    lv_draw_rect(d, &b, &r);
    // small orange triangle just outside it, pointing toward the vessel's bearing
    const lv_coord_t ox = (lv_coord_t)lroundf(sh.pos.x + 12.0f * sinf(sh.bearingDeg * (float)M_PI / 180.0f));
    const lv_coord_t oy = (lv_coord_t)lroundf(sh.pos.y - 12.0f * cosf(sh.bearingDeg * (float)M_PI / 180.0f));
    lv_point_t tri[3] = { rot_pt(0, -7, sh.bearingDeg, ox, oy),
                          rot_pt(5, 4, sh.bearingDeg, ox, oy),
                          rot_pt(-5, 4, sh.bearingDeg, ox, oy) };
    lv_draw_rect_dsc_t td;
    lv_draw_rect_dsc_init(&td);
    td.bg_color = ORB_ACCENT;
    td.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &td, tri, 3);
}

// A quiet diamond for anchored/moored (stationary) vessels — no heading to show.
static void draw_dot(lv_draw_ctx_t *d, const ShipDraw &sh) {
    lv_point_t dia[4] = { { sh.pos.x, (lv_coord_t)(sh.pos.y - 6) },
                          { (lv_coord_t)(sh.pos.x + 6), sh.pos.y },
                          { sh.pos.x, (lv_coord_t)(sh.pos.y + 6) },
                          { (lv_coord_t)(sh.pos.x - 6), sh.pos.y } };
    lv_draw_rect_dsc_t g;
    lv_draw_rect_dsc_init(&g);
    g.bg_color = sh.color;
    g.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &g, dia, 4);
}

static void draw_glyph(lv_draw_ctx_t *d, const ShipDraw &sh) {
    if (sh.stationary) { draw_dot(d, sh); return; }
    const float th = sh.rotDeg * (float)M_PI / 180.0f;
    const float c = cosf(th), s = sinf(th);
    lv_point_t pts[5];
    for (int i = 0; i < 5; ++i) {
        const float x = SHX[i] * c - SHY[i] * s;
        const float y = SHX[i] * s + SHY[i] * c;
        pts[i].x = (lv_coord_t)(sh.pos.x + (lv_coord_t)lroundf(x));
        pts[i].y = (lv_coord_t)(sh.pos.y + (lv_coord_t)lroundf(y));
    }
    lv_draw_rect_dsc_t g;
    lv_draw_rect_dsc_init(&g);
    g.bg_color = sh.color;
    g.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &g, pts, 5);
}

static void ship_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const bool drg = orb();
    int balls = 0, arrows = 0;

    for (const ShipDraw &sh : s_ships) {
        if (drg) {
            if (sh.inRange) {
                if (balls >= ORB_BLIPS) continue;   // up to 7 in-range balls
                draw_trail(d, sh, ORB_FLOW);
                draw_ball(d, sh);
                balls++;
            } else {
                if (arrows >= ORB_ARROWS) continue;  // up to 8 off-range arrows
                draw_offrange(d, sh);
                arrows++;
            }
        } else {
            if (!sh.inRange) continue;            // phosphor shows in-range traffic only
            draw_trail(d, sh, sh.color);
            draw_glyph(d, sh);
            if (sh.alert) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = COL_ALERT; h.width = 2; h.opa = 200;
                lv_draw_arc(d, &h, &sh.pos, 16, 0, 360);
            }
        }

        // selection ring(s)
        if (!s_selMmsi.empty() && s_selMmsi == sh.mmsi) {
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.width = 2;
            sr.opa = 240;
            if (drg) {
                sr.color = ORB_ACCENT;
                lv_draw_arc(d, &sr, &sh.pos, 15, 0, 360);
                lv_draw_arc(d, &sr, &sh.pos, 23, 0, 360);
            } else {
                sr.color = sh.alert ? COL_ALERT : s_cInk;
                lv_draw_arc(d, &sr, &sh.pos, 19, 0, 360);
            }
        }

        // floating labels (phosphor only; orb keeps clean balls + the tap card)
        if (!drg) {
            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = &lv_font_montserrat_14;
            lc.color = s_cInk;
            lv_area_t a1 = { (lv_coord_t)(sh.pos.x + 12), (lv_coord_t)(sh.pos.y - 14),
                             (lv_coord_t)(sh.pos.x + 142), (lv_coord_t)(sh.pos.y + 2) };
            if (sh.label1[0]) lv_draw_label(d, &lc, &a1, sh.label1, NULL);
            lv_draw_label_dsc_t la;
            lv_draw_label_dsc_init(&la);
            la.font = &lv_font_montserrat_12;
            la.color = sh.color;
            lv_area_t a2 = { a1.x1, (lv_coord_t)(sh.pos.y + 2), a1.x2, (lv_coord_t)(sh.pos.y + 20) };
            if (sh.label2[0]) lv_draw_label(d, &la, &a2, sh.label2, NULL);
        }
    }
}

// =============================== helpers =====================================
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                            lv_color_t color, lv_align_t align, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, dx, dy);
    return l;
}

static lv_obj_t *make_layer(lv_obj_t *parent, lv_event_cb_t draw_cb) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_center(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (draw_cb) lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

static void pulse_anim_cb(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    const lv_coord_t dia = 10 + (lv_coord_t)((v * 44) / 100);
    lv_obj_set_size(o, dia, dia);
    lv_obj_center(o);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 220 / 100), 0);
}

namespace radar {

void setTheme(int t) {
    s_theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    const bool drg = orb();

    switch (s_theme) {                          // pick the scope chrome palette
        case THEME_AMBER:
            s_cRing = lv_color_hex(0xFFB23C); s_cLead = lv_color_hex(0xFFD27A);
            s_cInk  = lv_color_hex(0xFFE9C2); s_cSoft = lv_color_hex(0xFFC98A); break;
        case THEME_MILITARY:
            s_cRing = lv_color_hex(0x49C46B); s_cLead = lv_color_hex(0x76E08C);
            s_cInk  = lv_color_hex(0xE0FFE6); s_cSoft = lv_color_hex(0x9FD7A8); break;
        default:                                // phosphor (orb uses its own colors)
            s_cRing = COL_GREEN; s_cLead = COL_LEAD; s_cInk = COL_INK; s_cSoft = COL_SOFT; break;
    }

    if (s_parent) {
        if (drg) {
            lv_obj_set_style_bg_color(s_parent, ORB_BG_TOP, 0);
            lv_obj_set_style_bg_grad_color(s_parent, ORB_BG_BOT, 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_VER, 0);
        } else {
            lv_obj_set_style_bg_color(s_parent, lv_color_black(), 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_NONE, 0);
        }
        lv_obj_set_style_bg_opa(s_parent, LV_OPA_COVER, 0);
    }
    for (int i = 0; i < 4; ++i) show(s_rose[i], !drg);   // hide compass in Orb
    show(s_rangeLbl, !drg && s_rangeLblVisible);
    show(s_centerDot, !drg);                             // orb draws an orange triangle instead
    show(s_pulse, !drg);

    // retint the persistent chrome objects for the active palette
    if (s_rose[0]) lv_obj_set_style_text_color(s_rose[0], s_cInk, 0);
    for (int i = 1; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cSoft, 0);
    if (s_centerDot) lv_obj_set_style_bg_color(s_centerDot, s_cInk, 0);
    if (s_pulse)     lv_obj_set_style_border_color(s_pulse, s_cInk, 0);
    if (s_rangeLbl)  lv_obj_set_style_text_color(s_rangeLbl, s_cRing, 0);

    flow_redraw_all();
    if (s_parent) lv_obj_invalidate(s_parent);
    if (s_themeCb) s_themeCb(s_theme);
}

int  theme() { return s_theme; }
void cycleTheme() { setTheme(s_theme + 1); }
void setThemeChangedCb(void (*cb)(int)) { s_themeCb = cb; }

void setColorMode(int mode) {
    s_colorMode = (mode == 1) ? 1 : 0;
    // recolour the current snapshot in place so the change is immediate
    for (ShipDraw &sh : s_ships) sh.color = ship_color(sh.navStatus, sh.shipType, s_colorMode);
    if (s_shipLayer) lv_obj_invalidate(s_shipLayer);
}
int colorMode() { return s_colorMode; }

void setRangeLabelVisible(bool v) { s_rangeLblVisible = v; if (s_rangeLbl) show(s_rangeLbl, v && !orb()); }

void setSweepEnabled(bool on) {
    s_sweepEnabled = on;
    if (s_sweep) {
        show(s_sweep, on);
        if (!on) lv_obj_invalidate(s_sweep);   // clear any wedge currently painted
    }
}
bool sweepEnabled() { return s_sweepEnabled; }

// 0 = off, 1 = short, 2 = medium (default), 3 = long. Controls both the per-vessel
// trail and the persistent flow layer (the long-lived "where everything has been" tracks).
void setTrailLength(int level) {
    switch (level) {
        case 0: s_trailMax = 0;  s_flowMax = 0;    s_flowGenMax = 0;  break;
        case 1: s_trailMax = 3;  s_flowMax = 150;  s_flowGenMax = 8;  break;
        case 3: s_trailMax = 12; s_flowMax = 1500; s_flowGenMax = 30; break;
        default: s_trailMax = 7; s_flowMax = 700;  s_flowGenMax = 14; break;
    }
    if (s_flowMax == 0) { s_flow.clear(); s_trails.clear(); }
    else while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
    flow_redraw_all();
    if (s_shipLayer) lv_obj_invalidate(s_shipLayer);
}

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_ships.clear();
    s_trails.clear();
    s_flow.clear();
    s_selMmsi.clear();
    s_flowRedrawCtr = 0;

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_flowBuf) {
        const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
        s_flowBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_flowBuf = (lv_color_t *)malloc(sz);
#endif
    }
    s_flowCanvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_flowCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (s_flowBuf) {
        lv_canvas_set_buffer(s_flowCanvas, s_flowBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    }
    lv_obj_center(s_flowCanvas);

    s_gridLayer = make_layer(parent, grid_draw_cb);
    s_sweep     = make_layer(parent, sweep_draw_cb);
    s_shipLayer = make_layer(parent, ship_draw_cb);

    s_rose[0] = make_label(parent, "N", &lv_font_montserrat_28, COL_INK,  LV_ALIGN_TOP_MID,    0, 12);
    s_rose[1] = make_label(parent, "S", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_rose[2] = make_label(parent, "E", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_RIGHT_MID, -12, 0);
    s_rose[3] = make_label(parent, "W", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_LEFT_MID,   12, 0);

    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f nm", (double)RANGE_NM_DEFAULT);
    s_rangeLbl = make_label(parent, rng, &lv_font_montserrat_14, COL_GREEN, LV_ALIGN_CENTER, 92, -8);
    lv_obj_set_style_text_opa(s_rangeLbl, 128, 0);

    s_pulse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pulse);
    lv_obj_set_size(s_pulse, 12, 12);
    lv_obj_center(s_pulse);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pulse, COL_INK, 0);
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_centerDot = lv_obj_create(parent);
    lv_obj_remove_style_all(s_centerDot);
    lv_obj_set_size(s_centerDot, 7, 7);
    lv_obj_center(s_centerDot);
    lv_obj_set_style_radius(s_centerDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_centerDot, COL_INK, 0);
    lv_obj_set_style_bg_opa(s_centerDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_centerDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);

    setTheme(s_theme);
}

void update(const std::vector<Ship> &ships, const RadarSettings &s) {
    s_colorMode = s.colorMode;
    std::vector<ShipDraw> out;
    out.reserve(ships.size());
    std::set<std::string> present;
    const float R = (float)RADAR_R_OUTER_PX;
    const double rangeKm = (double)s.rangeNm * 1.852;   // coastline projection works in km
    ++s_flowGen;

    // Reproject the coastline only when the scope geometry actually changes.
    static double s_coLat = 1e9, s_coLon = 1e9; static float s_coRange = -1.0f;
    if (s.homeLat != s_coLat || s.homeLon != s_coLon || s.rangeNm != s_coRange) {
        const bool firstFix = (s_coRange < 0.0f);
        s_coLat = s.homeLat; s_coLon = s.homeLon; s_coRange = s.rangeNm;
        coastline_project(s.homeLat, s.homeLon, rangeKm, s_cx, s_cy, R);
        if (s_gridLayer) lv_obj_invalidate(s_gridLayer);
        if (!firstFix) {
            s_trails.clear();
            s_flow.clear();
            flow_redraw_all();
        }
    }

    std::map<std::string, lv_point_t> prevPos;        // smooth-motion: glide starts here
    for (const ShipDraw &a : s_ships) prevPos[a.mmsi] = a.pos;

    for (const Ship &v : ships) {
        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, v.lat, v.lon);
        const double distNm = geo::kmToNm(distKm);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, v.lat, v.lon);
        // ratio (dist/range) is unit-independent, so projecting in nm is fine.
        const geo::Point p = geo::projectToScreen(distNm, brg, s.rangeNm, s_cx, s_cy, R, s.rotationDeg);

        ShipDraw d;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;

        char key[12];
        snprintf(key, sizeof(key), "%u", v.mmsi);
        {
            auto pit = prevPos.find(std::string(key));
            if (pit != prevPos.end()) {
                const long dx = (long)target.x - pit->second.x;
                const long dy = (long)target.y - pit->second.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : pit->second;  // snap if it jumped
            } else d.from = target;                                                  // new contact: appear in place
        }
#if MOTION_INTERP
        d.pos = d.from;
#else
        d.pos = target;
        d.from = target;
#endif
        d.inRange = p.inRange;

        // rotation: prefer COG, fall back to heading, else 0 (north)
        float rot = v.cogDeg;
        if (isnan(rot)) rot = v.headingDeg;
        if (isnan(rot)) rot = 0.0f;
        d.rotDeg = rot;
        d.color = ship_color(v.navStatus, v.type, s.colorMode);
        d.navStatus = v.navStatus;
        d.shipType = v.type;
        d.alert = shipIsAlert(v.navStatus);
        d.stationary = shipIsStationary(v.navStatus, v.sogKt);
        d.hasAlertFlag = d.alert;

        snprintf(d.mmsi, sizeof(d.mmsi), "%s", key);
        snprintf(d.name, sizeof(d.name), "%s", v.name.c_str());
        snprintf(d.type, sizeof(d.type), "%s", shipTypeName(v.type));
        snprintf(d.status, sizeof(d.status), "%s", navStatusName(v.navStatus));
        snprintf(d.dest, sizeof(d.dest), "%s", v.dest.c_str());
        d.sogKt = v.sogKt;
        d.cogDeg = v.cogDeg;
        d.headingDeg = v.headingDeg;
        d.distNm = (float)distNm;
        d.bearingDeg = (float)brg;

        // labels: name (or MMSI) on top, then speed if moving else status
        if (d.name[0]) snprintf(d.label1, sizeof(d.label1), "%s", d.name);
        else           snprintf(d.label1, sizeof(d.label1), "%s", d.mmsi);
        if (d.stationary)            snprintf(d.label2, sizeof(d.label2), "%s", d.status);
        else if (!isnan(v.sogKt))    snprintf(d.label2, sizeof(d.label2), "%.1f kt", (double)v.sogKt);
        else                         d.label2[0] = '\0';

        const std::string skey = key;
        present.insert(skey);
        if (d.inRange) {
            std::vector<lv_point_t> &hist = s_trails[skey];
            const bool moved = hist.empty() ||
                               abs((int)hist.back().x - (int)target.x) > 0 ||
                               abs((int)hist.back().y - (int)target.y) > 0;
            if (moved) {
                if (s_flowMax > 0 && !hist.empty()) {
                    FlowSeg seg = { hist.back(), target, s_flowGen };
                    s_flow.push_back(seg);
                    while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
                    flow_draw_seg(seg);
                }
                if (s_trailMax > 0) {
                    hist.push_back(target);
                    while ((int)hist.size() > s_trailMax) hist.erase(hist.begin());
                } else {
                    hist.clear();
                }
            }
            d.trail = hist;
        }
        out.push_back(std::move(d));
    }

    for (auto it = s_trails.begin(); it != s_trails.end();) {
        if (present.find(it->first) == present.end()) it = s_trails.erase(it);
        else ++it;
    }
    if (!s_selMmsi.empty() && present.find(s_selMmsi) == present.end()) s_selMmsi.clear();

    // Fade the flow layer by AGE: drop segments older than s_flowGenMax updates.
    if (s_flowGenMax > 0 && !s_flow.empty()) {
        bool pruned = false;
        while (!s_flow.empty() && (uint16_t)(s_flowGen - s_flow.front().gen) > (uint16_t)s_flowGenMax) {
            s_flow.pop_front();
            pruned = true;
        }
        if (pruned) flow_redraw_all();
    }

    // nearest first (the blips + the list); cap to keep work bounded
    std::sort(out.begin(), out.end(),
              [](const ShipDraw &a, const ShipDraw &b) { return a.distNm < b.distNm; });
    if (out.size() > 20) out.resize(20);

    if (++s_flowRedrawCtr >= FLOW_REDRAW_EVERY) {
        s_flowRedrawCtr = 0;
        flow_redraw_all();
    }

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        snprintf(r, sizeof(r), "%.0f nm", (double)s.rangeNm);
        lv_label_set_text(s_rangeLbl, r);
    }

    const uint32_t now = lv_tick_get();              // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)UPDATE_FALLBACK_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    s_ships = std::move(out);
    if (s_shipLayer) lv_obj_invalidate(s_shipLayer);
}

int hitTest(int x, int y) {
    int best = -1;
    long bestD = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
    const bool drg = orb();
    int balls = 0, arrows = 0;
    for (size_t i = 0; i < s_ships.size(); ++i) {
        if (drg) {
            if (s_ships[i].inRange) { if (balls >= ORB_BLIPS) continue; balls++; }
            else { if (arrows >= ORB_ARROWS) continue; arrows++; }
        } else if (!s_ships[i].inRange) continue;
        const long dx = (long)s_ships[i].pos.x - x;
        const long dy = (long)s_ships[i].pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    }
    return best;
}

static void fill_info(const ShipDraw &a, ShipInfo &out) {
    out.mmsi = (uint32_t)strtoul(a.mmsi, nullptr, 10);
    snprintf(out.name, sizeof(out.name), "%s", a.name);
    snprintf(out.type, sizeof(out.type), "%s", a.type);
    snprintf(out.status, sizeof(out.status), "%s", a.status);
    snprintf(out.dest, sizeof(out.dest), "%s", a.dest);
    out.sogKt = a.sogKt; out.cogDeg = a.cogDeg; out.headingDeg = a.headingDeg;
    out.distNm = a.distNm; out.bearingDeg = a.bearingDeg;
    out.alert = a.hasAlertFlag;
}

void select(int idx) {
    if (idx < 0 || idx >= (int)s_ships.size()) s_selMmsi.clear();
    else s_selMmsi = s_ships[idx].mmsi;
    if (s_shipLayer) lv_obj_invalidate(s_shipLayer);
}

bool selected(ShipInfo &out) {
    if (s_selMmsi.empty()) return false;
    for (const ShipDraw &a : s_ships)
        if (s_selMmsi == a.mmsi) { fill_info(a, out); return true; }
    return false;
}

int count() { return (int)s_ships.size(); }

int countInRange() {
    int n = 0;
    for (const ShipDraw &a : s_ships) if (a.inRange) ++n;
    return n;
}

bool info(int idx, ShipInfo &out) {
    if (idx < 0 || idx >= (int)s_ships.size()) return false;
    fill_info(s_ships[idx], out);
    return true;
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

} // namespace radar
