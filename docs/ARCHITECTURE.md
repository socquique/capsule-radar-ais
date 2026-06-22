# Architecture

Mirrors Capsule Radar (aircraft), with the network layer swapped for a persistent WebSocket.

## Tasks / cores
- **Core 0 — `ais_task`:** WiFi keepalive; open and **maintain the WSS WebSocket** to aisstream;
  send the subscription on (re)connect; on each frame, parse the AIS JSON and update the shared
  `std::vector<Ship>` (or `std::map<mmsi,Ship>`) under a FreeRTOS mutex; expire stale vessels;
  reconnect with backoff on close/error. **No drawing here.**
- **Core 1 — Arduino `loop()`:** LVGL tick + render. Under the mutex, copy/read the ship list,
  project lat/lon → screen (`geo.*`), draw scope + sweep + trails + ship glyphs + labels + the
  active detail card. **No network here.** All shared-I2C (touch/IMU/RTC/battery) on this core only.

## WebSocket client
- Use `links2004/WebSockets` (`WebSocketsClient`) over `WiFiClientSecure` (it supports WSS on S3).
- Lifecycle: `beginSSL(host, 443, "/v0/stream")` → on `WStype_CONNECTED`, send the subscription JSON
  → on `WStype_TEXT`, hand the payload to the parser → on `WStype_DISCONNECTED/ERROR`, flag for
  reconnect (exponential backoff, cap ~30 s). Call `webSocket.loop()` frequently from `ais_task`.
- **TLS:** `setInsecure()` is fine for a hobby device (note it in code). The first handshake needs a
  big contiguous internal block (see memory note).

## Data model — `Ship` (replaces `Aircraft`)
```cpp
struct Ship {
    uint32_t mmsi;
    char     name[24];        // from ShipStaticData / MetaData.ShipName (trimmed)
    double   lat, lon;
    float    sogKt;           // speed over ground (knots)
    float    cogDeg;          // course over ground -> glyph rotation
    float    headingDeg;      // TrueHeading (NaN/511 -> use cog)
    uint8_t  navStatus;       // 0 under way, 1 anchored, 5 moored, 7 fishing, ...
    uint16_t type;            // AIS ship type code -> category/colour
    char     dest[24];        // destination (optional)
    uint32_t lastSeenMs;      // for expiry + staleness
};
```
Keep a parallel display struct in `radar_view` (screen pos, animated glide, trail) exactly like the
aircraft `AcDraw`.

## Rendering (adapt radar_view from Capsule Radar)
- Reuse rings / sweep / rose / range label / trails / themes unchanged.
- **Glyph:** swap the plane triangle for a **ship marker** (a stadium/boat shape or arrow) rotated by
  `cogDeg` (fall back to `headingDeg`). Moored/anchored vessels can be a small dot (no heading).
- **Colour:** by **navigation status** (under way / anchored / moored / fishing) OR by **ship type**
  category (cargo, tanker, passenger, fishing, sailing/pleasure, service/tug, other). Pick one as the
  default, offer the other in settings.
- **Range:** nautical miles (marine convention). Reuse the zoom button; steps e.g. 2/5/10/20/40 nm.
- **Coastline map:** copy `tools/gen_coastline.py` output — land context matters even more for ships.

## Web config / settings (adapt main.cpp from Capsule Radar)
- Captive portal (WiFiManager) for WiFi **+ the aisstream API key** (store in NVS, never commit).
- Web page: home lat/lon (+ map picker), range (nm), colour mode (status/type), theme, brightness,
  sound, time zone (reuse Capsule Radar's auto-detect), OTA. Same NVS `Preferences` pattern.

## Memory budget (the big risk — read this)
The aircraft project's #1 recurring bug was **internal-RAM exhaustion breaking the TLS feed**. For
AIS it's the same class of problem:
- Allocate the **ArduinoJson document in PSRAM** (custom allocator) — AIS frames are small but frequent.
- Keep the **LVGL draw buffer small** (e.g. 40 lines, internal DMA) and the framebuffer/double-buffer
  in **PSRAM**.
- **Never** churn `Wire.setBufferSize()` or large internal `malloc`s at runtime.
- A persistent WebSocket means **one** TLS handshake (vs ADS-B's repeated ones) — good — but that one
  handshake still needs the contiguous internal block, so do heavy init (drivers, buffers) *before*
  bringing up TLS, and keep big buffers in PSRAM.
- Read `PlaneRadar2.0`'s memory notes (`/Users/quique/.claude/projects/-Users-quique-proyectos-PlaneRadar2-0/memory/`)
  for the concrete gotchas (audio PSRAM buffer, GPS Wire buffer, config-page buffer in PSRAM).
