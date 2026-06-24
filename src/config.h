#pragma once
// Capsule Radar — Marine (AIS) — build & user configuration.
// Sibling of the aircraft "Capsule Radar". Same board; the pin map + I2C addresses
// below are copied VERBATIM from the verified aircraft config (do NOT re-derive them).

#define FW_VERSION "0.1.0"   // shown on the web config page + Stats screen; bump on release

// ---------- Home location (default: Dénia, Spain — coastal, good AIS coverage) ----------
// Overridable at runtime via the captive portal (stored in NVS).
#define HOME_LAT_DEFAULT   38.8409
#define HOME_LON_DEFAULT    0.1059

// ---------- Radar (range in NAUTICAL MILES — marine convention) ----------
#define RANGE_NM_DEFAULT    10.0f          // display range (outer ring)
static const float RANGE_STEPS_NM[] = {2.0f, 5.0f, 10.0f, 20.0f, 40.0f};
// The AIS subscription bounding box is built from home ± a delta wider than the display
// range, so vessels just outside the scope show as edge markers. 1 deg lat ~= 60 nm;
// 1 deg lon ~= 60*cos(lat) nm. AIS_BOX_NM is the half-extent of the box (per side).
#define AIS_BOX_MARGIN      1.5f           // box half-extent = display range * this factor
#define MOTION_INTERP       1              // 1 = glyphs glide between updates; 0 = snap
// Ships report far less often than planes (moving: 2-10 s, anchored: ~3 min) and move
// slowly, so expire gently — don't drop a vessel that simply went quiet at anchor.
#define SHIP_STALE_MS       (12 * 60 * 1000)   // drop vessels not seen in ~12 min
#define AIS_MAX_SHIPS       100                // hard cap parsed/kept (protect RAM in busy harbours)

// Colour mode: 0 = by navigation status (under way / anchored / moored / fishing / sailing),
// 1 = by ship type category (cargo / tanker / passenger / fishing / sailing / service / other).
// Default is navigation status (the richer marine signal); switchable in settings.
#define COLOR_MODE_DEFAULT  0

// ---------- Screen (CO5300 AMOLED) ----------
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
#define LV_COLOR_DEPTH_BITS 16
#define LCD_COL_OFFSET      6              // CO5300 column (x) gap on this panel (esp_lcd set_gap 0x06)
#define LCD_ROW_OFFSET      0              // no row (y) gap
#define LCD_QSPI_HZ         80000000       // CO5300 QSPI clock (vendor uses 40 MHz; 80 = faster, verify no artifacts)
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
#define TZ_STR              "CET-1CEST,M3.5.0,M10.5.0/3"  // POSIX TZ (Spain) for local time/date
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         20000          // dim the screen after this long without a touch

// ---------- AIS feed (aisstream.io — free, non-commercial WebSocket) ----------
// Open ONE persistent secure WebSocket, send the subscription JSON once, then receive
// PositionReport + ShipStaticData messages pushed in real time. See docs/DATA_SOURCE.md.
#define AIS_HOST            "stream.aisstream.io"
#define AIS_PORT            443
#define AIS_PATH            "/v0/stream"
#define AIS_USER_AGENT      "CapsuleRadarMarine/0.1 (ESP32-S3 hobby; +https://github.com/socquique/capsule-radar-ais)"
#define AIS_TLS_INSECURE    1               // 1 = setInsecure() (hobby). 0 = pin a root CA.
// The API key is entered via the captive portal and stored in NVS — NEVER committed.
// A compile-time fallback for bench testing only (leave empty for production builds):
#define AIS_API_KEY_FALLBACK ""
#define AIS_RECONNECT_MIN_MS  2000          // backoff floor after a disconnect/error
#define AIS_RECONNECT_MAX_MS  30000         // backoff ceiling
// Self-heal: aisstream can stay "connected" yet stream nothing. If no AIS message
// arrives for this long while connected, drop + reopen the socket (re-subscribes).
#define AIS_STALE_RECONNECT_MS (8 * 60 * 1000)

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial

// ---------- Pin map (VERBATIM from the verified aircraft config) ----------
#define PIN_LCD_CS          12
#define PIN_LCD_RST         39
#define PIN_TP_INT          11
#define PIN_TP_RST          40
#define TP_MIRROR_X         true
#define TP_MIRROR_Y         true

// CO5300 QSPI databus:
#define PIN_LCD_SCLK        38             // QSPI PCLK
#define PIN_LCD_D0          4
#define PIN_LCD_D1          5
#define PIN_LCD_D2          6
#define PIN_LCD_D3          7

// shared I2C bus (touch + IMU + RTC + PMIC + audio codec):
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         14

// ES8311 codec over I2S (alert ping):
#define PIN_I2S_MCLK        42
#define PIN_I2S_BCLK        9
#define PIN_I2S_LRCLK       45             // a.k.a. WS
#define PIN_I2S_DOUT        8              // ESP32 -> codec (speaker)
#define PIN_I2S_DIN         10             // codec -> ESP32 (mics)
#define PIN_AUDIO_PA        46             // speaker amp enable
#define PIN_BOOT_BUTTON     0              // BOOT button (held on boot = captive portal)

// I2C addresses:
#define I2C_ADDR_TOUCH      0x5A           // CST9217
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51
#define I2C_ADDR_PMIC       0x34

// Safety net: should never fire now that pins are filled in. Keeps future edits honest.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "config.h: QSPI/I2C pins are back to placeholders (-1). Restore the real values."
#endif
