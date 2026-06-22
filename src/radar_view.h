#pragma once
// Scope rendering API: scope (rings/sweep/rose), vessels (ship glyphs), selection,
// selectable themes. Pure LVGL, portable (device + SDL simulator).
// Adapted from the aircraft Capsule Radar: aircraft -> Ship, altitude colour ->
// navigation-status / ship-type colour, plane glyph -> ship glyph, range in nm.
#include <vector>
#include "ship.h"

struct RadarSettings {
    double homeLat, homeLon;
    float  rangeNm;             // outer-ring distance in nautical miles
    int    colorMode = 0;       // 0 = by nav status, 1 = by ship type
    double rotationDeg = 0.0;   // 0 = north-up
    bool   mute = false;
};

// Selectable visual skins.
enum RadarTheme {
    THEME_PHOSPHOR = 0,   // green-on-black radar scope (the mockup look)
    THEME_ORB      = 1,   // Orb scope: green gradient, grid, yellow blips
    THEME_AMBER    = 2,   // amber CRT scope (warm monochrome chrome)
    THEME_MILITARY = 3,   // night-vision / military green scope
    THEME_COUNT    = 4
};

// Flattened, display-ready info for one vessel (detail card / list view).
struct ShipInfo {
    uint32_t mmsi;
    char  name[24];
    char  type[16];        // category text (Cargo / Tanker / ...)
    char  status[16];      // nav-status text
    char  dest[24];
    float sogKt;           // NaN if unknown
    float cogDeg;          // NaN if unknown
    float headingDeg;      // NaN if unknown
    float distNm;
    float bearingDeg;
    bool  alert;           // not-under-command / aground / SART
};

namespace radar {

// Build the radar scope (rings, crosshair, rose, sweep, center) under `parent`.
void init(void* lv_parent);                 // pass lv_obj_t*

// Rebuild the vessel layer from the latest snapshot. Call at update cadence.
void update(const std::vector<Ship>& ships, const RadarSettings& s);

// Nearest vessel to (x,y) within a tap radius -> snapshot index, or -1.
int  hitTest(int x, int y);

// Selection (tracked by MMSI so it survives data updates). idx < 0 clears.
void select(int idx);
bool selected(ShipInfo& out);               // false if nothing selected/visible

// Snapshot access for the list / stats views.
int  count();
int  countInRange();                        // vessels within the display range (for the HUD)
bool info(int idx, ShipInfo& out);

// Sweep self-animates via an internal timer; kept for API compatibility.
void tickSweep();

// Selectable visual skin.
void setTheme(int theme);
int  theme();
void cycleTheme();
void setThemeChangedCb(void (*cb)(int theme));   // called when the theme changes (for persistence)
void setColorMode(int mode);                     // 0 = nav status, 1 = ship type
int  colorMode();
void setRangeLabelVisible(bool v);               // hide the built-in range label (UI shows its own)
void setSweepEnabled(bool on);                   // show/hide the rotating sweep line
bool sweepEnabled();
void setTrailLength(int level);                  // 0=off 1=short 2=medium 3=long (vessel trails + flow)

} // namespace radar
