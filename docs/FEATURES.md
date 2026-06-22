# Features (target)

Mirror Capsule Radar's polish, re-themed for vessels.

## Core
- **Live ship traffic** from a free AIS feed (aisstream.io) over a persistent WebSocket.
- Vessels drawn as **ship glyphs rotated by course (COG)**, colour-coded by **nav status** or
  **ship type**, with fading **trails** and an animated **sweep**.
- Out-of-range vessels as **edge markers** pointing their way (reuse the aircraft behaviour).
- Memory-safe parser with a **vessel cap** for busy harbours.

## Touch & detail
- **Tap a vessel** → detail card: **name, MMSI, type, speed (kt), course, heading, nav status,
  destination, distance, bearing**.
- Swipe between **Radar / List / Stats** circular views.
- On-screen **range** button cycles nm steps (e.g. 2 / 5 / 10 / 20 / 40 nm).
- Long-press to cycle **themes** (Phosphor / green grid / Amber / Military — reuse).

## Status HUD
- WiFi/feed indicator (amber if the WebSocket is down/stale), **vessels-in-range count**, clock,
  battery %, date. (Optional GPS satellite icon on the -G board.)

## Nice marine-specific touches
- **Colour by navigation status**: under way / at anchor / moored / fishing / sailing.
- **Anchored/moored vessels** as quiet dots; **moving** ones as arrows.
- **Name labels** for the nearest few (AIS gives real names — a big upgrade over ADS-B callsigns).
- **Zone / proximity alert**: speaker ping when a vessel enters the harbour / a set radius, or when a
  specific MMSI appears (watch a friend's boat).
- Inland mode is fine too — barges on navigable rivers/canals.

## Config & updates (reuse from Capsule Radar)
- Captive portal for WiFi **+ aisstream API key**; web page for home (map picker), range, colour
  mode, theme, brightness, sound, time zone (auto-detected), OTA. Settings persist in NVS.
- Browser-based **OTA** + PlatformIO espota.

## Power & time
- Battery aware (slower reconnect/idle on battery), RTC clock across power loss, NTP re-sync,
  idle auto-dim, face-down sleep (reuse).
