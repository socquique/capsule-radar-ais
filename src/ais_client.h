#pragma once
// AIS feed client — maintains ONE persistent secure WebSocket (WSS) to aisstream.io,
// subscribes to a bounding box around home, parses PositionReport + ShipStaticData
// messages and merges them by MMSI into a shared, mutex-guarded vessel table.
// Replaces the aircraft project's HTTP-polling adsb_client. See docs/DATA_SOURCE.md.
//
// Lives on core 0 (ais_task). loop() must be called frequently. The render loop on
// core 1 reads vessels via snapshot() (copies the table under the mutex).
#include <vector>
#include <map>
#include "ship.h"

class AisClient {
public:
    // apiKey: aisstream key (from NVS / captive portal). Empty key -> never connects.
    void begin(const String& apiKey, double homeLat, double homeLon, float rangeNm);

    // Update home / range -> rebuild the bounding box and (if connected) re-subscribe.
    void setHome(double lat, double lon);
    void setRange(float rangeNm);

    // Pump the WebSocket. Call often from ais_task (core 0). Handles connect +
    // (re)subscribe + reconnect backoff (the underlying lib drives the backoff).
    void loop();

    // Copy the current (non-stale) vessels into `out` under the mutex. Safe to call
    // from the render loop (core 1). Expires vessels not seen in SHIP_STALE_MS.
    void snapshot(std::vector<Ship>& out);

    bool     connected() const { return _connected; }
    uint32_t lastMsgMs() const { return _lastMsgMs; }   // 0 until the first message
    // Most recent sign of life (last message, or last connect) — for stale-feed self-heal.
    uint32_t lastActivityMs() const { return _lastMsgMs > _lastConnectMs ? _lastMsgMs : _lastConnectMs; }
    void     forceReconnect();                           // drop + reopen the WSS (re-subscribes)
    size_t   trackedCount();                             // vessels currently in the table

private:
    void sendSubscription();
    void onEvent(int type, uint8_t* payload, size_t length);   // WStype_t as int
    void ingest(const char* json, size_t len);                 // parse one AIS frame
    static void eventTrampoline(int type, uint8_t* payload, size_t length);

    String   _apiKey;
    double   _lat = 0, _lon = 0;
    float    _rangeNm = 10.0f;
    bool     _connected = false;
    bool     _begun = false;
    uint32_t _lastMsgMs = 0;
    uint32_t _lastConnectMs = 0;
    std::map<uint32_t, Ship> _ships;   // keyed by MMSI
};
