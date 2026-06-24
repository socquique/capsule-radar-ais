// Capsule Radar — Marine (AIS) — entry point / glue.
// Same board + plumbing as the aircraft Capsule Radar; the network layer is a
// persistent WSS WebSocket to aisstream (ais_client) instead of HTTP ADS-B polling.
//   Core 0 (ais_task): WiFi keepalive + WebSocket pump (AisClient owns the vessel
//                       table, updated under a mutex inside its message callback).
//   Core 1 (loop):     LVGL render; reads vessels via AisClient::snapshot(); HUD;
//                       web config; OTA; brightness/IMU. No network here.
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include <set>
#include <string>
#include "config.h"
#include "ship.h"
#include "geo.h"
#include "ais_client.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"                 // CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "gps.h"                     // LC76G GNSS (-G variant only)
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include <WiFiManager.h>             // captive portal
#include <Preferences.h>            // NVS (persist settings + API key)
#include <time.h>                   // NTP/RTC clock + date
#include <WebServer.h>              // configuration web page
#include <ESPmDNS.h>                // http://capsule-marine.local
#include <ArduinoOTA.h>             // OTA firmware update over WiFi
#include <Update.h>                 // browser OTA: self-flash an uploaded .bin
#include <esp_heap_caps.h>

#define NVS_NS "capmarine"

// ---- shared state ----
static AisClient            g_ais;
static RadarSettings        g_settings;
static WiFiManager          g_wm;
static String               g_apiKey;                              // aisstream key (NVS)
static int                  g_brightnessDay = BRIGHTNESS_DEFAULT;  // user brightness (web/NVS)
static int                  g_volume = 60;                         // alert volume 0..100 (web/NVS)
static bool                 g_muted  = false;                      // mute alert pings
static int                  g_alertMode = 2;                       // 0=off 1=alerts(NUC/aground) 2=new+alerts
static float                g_proximityNm = 0.0f;                  // proximity alert radius, nm (0=off)
static uint32_t             g_idleDimMs = IDLE_DIM_MS;             // dim after this idle time (0 = never)
static bool                 g_showSweep = true;                    // rotating sweep line on/off
static int                  g_colorMode = COLOR_MODE_DEFAULT;      // 0=nav status 1=ship type
static uint32_t             g_watchMmsi = 0;                       // watched vessel ("a friend's boat"); 0 = off
static int                  g_rotation = 0;                        // display rotation 0/1/2/3
static bool                 g_useGps = false;                      // auto-set home from the LC76G GPS
static int                  g_trailLen = 2;                        // vessel trails 0=off 1=short 2=med 3=long
static volatile bool        g_onBattery = false;                   // discharging (set on core 1, read on core 0)
static bool                 g_rtcSynced = false;                   // RTC written from NTP this session?
static std::vector<Ship>    g_snap;                                // last snapshot (render + audio)
static volatile uint32_t    g_rebootAtMs = 0;                      // !=0: reboot when millis() reaches it
static String               g_tz = TZ_STR;                         // POSIX timezone (web-configurable, NVS)
static WiFiManagerParameter *g_apiKeyParam = nullptr;              // captive-portal field for the AIS key

// Web-selectable time zones (label + POSIX TZ). Copied from the aircraft project.
static const struct { const char *label; const char *tz; int offMin; int dst; } TZOPTS[] = {
    {"UTC",                      "UTC0",                              0, 0},
    {"London / Lisbon",          "GMT0BST,M3.5.0/1,M10.5.0",          0, 1},
    {"Madrid / Paris / Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3",       60, 1},
    {"Athens / Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4",     120, 1},
    {"New York (US Eastern)",    "EST5EDT,M3.2.0,M11.1.0",          -300, 1},
    {"Chicago (US Central)",     "CST6CDT,M3.2.0,M11.1.0",          -360, 1},
    {"Denver (US Mountain)",     "MST7MDT,M3.2.0,M11.1.0",          -420, 1},
    {"Phoenix (Arizona)",        "MST7",                            -420, 0},
    {"Los Angeles (US Pacific)", "PST8PDT,M3.2.0,M11.1.0",          -480, 1},
    {"Anchorage (Alaska)",       "AKST9AKDT,M3.2.0,M11.1.0",        -540, 1},
    {"Honolulu (Hawaii)",        "HST10",                           -600, 0},
    {"Argentina / Brazil (E)",   "<-03>3",                          -180, 0},
    {"India (IST)",              "<+0530>-5:30",                     330, 0},
    {"China / Singapore",        "<+08>-8",                          480, 0},
    {"Japan / Korea",            "JST-9",                            540, 0},
    {"Sydney (AU Eastern)",      "AEST-10AEDT,M10.1.0,M4.1.0/3",     600, 1},
    {"Auckland (NZ)",            "NZST-12NZDT,M9.5.0,M4.1.0/3",      720, 1},
};
static const int TZOPTS_N = sizeof(TZOPTS) / sizeof(TZOPTS[0]);

// ---- networking task (core 0): pump the WebSocket, never touch the display ----
static void ais_task(void*) {
    bool wasConnected = false;
    uint32_t lastWsOk = millis();             // self-heal: time of last WS-connected (or no-WiFi) tick
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        if (conn && !wasConnected) {
            Serial.printf("[wifi] up, IP %s\n", WiFi.localIP().toString().c_str());
            configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");
            Serial.println("[web] config: http://capsule-marine.local/  (or the IP above)");
        }
        wasConnected = conn;

        g_ais.loop();                          // pump the WSS WebSocket (connect/subscribe/recv/reconnect)

        // self-heal: a long stretch with WiFi up but no WS connection usually means the
        // internal heap fragmented and the TLS handshake can't allocate -> reboot to recover.
        if (!conn || g_ais.connected()) lastWsOk = millis();
        else if (millis() - lastWsOk > 180000UL) {
            Serial.println("[ais] WS down >180s with WiFi up -> restarting to recover");
            delay(100);
            ESP.restart();
        }
        // stale-feed self-heal: socket "connected" but no AIS message for a long time
        // (seen after aisstream outages) -> drop + reopen to force a fresh subscription.
        if (conn && g_ais.connected() &&
            (millis() - g_ais.lastActivityMs()) > (uint32_t)AIS_STALE_RECONNECT_MS) {
            g_ais.forceReconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(10));         // tight loop: the WS lib needs frequent service
    }
}

static void loadSettings() {
    Preferences p;
    p.begin(NVS_NS, true);
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
    g_settings.rangeNm = p.getFloat("rangeNm", RANGE_NM_DEFAULT);
    g_colorMode        = p.getInt("colormode", COLOR_MODE_DEFAULT);
    g_watchMmsi        = p.getUInt("watchmmsi", 0);
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_muted            = p.getBool("mute", false);
    g_alertMode        = p.getInt("alertmode", 2);
    g_proximityNm      = p.getFloat("proxnm", 0.0f);
    g_useGps           = p.getBool("usegps", false);
    g_trailLen         = p.getInt("traillen", 2);
    g_idleDimMs        = p.getUInt("idledim", IDLE_DIM_MS);
    g_tz               = p.getString("tz", TZ_STR);
    g_apiKey           = p.getString("aiskey", AIS_API_KEY_FALLBACK);
    p.end();
    g_settings.colorMode = g_colorMode;
}

// Audio alerts. g_alertMode: 0 = off, 1 = alerts only (NUC/aground/SART), 2 = new vessel + alerts.
// g_proximityNm > 0 also pings (once) when any vessel crosses into that radius.
static void checkAudioEvents() {
    if (!audio_present()) return;
    static std::set<uint32_t> seen, seenProx;
    static bool first = true;
    static uint32_t lastNew = 0;
    std::set<uint32_t> now, nowProx;
    static bool watchPresent = false;
    bool watchNow = false;
    for (const Ship &v : g_snap) {
        // watched vessel: ping once when it (re)appears anywhere in the snapshot
        if (g_watchMmsi && v.mmsi == g_watchMmsi) {
            watchNow = true;
            if (!watchPresent) { audio_play(AUDIO_ALERT); Serial.printf("[ais] watched vessel %u appeared\n", g_watchMmsi); }
        }
        const double d = geo::kmToNm(geo::haversineKm(g_settings.homeLat, g_settings.homeLon, v.lat, v.lon));
        if (d > g_settings.rangeNm) continue;                 // in-range only
        now.insert(v.mmsi);
        const bool isNew = !first && !seen.count(v.mmsi);
        const bool alert = shipIsAlert(v.navStatus);

        if (g_proximityNm > 0.0f && d <= g_proximityNm) {     // proximity: fire once on entry
            nowProx.insert(v.mmsi);
            if (!first && !seenProx.count(v.mmsi)) audio_play(AUDIO_ALERT);
        }
        if (isNew) {
            if (alert) { if (g_alertMode >= 1) audio_play(AUDIO_ALERT); }
            else if (g_alertMode >= 2 && millis() - lastNew > 3000) {
                audio_play(AUDIO_NEW);
                lastNew = millis();
            }
        }
    }
    seen.swap(now);
    seenProx.swap(nowProx);
    watchPresent = watchNow;
    first = false;
}

// Zoom button: change the display range, persist it, rebuild the AIS bounding box.
static void onRangeChange(float nm) {
    g_settings.rangeNm = nm;
    Preferences p; p.begin(NVS_NS, false); p.putFloat("rangeNm", nm); p.end();
    g_ais.setRange(nm);                  // re-subscribe with a matching box (safe on core 0 next loop)
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_set_range_nm(nm);
    ui_on_data_updated();
}

static void saveTheme(int t) {
    Preferences p; p.begin(NVS_NS, false); p.putInt("theme", t); p.end();
}

static time_t utc_to_time(struct tm *utc) {
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(utc);
    setenv("TZ", g_tz.c_str(), 1); tzset();
    return t;
}

static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { Serial.println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[rtc] system clock seeded from RTC");
}

static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE < b) b = BRIGHTNESS_IDLE;
    if (g_asleep) b = 0;
    display::setBrightness(b);
}

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

static void handleRoot() {
    const int th = radar::theme();
    String ropts;
    const int nR = (int)(sizeof(RANGE_STEPS_NM) / sizeof(RANGE_STEPS_NM[0]));
    for (int i = 0; i < nR; ++i) {
        const int r = (int)RANGE_STEPS_NM[i];
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%d nm</option>",
                 r, (r == (int)(g_settings.rangeNm + 0.5f)) ? " selected" : "", r);
        ropts += o;
    }
    const char *tnames[] = {"Phosphor", "Orb", "Amber CRT", "Military"};
    String topts;
    for (int i = 0; i < 4; ++i) {
        char o[80]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", tnames[i]); topts += o;
    }
    const char *cnames[] = {"By navigation status", "By ship type"};
    String copts;
    for (int i = 0; i < 2; ++i) {
        char o[80]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_colorMode ? " selected" : "", cnames[i]); copts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300, 1800, 3600};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if      (sV < 60)   snprintf(lbl, sizeof(lbl), "%d s", sV);
        else if (sV < 3600) snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        else                snprintf(lbl, sizeof(lbl), "%d h", sV / 3600);
        char o[96]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl); iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    const char *rnames[] = {"0\xc2\xb0 (default)", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
    String rotopts;
    for (int i = 0; i < 4; ++i) {
        char o[64]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_rotation ? " selected" : "", rnames[i]); rotopts += o;
    }
    const char *tlnames[] = {"Off", "Short", "Medium", "Long"};
    String tlopts;
    for (int i = 0; i < 4; ++i) {
        char o[64]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trailLen ? " selected" : "", tlnames[i]); tlopts += o;
    }
    const char *anames[] = {"Off", "Alerts only (NUC / aground)", "New vessel + alerts"};
    String aopts;
    for (int i = 0; i < 3; ++i) {
        char o[80]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_alertMode ? " selected" : "", anames[i]); aopts += o;
    }
    const int proxNm[] = {0, 1, 2, 5, 10};
    String popts;
    for (int pv : proxNm) {
        const bool sel = (pv == 0) ? (g_proximityNm <= 0.0f) : (fabsf(g_proximityNm - pv) < 0.4f);
        char lbl[24]; if (pv == 0) snprintf(lbl, sizeof(lbl), "Off"); else snprintf(lbl, sizeof(lbl), "%d nm", pv);
        char o[80]; snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", pv, sel ? " selected" : "", lbl); popts += o;
    }
    String tzopts;
    for (int i = 0; i < TZOPTS_N; ++i) {
        char o[128];
        snprintf(o, sizeof(o), "<option value=%d data-off=%d data-dst=%d%s>%s</option>",
                 i, TZOPTS[i].offMin, TZOPTS[i].dst, g_tz == TZOPTS[i].tz ? " selected" : "", TZOPTS[i].label);
        tzopts += o;
    }
    String gpsRow;
    if (gps_present()) {
        gpsRow  = "<label><input type=checkbox class=ck ";
        gpsRow += g_useGps ? "checked" : "";
        gpsRow += " onchange='gp(this.checked)'>Use GPS for location</label>";
    }
    char watchStr[12];
    if (g_watchMmsi) snprintf(watchStr, sizeof(watchStr), "%u", g_watchMmsi); else watchStr[0] = '\0';

    static const size_t BUFSZ = 11264;
    static char *buf = (char *)ps_malloc(BUFSZ);   // PSRAM: keep this big page off the scarce internal heap
    if (!buf) return;
    snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar Marine</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>Capsule Radar Marine</h1><p class=sub>Live AIS ship radar &middot; configuration</p></div></div>"
        "<div class=card><div class=t>AIS feed</div><form method=POST action=/save>"
        "<label>aisstream.io API key</label><input name=aiskey value='%s' placeholder='paste your free key'>"
        "<div style='font-size:12px;opacity:.6;margin:-2px 0 6px'>Free, non-commercial. Get one at "
        "<a href='https://aisstream.io' target=_blank style='color:#9affc8'>aisstream.io</a> &rarr; Create API Key. Stored on the device only.</div>"
        "<label>Watch a vessel (MMSI) &mdash; optional</label><input name=watch value='%s' placeholder='e.g. 224071970'>"
        "<div style='font-size:12px;opacity:.6;margin:-2px 0 6px'>Highlights it on the scope and pings when it appears (a friend's boat). Blank = off.</div>"
        "<div class=t style='margin-top:14px'>Location &amp; range</div>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        "%s"
        "<label>Display range</label><select name=range>%s</select>"
        "<label>Colour vessels</label><select name=color>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<label>Time zone</label><select name=tz>%s</select>"
        "<button>Save &amp; restart</button></form></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='sw(this.checked)'>Show radar sweep</label>"
        "<label>Vessel trails</label><select onchange='tl(this.value)'>%s</select>"
        "<label>Screen rotation (USB-C position)</label><select onchange='ro(this.value)'>%s</select></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input type=range min=0 max=100 value='%d' oninput='v(this.value,0)' onchange='v(this.value,1)'>"
        "<label><input type=checkbox class=ck %s onchange='m(this.checked)'>Mute alerts</label>"
        "<label>Alert on</label><select onchange='al(this.value)'>%s</select>"
        "<label>Proximity alert</label><select onchange='px(this.value)'>%s</select>"
        "<button type=button class=sec onclick='t()'>Test ping</button></div>"
        "<div class=card><div class=t>Network</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        "<p class=ft>Reach me at <code>capsule-marine.local</code> &middot; <a href=/update style='color:#9affc8'>Firmware update</a> &middot; v" FW_VERSION "</p>"
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function sw(c){fetch('/sweep?v='+(c?1:0)+'&save=1')}"
        "function tl(v){fetch('/trail?v='+v+'&save=1')}"
        "function ro(v){fetch('/rotate?v='+v+'&save=1')}"
        "function al(v){fetch('/alerts?mode='+v+'&save=1')}"
        "function px(v){fetch('/alerts?prox='+v+'&save=1')}"
        "function gp(c){fetch('/gps?v='+(c?1:0)+'&save=1')}"
        "var TZSET=%d;(function(){if(TZSET)return;"
        "var d=new Date(),j=new Date(d.getFullYear(),0,1).getTimezoneOffset(),"
        "u=new Date(d.getFullYear(),6,1).getTimezoneOffset(),o=-Math.max(j,u),s=(j!=u)?1:0,"
        "e=document.querySelector('select[name=tz]'),b=-1,i;"
        "for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o&&+e.options[i].dataset.dst===s){b=i;break;}}"
        "if(b<0)for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o){b=i;break;}}"
        "if(b>=0)e.selectedIndex=b;})();</script></body></html>",
        g_apiKey.c_str(), watchStr, g_settings.homeLat, g_settings.homeLon, gpsRow.c_str(),
        ropts.c_str(), copts.c_str(), topts.c_str(), tzopts.c_str(),
        g_brightnessDay, iopts.c_str(), g_showSweep ? "checked" : "",
        tlopts.c_str(), rotopts.c_str(),
        g_volume, g_muted ? "checked" : "", aopts.c_str(), popts.c_str(),
        g_settings.homeLat, g_settings.homeLon, (g_tz == TZ_STR ? 0 : 1));
    g_web.send(200, "text/html", buf);
}

static void handleSave() {
    Preferences p;
    p.begin(NVS_NS, false);
    if (g_web.hasArg("aiskey")) p.putString("aiskey", g_web.arg("aiskey"));
    if (g_web.hasArg("watch"))  p.putUInt("watchmmsi", (uint32_t)strtoul(g_web.arg("watch").c_str(), nullptr, 10));
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) p.putDouble("homeLat", lat);
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) p.putDouble("homeLon", lon);
    }
    if (g_web.hasArg("range")) p.putFloat("rangeNm", g_web.arg("range").toFloat());
    if (g_web.hasArg("color")) p.putInt("colormode", g_web.arg("color").toInt());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    if (g_web.hasArg("tz")) {
        const int i = g_web.arg("tz").toInt();
        if (i >= 0 && i < TZOPTS_N) p.putString("tz", TZOPTS[i].tz);
    }
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='4;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>CapsuleMarine-Setup</b> network to reconfigure.</body>");
    delay(400);
    g_wm.resetSettings();
    ESP.restart();
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putInt("bright", g_brightnessDay); p.end(); }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putInt("vol", g_volume); p.putBool("mute", g_muted); p.end(); }
    if (g_web.hasArg("test")) audio_play(AUDIO_NEW);
    g_web.send(200, "text/plain", "ok");
}

static void handleAlerts() {
    if (g_web.hasArg("mode")) g_alertMode   = constrain((int)g_web.arg("mode").toInt(), 0, 2);
    if (g_web.hasArg("prox")) g_proximityNm = g_web.arg("prox").toFloat();   // nm (0 = off)
    if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putInt("alertmode", g_alertMode); p.putFloat("proxnm", g_proximityNm); p.end(); }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putUInt("idledim", g_idleDimMs); p.end(); }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSweep() {
    if (g_web.hasArg("v")) {
        g_showSweep = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_showSweep);
        if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putBool("sweep", g_showSweep); p.end(); }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrail() {
    if (g_web.hasArg("v")) {
        g_trailLen = constrain((int)g_web.arg("v").toInt(), 0, 3);
        radar::setTrailLength(g_trailLen);
        if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putInt("traillen", g_trailLen); p.end(); }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotate() {
    if (g_web.hasArg("v")) {
        g_rotation = constrain((int)g_web.arg("v").toInt(), 0, 3);
        display::setRotation((uint8_t)g_rotation);
        if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putInt("rot", g_rotation); p.end(); }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGps() {
    if (g_web.hasArg("v")) {
        g_useGps = g_web.arg("v").toInt() != 0;
        if (g_web.hasArg("save")) { Preferences p; p.begin(NVS_NS, false); p.putBool("usegps", g_useGps); p.end(); }
    }
    g_web.send(200, "text/plain", "ok");
}

// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static void handleUpdatePage() {
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar Marine - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg{margin-top:10px;color:#9affc8;font-size:13px}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1><div class=card>"
        "<p>Upload the <b>app firmware</b> from the release. Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<p style='text-align:center;margin-top:14px'><a href=/>&larr; Back to settings</a></p>"
        "<script>function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nCapsule Radar Marine boot");
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    loadSettings();

    // --- Display + LVGL ---------------------------------------------------
    if (!display::begin()) Serial.println("[!] display::begin() failed — check QSPI pins / power.");

    {   // restore saved theme + display prefs
        Preferences p;
        p.begin(NVS_NS, true);
        const int t = p.getInt("theme", THEME_PHOSPHOR);
        g_showSweep = p.getBool("sweep", true);
        g_rotation  = p.getInt("rot", 0);
        p.end();
        radar::setTheme(t);
        radar::setSweepEnabled(g_showSweep);
        radar::setColorMode(g_colorMode);
        radar::setTrailLength(g_trailLen);
        radar::setWatchMmsi(g_watchMmsi);
        display::setRotation((uint8_t)g_rotation);
    }
    radar::setThemeChangedCb(saveTheme);
    ui_set_range_cb(onRangeChange);
    ui_set_range_nm(g_settings.rangeNm);

    imu_begin();
    battery_begin();
    gps_begin();
    battery_enable_codec_rail();

    setenv("TZ", g_tz.c_str(), 1); tzset();
    rtc_begin();
    rtc_seed_clock();
    if (audio_begin()) { audio_set_volume(g_volume); audio_set_muted(g_muted); }

    // --- WiFi (captive portal, non-blocking) + aisstream API-key field ----
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("Capsule Radar Marine");
    static WiFiManagerParameter apiKeyHint(
        "<p style='font-size:13px;margin:8px 0 0'>Need a key? Get a free one at "
        "<a href='https://aisstream.io' target='_blank'>aisstream.io</a> &rarr; Create API Key.</p>");
    g_wm.addParameter(&apiKeyHint);
    g_apiKeyParam = new WiFiManagerParameter("aiskey", "aisstream.io API key", g_apiKey.c_str(), 48);
    g_wm.addParameter(g_apiKeyParam);
    g_wm.setCustomHeadElement(
        "<style>body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}</style>");
    g_wm.setSaveConfigCallback([]() {
        // persist the API key entered in the portal, then reboot for a clean web/mDNS start
        if (g_apiKeyParam && strlen(g_apiKeyParam->getValue()) > 0) {
            Preferences p; p.begin(NVS_NS, false); p.putString("aiskey", g_apiKeyParam->getValue()); p.end();
            Serial.println("[ais] API key saved from setup portal");
        }
        Serial.println("[wifi] new credentials saved -> rebooting");
        g_rebootAtMs = millis() + 2500;
    });
    if (g_wm.autoConnect("CapsuleMarine-Setup"))
        Serial.println("[wifi] connected");
    else
        Serial.println("[wifi] config portal open - join 'CapsuleMarine-Setup'; UI stays live");

    // --- AIS client + task -------------------------------------------------
    g_ais.begin(g_apiKey, g_settings.homeLat, g_settings.homeLon, g_settings.rangeNm);
    xTaskCreatePinnedToCore(ais_task, "ais", 16384, nullptr, 1, nullptr, 0);  // TLS needs a big stack

    // configuration web page (http://capsule-marine.local/)
    g_web.on("/", handleRoot);
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/wifi", HTTP_POST, handleWifi);
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/alerts", handleAlerts);
    g_web.on("/idle", handleIdle);
    g_web.on("/sweep", handleSweep);
    g_web.on("/trail", handleTrail);
    g_web.on("/rotate", handleRotate);
    g_web.on("/gps", handleGps);
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
        },
        handleUpdateUpload);
    g_web.begin();

    Serial.println("setup done");
}

void loop() {
    display::loop();
    g_wm.process();
    g_web.handleClient();
    if (g_useGps) gps_poll();

    if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) { delay(50); ESP.restart(); }

    static bool otaUp = false;
    if (!otaUp && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("capsule-marine");
        ArduinoOTA.begin();
        MDNS.addService("http", "tcp", 80);
        otaUp = true;
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
    }
    if (otaUp) ArduinoOTA.handle();

    // Pull a fresh vessel snapshot from the AIS client (mutex-guarded copy), render on core 1.
    static uint32_t lastSnap = 0;
    if (millis() - lastSnap > 1000) {            // ships move slowly: 1 Hz render cadence is plenty
        lastSnap = millis();
        g_ais.snapshot(g_snap);                  // copies + expires under the mutex
        radar::update(g_snap, g_settings);
        ui_on_data_updated();
        checkAudioEvents();
    }

    // periodic: HUD clock + wifi/battery/feed indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
#if DEBUG_MEM
        Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | vessels %d\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000), (int)g_snap.size());
#endif
        char clk[8] = "--:--";
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
            char date[20]; strftime(date, sizeof(date), "%d %b %Y", &ti);
            ui_set_date(date);
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // feed "fresh" = connected and we received an AIS message recently
        const bool feedFresh = g_ais.connected() && g_ais.lastMsgMs() != 0 &&
                               (millis() - g_ais.lastMsgMs() < 30000UL);
        ui_set_status(wifiUp, feedFresh, rssi, clk);
        Serial.printf("[ais] feed %s | %u tracked | %d shown | %d in range\n",
                      g_ais.connected() ? "up" : "down", (unsigned)g_ais.trackedCount(),
                      (int)g_snap.size(), radar::countInRange());
        char net[80];
        if (wifiUp) snprintf(net, sizeof(net), "Configure at\ncapsule-marine.local\n%s", WiFi.localIP().toString().c_str());
        else        snprintf(net, sizeof(net), "WiFi setup:\njoin CapsuleMarine-Setup");
        ui_set_netinfo(net);
        const bool bpresent = battery_present();
        ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !battery_charging();
        const int gpsState = (!g_useGps || !gps_present()) ? 0 : (gps_has_fix() ? 2 : 1);
        ui_set_gps(gpsState, gps_satellites());
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr); struct tm utc; gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
        }
        if (g_useGps) {
            double glat, glon;
            if (gps_location(&glat, &glon) &&
                geo::haversineKm(g_settings.homeLat, g_settings.homeLon, glat, glon) > 1.0) {
                g_settings.homeLat = glat; g_settings.homeLon = glon;
                g_ais.setHome(glat, glon);     // re-subscribe around the new centre
                Serial.printf("[gps] re-centred to %.4f, %.4f\n", glat, glon);
            }
        }
    }

    // face-down -> screen off (IMU); idle auto-dim
    static uint32_t lastImu = 0;
    static int fdCount = 0;
    if (millis() - lastImu > 400) {
        lastImu = millis();
        const int fd = imu_facedown();
        if (fd > 0)       { if (fdCount < 8) fdCount++; }
        else if (fd == 0) fdCount = 0;
        const bool sleep = (fdCount >= 4);
        const bool idle  = g_idleDimMs > 0 && display::inactiveMs() > g_idleDimMs;
        if (sleep != g_asleep || idle != g_idle) { g_asleep = sleep; g_idle = idle; applyBrightness(); }
    }

    delay(5);
}
