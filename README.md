# Capsule Radar — Marine (AIS) 🚢

Live **AIS ship radar** for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (round 466×466 AMOLED, touch).
It pulls nearby vessels from a free online **AIS** feed over WiFi and plots them on a glowing round
radar scope — rotated by course, colour-coded by ship type / navigation status, with fading trails
and tap-to-inspect (name, MMSI, type, speed, course, destination, status). Useful by the coast and
inland (navigable rivers/canals). Sibling of **Capsule Radar** (the aircraft version).

> **This folder is a documentation starter kit**, not code yet. It tells Claude Code how to build
> the firmware by reusing the finished aircraft project.

## Start here
1. Read **[CLAUDE.md](CLAUDE.md)** — master context.
2. **[docs/REUSE.md](docs/REUSE.md)** — what to copy from `/Users/quique/proyectos/PlaneRadar2.0/`
   (the working aircraft firmware for the same board) and what to build new.
3. **[docs/DATA_SOURCE.md](docs/DATA_SOURCE.md)** — the AIS feed (aisstream.io, free WebSocket) +
   message format and the fields we use.
4. **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — task/core split, `Ship` model, WebSocket
   client, and the **internal-RAM / TLS** budget (the project's recurring landmine).
5. **[docs/HARDWARE.md](docs/HARDWARE.md)** · **[docs/FEATURES.md](docs/FEATURES.md)**.

## One-line summary of the plan
Copy the aircraft firmware → swap `adsb_client` (HTTP polling) for **`ais_client`** (a persistent
**WSS WebSocket** to aisstream) → swap the `Aircraft` model + plane glyph for **`Ship`** + a ship
glyph → relabel the UI. Same board, same scope, same web/OTA/NVS plumbing.

## Build & run
- **Desktop simulator** (no hardware needed): `pio run -e native -t exec` — opens an SDL2
  window with the full UI (radar / list / stats, detail card, 4 themes) fed by mock vessels
  near Dénia. Keys: `T` theme · `C` colour mode (status↔type) · `R` range.
  Headless screenshots: `.pio/build/native/program --shot out` (with `SDL_VIDEODRIVER=dummy`).
- **Device firmware**: `pio run -e esp32-s3-amoled-175` (build) / `-t upload` (flash).
  Enter WiFi + your free **aisstream.io API key** in the `CapsuleMarine-Setup` captive portal
  (or later at `capsule-marine.local`). OTA: `pio run -e esp32-s3-amoled-175-ota -t upload`.

## Status
🟢 **M0–M2 done.** `src/` builds on both targets:
- **M0 bring-up** — board drivers copied verbatim; build system (PlatformIO native + device).
- **M1 AIS client** — `ais_client` opens the WSS WebSocket to aisstream, subscribes to a
  bounding box, parses `PositionReport` + `ShipStaticData`, merges/expires by MMSI. *Compiles;
  pending a flash + real API key to confirm the live feed.*
- **M2 live scope** — ship glyphs rotated by COG, coloured by nav status / ship type, fading
  trails, tap → detail card (name/MMSI/type/SOG/COG/heading/status/dest/dist/bearing), List &
  Stats views, 4 themes. **Verified in the simulator.**

Next (**M3**): flash + verify the live aisstream feed on hardware; edge markers in the phosphor
theme; zone/proximity alerts polish; on-screen colour-mode toggle.
