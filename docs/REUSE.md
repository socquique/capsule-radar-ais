# Reuse plan — copy from Capsule Radar (aircraft)

Source: **`/Users/quique/proyectos/PlaneRadar2.0/`** — a finished, working firmware for the **same
board**. Start by copying it, then swap the data layer. This is the fastest, safest path.

## Copy ~as-is (same hardware, no logic change)
| From `PlaneRadar2.0/` | Notes |
|---|---|
| `platformio.ini` | + add `links2004/WebSockets` to `lib_deps`. Keep the platform pin, `memory_type=qio_opi`, partitions, `CORE_DEBUG_LEVEL=0`. |
| `include/lv_conf.h` | LVGL config (and the `-I include` build flag). |
| `src/config.h` | Keep the **verified pin map + I2C addresses**. Replace ADS-B host/cap with AIS box/cap + aisstream host. |
| `src/geo.h` | haversine / bearing / project-to-screen — verbatim. |
| `src/display.*`, `src/touch_cst9217.*` | CO5300 + CST9217 bring-up — verbatim. |
| `src/imu_qmi8658.*`, `src/rtc_pcf85063.*`, `src/battery.*`, `src/audio.*` | Same board peripherals — verbatim. |
| `src/gps.*` | Optional `-G` GPS (auto home position). Hard-won working LC76G I2C driver — copy if you want GPS. |
| `tools/gen_coastline.py` (+ `coastline.*`) | Coastline map — even more relevant for ships. |
| WiFiManager portal + web config + OTA glue in `src/main.cpp` | Reuse the structure; relabel fields; **add an aisstream API-key field** (NVS). |
| Time-zone selector + browser auto-detect (in `main.cpp`) | Copy — same usefulness. |

## Adapt
| File | Change |
|---|---|
| `src/radar_view.*` | Swap the plane glyph → **ship marker** (rotate by COG). Colour by **nav status** or **ship type** instead of altitude. Keep rings/sweep/trails/themes. Range label in **nm**. |
| `src/ui.*` | Radar/List/Stats tileview + detail card + HUD — relabel: **name, MMSI, type, SOG (kt), COG, heading, nav status, destination, distance, bearing**. HUD count = vessels in range. |
| `src/config.h` | Range steps in nm (e.g. 2/5/10/20/40). AIS bounding-box delta. aisstream host. Vessel cap + expiry. |

## Replace (the genuinely new work)
| New file | Replaces | What it does |
|---|---|---|
| `src/ship.h` | `aircraft.h` | The `Ship` data model (see ARCHITECTURE.md). |
| `src/ais_client.h/.cpp` | `adsb_client.*` | Open + maintain the **WSS WebSocket** to aisstream; send subscription (box + key); parse `PositionReport` + `ShipStaticData` JSON; merge/expire by MMSI into the shared `Ship` list. **Persistent connection, not polling.** |
| `ais_task` in `main.cpp` | `adsb_task` | Drives `webSocket.loop()`, handles connect/subscribe/reconnect-backoff, writes the shared list under the mutex. |

## Lessons to carry over (don't relearn the hard way)
- **Internal RAM / TLS is the #1 risk.** Keep JSON + page + framebuffers in **PSRAM**; small internal
  LVGL draw buffer; never churn big internal allocations at runtime. (See ARCHITECTURE.md + the
  Capsule Radar memory files.)
- **All shared-I2C on core 1 only.** Network/TLS on core 0.
- **Render loop never blocks** on network.
- **Secrets (API key, WiFi) in NVS**, entered via the portal — never committed.
- Free data API → gentle use, honest User-Agent, reconnect with backoff, respect terms, MIT firmware,
  non-commercial.

## Suggested first session for Claude Code
1. `cp` the "copy as-is" files; get `platformio.ini` building an empty `main.cpp` (M0).
2. Bring up display + touch + the static scope (reuse `radar_view` with a dummy ship). Confirm on HW.
3. Implement `ais_client` against aisstream with a hard-coded key + box; log parsed ships to serial.
4. Wire ships into `radar_view`; tap → detail. Then portal for key/home/range, themes, alerts (M1→M3).
