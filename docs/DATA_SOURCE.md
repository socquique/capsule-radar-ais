# Data source — AIS

## Primary: aisstream.io (free WebSocket)
- Sign up at **https://aisstream.io** for a **free API key** (non-commercial). No credit card.
- Endpoint: **`wss://stream.aisstream.io/v0/stream`** (secure WebSocket — needs `WiFiClientSecure`).
- You **open one persistent WebSocket**, send a **subscription** JSON as the first message, then the
  server **pushes** AIS messages as they arrive. No polling.

### Subscription message (send once, right after connect)
```json
{
  "APIKey": "<YOUR_KEY>",
  "BoundingBoxes": [ [ [latMin, lonMin], [latMax, lonMax] ] ],
  "FilterMessageTypes": ["PositionReport", "ShipStaticData"]
}
```
- `BoundingBoxes` is a list of boxes; each box is `[[south-west lat,lon],[north-east lat,lon]]`.
  Build it from the home position ± a delta (≈ range in nm → degrees: 1° lat ≈ 60 nm;
  1° lon ≈ 60·cos(lat) nm). Re-send the subscription if the user changes home/range.
- You can also filter by MMSI (`FiltersShipMMSI`) — not needed for an area radar.
- ⚠️ If the subscription is malformed or the key is bad, the server sends an **error message**
  then closes — log it. Reconnect with backoff on any close.

### Incoming message shape
Every message has a `MessageType`, a `MetaData` block, and a typed `Message` payload:
```json
{
  "MessageType": "PositionReport",
  "MetaData": { "MMSI": 211234560, "ShipName": "EXAMPLE NAME   ",
                "latitude": 38.81, "longitude": 0.12, "time_utc": "2026-06-22 ..." },
  "Message": { "PositionReport": {
       "UserID": 211234560, "Latitude": 38.81, "Longitude": 0.12,
       "Cog": 187.4, "Sog": 6.2, "TrueHeading": 186, "NavigationalStatus": 0,
       "RateOfTurn": 0 } }
}
```
Use **`MetaData.MMSI`** as the key (also in `Message.*.UserID`). `MetaData.latitude/longitude` and
`time_utc` are convenient (present on every message).

### Fields we use
**PositionReport** (the live track):
- `Latitude`, `Longitude` — position (project with `geo.*`).
- `Cog` — course over ground (deg) → **glyph rotation**.
- `Sog` — speed over ground (**knots**).
- `TrueHeading` — heading (deg, 511 = not available; fall back to Cog).
- `NavigationalStatus` — 0 under way using engine · 1 at anchor · 5 moored · 7 fishing · 8 under way
  sailing · 15 undefined (full table in the AIS spec) → **colour / icon**.
- `RateOfTurn` — optional.

**ShipStaticData** (slower-changing identity — merge by MMSI):
- `Name` (also `MetaData.ShipName`) — vessel name. Trim trailing spaces / `@`.
- `Type` — ship type code (e.g. 70-79 cargo, 80-89 tanker, 60-69 passenger, 30 fishing, 36 sailing,
  37 pleasure craft, 50 pilot, 52 tug...). Map ranges → category + colour.
- `Destination`, `Dimension` (A/B/C/D → length/beam), `MaximumStaticDraught`, `CallSign`, `ImoNumber`.

### Merge & expire
- Keep a `std::map<uint32_t, Ship>` (or vector) keyed by MMSI. PositionReport updates pose;
  ShipStaticData fills in name/type/destination. Stamp `lastSeen` on every message.
- **Expire** vessels not seen in ~**10–15 min** (ships report less often than planes — moving vessels
  every 2-10 s, anchored every 3 min). Don't drop too aggressively.
- Cap the list (e.g. 100) to protect RAM in busy harbours, like the ADS-B `ADSB_MAX_AIRCRAFT` cap.

### Coverage & etiquette
- aisstream aggregates **terrestrial** receivers → great near coasts, ports, busy straits and
  navigable rivers; sparse/none in open ocean. Set the default home somewhere coastal for the demo.
- Free tier / **non-commercial** — exactly this project. Be polite, identify honestly, reconnect with
  backoff (don't hammer on errors). Read aisstream's terms before publishing.

## Fallbacks / alternatives (future)
- **Self-hosted AIS receiver:** RTL-SDR + `rtl_ais`, or a **dAISy HAT/USB**, produces NMEA 0183
  (`!AIVDM`) locally — no internet, no API key, but needs an antenna near the water. Could feed the
  device over TCP/serial. Best coverage for a specific harbour.
- **MarineTraffic / VesselFinder APIs** — mostly **paid**; avoid for a free hobby device.
- **AISHub** — free but requires you to **contribute your own feed** to gain access.

## NMEA AIS note (if you go the local-receiver route)
Raw AIS is NMEA `!AIVDM` sentences with a 6-bit ASCII-armoured payload that must be **bit-unpacked**
per message type (1/2/3 position, 5 static, 18/19 class B, 24 static B). A library like
`AIS-catcher` (host) or a small on-device decoder handles this. The **aisstream JSON path avoids all
of this** — prefer it for v1.
