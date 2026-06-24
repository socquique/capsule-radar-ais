// Persistent WSS client to aisstream.io. Opens one secure WebSocket, sends the
// bounding-box subscription on connect, then receives pushed AIS messages and merges
// them by MMSI into a mutex-guarded table. Memory-safe: ArduinoJson parses into PSRAM
// (a persistent WS means one TLS handshake, unlike the aircraft project's repeated
// HTTPS polls — but that one handshake still needs a contiguous internal block, so we
// keep all JSON/parse buffers in PSRAM). See docs/ARCHITECTURE.md.
#include "ais_client.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>

// Parse AIS JSON in PSRAM, not internal RAM, so the per-message alloc/free churn never
// fragments the internal heap that mbedTLS needs for the (re)connect TLS handshake.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

static WebSocketsClient s_ws;
static AisClient*       s_self = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;

// Trim AIS string padding: trailing spaces and '@' fill characters.
static void trimAis(String& s) {
    int end = (int)s.length();
    while (end > 0) {
        char c = s[end - 1];
        if (c == ' ' || c == '@' || c == '\0') end--;
        else break;
    }
    if (end < (int)s.length()) s.remove(end);
}

void AisClient::begin(const String& apiKey, double homeLat, double homeLon, float rangeNm) {
    _apiKey = apiKey;
    _lat = homeLat; _lon = homeLon; _rangeNm = rangeNm;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    s_self = this;

    if (_apiKey.length() == 0) {
        Serial.println("[ais] no API key set — feed disabled (enter it in the web config)");
        return;
    }

    s_ws.beginSSL(AIS_HOST, AIS_PORT, AIS_PATH);   // WSS. TLS validation: see AIS_TLS_INSECURE note below.
    s_ws.onEvent(AisClient::eventTrampoline);
    // The library drives reconnect backoff; bound it between our floor and ceiling.
    s_ws.setReconnectInterval(AIS_RECONNECT_MIN_MS);
    s_ws.enableHeartbeat(15000, 3000, 2);          // ping every 15 s; drop after 2 missed pongs
    _begun = true;
    Serial.printf("[ais] WSS client started -> wss://%s%s\n", AIS_HOST, AIS_PATH);
}

void AisClient::setHome(double lat, double lon) {
    _lat = lat; _lon = lon;
    if (_connected) sendSubscription();
}

void AisClient::setRange(float rangeNm) {
    _rangeNm = rangeNm;
    if (_connected) sendSubscription();
}

void AisClient::loop() {
    if (_begun) s_ws.loop();
}

// Drop and reopen the secure WebSocket. Used by the stale-feed watchdog: aisstream
// can keep a socket "connected" yet stream nothing (we saw this during an outage and
// after recovery) — a fresh connection re-sends the subscription and gets data flowing.
void AisClient::forceReconnect() {
    if (!_begun) return;
    Serial.println("[ais] forcing WSS reconnect (stale feed)");
    _connected = false;
    s_ws.disconnect();
    s_ws.beginSSL(AIS_HOST, AIS_PORT, AIS_PATH);   // re-initiate; CONNECTED -> re-subscribe
    _lastConnectMs = millis();                      // reset the staleness clock
}

// Build + send the subscription JSON. aisstream wants the box as
// [[ [SW-lat, SW-lon], [NE-lat, NE-lon] ]].
void AisClient::sendSubscription() {
    if (_apiKey.length() == 0) return;
    const double nm = (double)_rangeNm * AIS_BOX_MARGIN;       // box half-extent
    const double dLat = nm / 60.0;                              // 1 deg lat ~= 60 nm
    const double cosL = cos(_lat * M_PI / 180.0);
    const double dLon = nm / (60.0 * (cosL > 0.01 ? cosL : 0.01));
    const double latMin = _lat - dLat, latMax = _lat + dLat;
    const double lonMin = _lon - dLon, lonMax = _lon + dLon;

    char msg[320];
    snprintf(msg, sizeof(msg),
        "{\"APIKey\":\"%s\",\"BoundingBoxes\":[[[%.5f,%.5f],[%.5f,%.5f]]],"
        "\"FilterMessageTypes\":[\"PositionReport\",\"ShipStaticData\"]}",
        _apiKey.c_str(), latMin, lonMin, latMax, lonMax);
    s_ws.sendTXT(msg);
    Serial.printf("[ais] subscribed box [%.4f,%.4f]..[%.4f,%.4f]\n", latMin, lonMin, latMax, lonMax);
}

void AisClient::eventTrampoline(int type, uint8_t* payload, size_t length) {
    if (s_self) s_self->onEvent(type, payload, length);
}

void AisClient::onEvent(int type, uint8_t* payload, size_t length) {
    switch ((WStype_t)type) {
        case WStype_CONNECTED:
            _connected = true;
            _lastConnectMs = millis();
            Serial.println("[ais] WebSocket connected");
            sendSubscription();
            break;
        case WStype_DISCONNECTED:
            _connected = false;
            Serial.println("[ais] WebSocket disconnected — will reconnect");
            break;
        case WStype_TEXT:
        case WStype_BIN:                 // aisstream pushes the JSON as BINARY frames
            ingest((const char*)payload, length);
            break;
        case WStype_ERROR:
            Serial.println("[ais] WebSocket error");
            break;
        default:
            break;   // BIN / PING / PONG / fragments: ignore
    }
}

// Parse one AIS JSON frame and merge it into the table by MMSI.
void AisClient::ingest(const char* json, size_t len) {
    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        static int e = 0; if (e++ < 5) Serial.printf("[ais] json parse error: %s\n", err.c_str());  // rate-limited
        return;
    }

    // aisstream sends an error object (no MessageType) if the subscription/key is bad.
    const char* mtype = doc["MessageType"] | (const char*)nullptr;
    if (!mtype) {
        const char* e = doc["error"] | doc["Error"] | (const char*)nullptr;
        if (e) Serial.printf("[ais] server error: %s\n", e);
        return;
    }

    JsonObjectConst meta = doc["MetaData"];
    uint32_t mmsi = meta["MMSI"] | 0u;
    if (mmsi == 0) return;

    const uint32_t now = millis();
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    auto it = _ships.find(mmsi);
    if (it == _ships.end()) {
        if (_ships.size() >= (size_t)AIS_MAX_SHIPS) {   // busy harbour: protect RAM
            xSemaphoreGive(s_mutex);
            return;
        }
        Ship blank; blank.mmsi = mmsi;
        it = _ships.emplace(mmsi, blank).first;
    }
    Ship& sh = it->second;
    sh.mmsi = mmsi;
    sh.lastSeenMs = now;

    // Name is convenient on every message via MetaData.ShipName; keep it if non-empty.
    if (sh.name.length() == 0) {
        const char* nm = meta["ShipName"] | (const char*)nullptr;
        if (nm) { sh.name = nm; trimAis(sh.name); }
    }
    // Position from MetaData is present on every message — a good fallback.
    if (meta["latitude"].is<double>())  sh.lat = meta["latitude"].as<double>();
    if (meta["longitude"].is<double>()) sh.lon = meta["longitude"].as<double>();

    JsonObjectConst m = doc["Message"];
    if (strcmp(mtype, "PositionReport") == 0) {
        JsonObjectConst pr = m["PositionReport"];
        if (pr["Latitude"].is<double>())  sh.lat = pr["Latitude"].as<double>();
        if (pr["Longitude"].is<double>()) sh.lon = pr["Longitude"].as<double>();
        sh.cogDeg    = pr["Cog"] | NAN;
        sh.sogKt     = pr["Sog"] | NAN;
        float th     = pr["TrueHeading"] | 511.0f;
        sh.headingDeg = (th >= 0.0f && th < 360.0f) ? th : NAN;   // 511 = not available
        sh.navStatus = (uint8_t)(pr["NavigationalStatus"] | (int)NAV_UNDEFINED);
    } else if (strcmp(mtype, "ShipStaticData") == 0) {
        JsonObjectConst sd = m["ShipStaticData"];
        const char* nm = sd["Name"] | (const char*)nullptr;
        if (nm) { sh.name = nm; trimAis(sh.name); }
        sh.type = (uint16_t)(sd["Type"] | 0);
        const char* dst = sd["Destination"] | (const char*)nullptr;
        if (dst) { sh.dest = dst; trimAis(sh.dest); }
        JsonObjectConst dim = sd["Dimension"];
        if (!dim.isNull()) {
            sh.lengthM = (uint16_t)((dim["A"] | 0) + (dim["B"] | 0));
            sh.beamM   = (uint16_t)((dim["C"] | 0) + (dim["D"] | 0));
        }
        if (sd["MaximumStaticDraught"].is<float>()) sh.draughtM = sd["MaximumStaticDraught"].as<float>();
        JsonObjectConst eta = sd["Eta"];
        if (!eta.isNull()) {
            sh.etaDay  = (uint8_t)(eta["Day"]    | 0);
            sh.etaHour = (uint8_t)(eta["Hour"]   | 24);
            sh.etaMin  = (uint8_t)(eta["Minute"] | 60);
        }
    }

    _lastMsgMs = now;
    xSemaphoreGive(s_mutex);
}

void AisClient::snapshot(std::vector<Ship>& out) {
    out.clear();
    if (!s_mutex) return;
    const uint32_t now = millis();
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    out.reserve(_ships.size());
    for (auto it = _ships.begin(); it != _ships.end(); ) {
        if (now - it->second.lastSeenMs > (uint32_t)SHIP_STALE_MS) {
            it = _ships.erase(it);                 // expire vessels gone quiet
        } else {
            if (it->second.lat != 0.0 || it->second.lon != 0.0) out.push_back(it->second);
            ++it;
        }
    }
    xSemaphoreGive(s_mutex);
}

size_t AisClient::trackedCount() {
    if (!s_mutex) return 0;
    size_t n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    n = _ships.size();
    xSemaphoreGive(s_mutex);
    return n;
}

// --- TLS note ------------------------------------------------------------------
// AIS_TLS_INSECURE=1 (default) accepts aisstream's cert without pinning — fine for a
// hobby device. arduinoWebSockets' beginSSL() on ESP32 connects without CA validation
// unless beginSslWithCA() is used; to pin a root CA, switch to beginSslWithCA() here.
