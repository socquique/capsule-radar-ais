# Capsule Radar — Marine (AIS) — CLAUDE.md

Master context for Claude Code. Read this first, then `docs/` for detail.

> **Working name:** "Capsule Radar — Marine" (or *Capsule Sonar* / *Harbor Radar* — rename freely).
> This is the **marine/AIS sibling** of **Capsule Radar**, the live *aircraft* radar. Same board,
> same look & feel, but it plots **ships** from a live **AIS** feed instead of aircraft from ADS-B.

## What we're building
A live **AIS ship radar** for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (round 466×466 CO5300
AMOLED, capacitive touch). It pulls nearby **vessels** from a free online **AIS** feed over WiFi and
plots them on a radar scope centered on the user (a coastal home, harbour, or a spot by a navigable
river/canal). Ship glyphs are rotated by **course (COG)**, colour-coded by **ship type / navigation
status**, with fading trails, an animated sweep, and **tap-to-inspect** (name, MMSI, type, speed,
course, destination, status). Useful by the sea **and** inland (canals/rivers — barges).

Target end-result, like the aircraft version: a polished, MakerWorld-publishable desk gadget.

## ⭐ Source project to reuse — READ THIS FIRST
The finished, working **aircraft** firmware lives at **`/Users/quique/proyectos/PlaneRadar2.0/`**
(product name "Capsule Radar"). It targets the **exact same board**. Do NOT start from scratch —
**copy and adapt** from it. See `docs/REUSE.md` for the file-by-file plan. In short:

- **Copy ~as-is** (same board): `display.*`, `touch_cst9217.*`, `imu_qmi8658.*`, `rtc_pcf85063.*`,
  `battery.*`, `audio.*`, `geo.h`, `platformio.ini`, `include/lv_conf.h`, the WiFiManager captive
  portal + web config + OTA glue in `main.cpp`, and `tools/gen_coastline.py` (coastline map — even
  more useful for ships).
- **Adapt**: `radar_view.*` (swap the plane glyph for a ship glyph; colour by ship type/status),
  `ui.*` (Radar/List/Stats tileview + detail card + HUD — relabel for ships), `config.h`.
- **Replace**: `adsb_client.*` → **`ais_client.*`** (WebSocket, not HTTP polling);
  the `Aircraft` data model → **`Ship`**.
- **Read** `PlaneRadar2.0/CLAUDE.md` and the project memory for the hard-won lessons (esp. the
  **internal-RAM / mbedTLS fragility** — it bites even harder here, see below).

## Hardware (same board — see docs/HARDWARE.md)
Identical to Capsule Radar. The **verified pin map + I2C addresses** are already in
`/Users/quique/proyectos/PlaneRadar2.0/src/config.h` — copy them verbatim. MCU ESP32-S3R8
(8 MB PSRAM / 16 MB flash), CO5300 AMOLED over QSPI, CST9217 touch + IMU + RTC + PMIC + audio on a
shared I2C bus. (The optional **-G** variant adds a GPS — handy here to auto-set the home position;
the working LC76G I2C driver is in `PlaneRadar2.0/src/gps.*`.)

## Stack decision
**PlatformIO + Arduino** (pioarduino `platform-espressif32`, same release as Capsule Radar). Libraries:
- `moononournation/GFX Library for Arduino` — CO5300 QSPI driver.
- `lvgl/lvgl` 8.x — UI.
- `bblanchon/ArduinoJson` 7 — parse AIS JSON.
- **`links2004/WebSockets`** (arduinoWebSockets) — **WSS (secure WebSocket)** client for the AIS feed.
  This is the main *new* dependency vs the aircraft project.
- WiFi / WiFiClientSecure / Preferences / WiFiManager / ArduinoOTA (built-in / same as Capsule Radar).

## Data source — AIS (full detail in docs/DATA_SOURCE.md)
**Primary: [aisstream.io](https://aisstream.io)** — a **FREE WebSocket AIS feed** (needs a free API
key; non-commercial). Connect to `wss://stream.aisstream.io/v0/stream`, send a JSON subscription with
your API key + a **bounding box** around home, then receive `PositionReport` / `ShipStaticData`
messages pushed in real time. Merge them by **MMSI** into the `Ship` list.

Key differences from ADS-B:
- **WebSocket push, not HTTP polling** — keep a persistent WSS connection alive on core 0.
- Queried by a **lat/lon bounding box**, not a centre+radius.
- Coverage is **terrestrial-receiver based** → best near coasts / busy waterways; open ocean is sparse.
- AIS gives you the **ship's name**, type, and **navigation status** (under way / at anchor / moored)
  — richer than ADS-B. Ships also move slowly → longer trails, gentler update cadence.
- Be polite + honest User-Agent; respect aisstream's free-tier / non-commercial terms (this project
  qualifies). Fallbacks: a self-hosted AIS receiver (RTL-SDR/dAISy → NMEA) is a future option.

## Architecture (full detail in docs/ARCHITECTURE.md)
- **Core 0 task** (`ais_task`): WiFi keepalive; open + maintain the WSS WebSocket; on each incoming
  message, parse the AIS JSON (ArduinoJson, **PSRAM allocator**) and update a shared
  `std::vector<Ship>` guarded by a FreeRTOS mutex; expire vessels not seen in N minutes.
- **Core 1 / loop**: LVGL tick + render. Reads the ship list under the mutex, projects lat/lon →
  screen (`geo.*`), draws scope, sweep, trails, ship glyphs (rotated by COG), labels, detail card.
- 8 MB PSRAM holds the framebuffer + double buffer + the JSON/ship buffers.
- ⚠️ **Internal-RAM / TLS landmine (carried from Capsule Radar — applies HARDER here):** the WSS
  handshake (mbedTLS) needs a large **contiguous internal RAM block**; fragmentation silently kills
  the connection. Keep JSON/parse/page buffers in **PSRAM**, never churn `Wire.setBufferSize()` or
  big internal allocations at runtime, use a small internal LVGL DMA draw buffer. A *persistent*
  WebSocket may actually be easier on RAM than repeated TLS handshakes — but the first handshake
  still needs the contiguous block, so budget for it.

## Repo layout (target — mirror Capsule Radar)
```
capsule-radar-ais/
├─ CLAUDE.md              ← you are here
├─ README.md
├─ platformio.ini        ← copy from PlaneRadar2.0, add the WebSockets lib
├─ src/
│  ├─ config.h           ← copy pins from PlaneRadar2.0; AIS box size, aisstream host/key handling
│  ├─ geo.h              ← copy as-is
│  ├─ ship.h             ← Ship data model (replaces aircraft.h)
│  ├─ ais_client.h/.cpp  ← NEW: WSS connect to aisstream + parse AIS JSON (replaces adsb_client)
│  ├─ radar_view.h/.cpp  ← adapt from PlaneRadar2.0 (ship glyph + type/status colours)
│  ├─ ui.h/.cpp          ← adapt (Radar/List/Stats + ship detail card)
│  ├─ display/touch/imu/rtc/battery/audio  ← copy from PlaneRadar2.0
│  └─ main.cpp           ← adapt task setup + web config + OTA
├─ docs/  (HARDWARE / DATA_SOURCE / ARCHITECTURE / FEATURES / REUSE)
└─ tools/ gen_coastline.py (copy from PlaneRadar2.0)
```

## Roadmap (milestones)
- **M0 — Bring-up:** copy the board drivers from Capsule Radar; light up the scope (rings, sweep,
  N/E/S/W) + one static ship glyph. Confirm touch + display.
- **M1 — AIS client:** WSS connect to aisstream, subscribe to a bounding box around home, parse
  `PositionReport` + `ShipStaticData`, build/merge the `Ship` list by MMSI, expire stale.
- **M2 — Live scope:** project ships to screen, glyph rotated by COG, colour by ship type / nav
  status, fading trails; tap → detail card (name, MMSI, type, SOG, COG, status, destination, dist).
- **M3 — Polish:** range in **nautical miles**; List/Stats views; name labels; zone/proximity alerts
  (e.g. a vessel entering the harbour) with the speaker ping; themes; settings in NVS; captive
  portal for WiFi + **aisstream API key**; OTA.

## Conventions & guardrails (same spirit as Capsule Radar)
- C++17. Keep the render path non-blocking — **no network in the LVGL loop**; all I/O lives in
  `ais_task` on core 0. Touch the shared ship vector only under the mutex.
- All shared-I2C reads on **core 1 only** (like Capsule Radar).
- All tunables in `config.h`; no magic numbers in render code.
- HTTPS/WSS: `setInsecure()` is acceptable for a hobby device; note the choice in code.
- The AIS API is free / non-commercial — gentle use, honest User-Agent, respect its terms. Keep the
  device firmware open-source (MIT, like Capsule Radar). Don't commercialize (data-licence terms).
- **Store the aisstream API key in NVS**, entered via the captive portal / web config — never commit it.
