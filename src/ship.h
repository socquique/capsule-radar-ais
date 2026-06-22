#pragma once
// Vessel (AIS) data model + small presentation helpers.
// Replaces the aircraft.h of the aircraft project. See docs/ARCHITECTURE.md.
#if defined(ARDUINO)
  #include <Arduino.h>
#else
  // Native build (SDL simulator): minimal shim so the data model compiles off-device.
  #include <string>
  #include <cstdint>
  using String = std::string;
#endif
#include <stdint.h>
#include <math.h>

// AIS navigation status codes (ITU-R M.1371). Only the common ones are named.
enum AisNavStatus {
    NAV_UNDERWAY_ENGINE   = 0,
    NAV_AT_ANCHOR         = 1,
    NAV_NOT_UNDER_COMMAND = 2,
    NAV_RESTRICTED_MANOEUV= 3,
    NAV_CONSTRAINED_DRAUGHT = 4,
    NAV_MOORED            = 5,
    NAV_AGROUND           = 6,
    NAV_FISHING           = 7,
    NAV_UNDERWAY_SAILING  = 8,
    NAV_AIS_SART          = 14,
    NAV_UNDEFINED         = 15,
};

struct Ship {
    uint32_t mmsi = 0;          // stable key (MetaData.MMSI / Message.*.UserID)
    String   name;              // vessel name (ShipStaticData.Name / MetaData.ShipName), trimmed
    String   dest;              // destination (optional)
    double   lat = 0, lon = 0;
    float    sogKt   = NAN;      // speed over ground (knots)
    float    cogDeg  = NAN;      // course over ground (deg) -> glyph rotation
    float    headingDeg = NAN;   // TrueHeading (511/NaN -> fall back to cog)
    uint8_t  navStatus = NAV_UNDEFINED;  // 0 under way, 1 anchored, 5 moored, 7 fishing...
    uint16_t type = 0;          // AIS ship type code -> category/colour (0 = unknown)
    uint16_t lengthM = 0;       // from Dimension A+B (optional)
    uint16_t beamM   = 0;       // from Dimension C+D (optional)
    uint32_t lastSeenMs = 0;    // millis() of last message (expiry + staleness)
};

// ----------------------------- presentation -----------------------------

// Pack RGB into RGB565 for the panel.
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// A vessel worth flagging on the scope (not-under-command / restricted / aground / SART).
inline bool shipIsAlert(uint8_t navStatus) {
    return navStatus == NAV_NOT_UNDER_COMMAND ||
           navStatus == NAV_RESTRICTED_MANOEUV ||
           navStatus == NAV_AGROUND ||
           navStatus == NAV_AIS_SART;
}

// A stationary vessel (anchored/moored) — drawn as a quiet dot rather than an arrow.
inline bool shipIsStationary(uint8_t navStatus, float sogKt) {
    if (navStatus == NAV_AT_ANCHOR || navStatus == NAV_MOORED) return true;
    if (!isnan(sogKt) && sogKt < 0.5f) return true;   // SOG ~0 -> effectively stopped
    return false;
}

// ---- colour by NAVIGATION STATUS ----
inline uint16_t navStatusColor565(uint8_t s) {
    switch (s) {
        case NAV_UNDERWAY_ENGINE:    return rgb565(0x39, 0xFF, 0x14); // green
        case NAV_UNDERWAY_SAILING:   return rgb565(0x3C, 0xE0, 0xFF); // cyan
        case NAV_AT_ANCHOR:          return rgb565(0xFF, 0xB2, 0x3C); // amber
        case NAV_MOORED:             return rgb565(0x8A, 0x93, 0xA6); // grey-blue
        case NAV_FISHING:            return rgb565(0xFF, 0x8A, 0x1E); // orange
        case NAV_NOT_UNDER_COMMAND:
        case NAV_RESTRICTED_MANOEUV:
        case NAV_CONSTRAINED_DRAUGHT:
        case NAV_AGROUND:
        case NAV_AIS_SART:           return rgb565(0xFF, 0x5A, 0x3C); // red (caution)
        default:                     return rgb565(0x9A, 0xFF, 0xC8); // soft (undefined)
    }
}

inline const char* navStatusName(uint8_t s) {
    switch (s) {
        case NAV_UNDERWAY_ENGINE:     return "Under way";
        case NAV_AT_ANCHOR:           return "At anchor";
        case NAV_NOT_UNDER_COMMAND:   return "Not under cmd";
        case NAV_RESTRICTED_MANOEUV:  return "Restricted";
        case NAV_CONSTRAINED_DRAUGHT: return "Constrained";
        case NAV_MOORED:              return "Moored";
        case NAV_AGROUND:             return "Aground";
        case NAV_FISHING:             return "Fishing";
        case NAV_UNDERWAY_SAILING:    return "Sailing";
        case NAV_AIS_SART:            return "SART";
        default:                      return "Unknown";
    }
}

// ---- colour + category by SHIP TYPE code (AIS type ranges) ----
// Category indices, also used by the list/legend.
enum ShipCategory {
    CAT_OTHER = 0, CAT_CARGO, CAT_TANKER, CAT_PASSENGER, CAT_HIGHSPEED,
    CAT_FISHING, CAT_SAILING, CAT_SERVICE, CAT_COUNT
};

inline uint8_t shipCategory(uint16_t type) {
    if (type >= 70 && type <= 79) return CAT_CARGO;
    if (type >= 80 && type <= 89) return CAT_TANKER;
    if (type >= 60 && type <= 69) return CAT_PASSENGER;
    if (type >= 40 && type <= 49) return CAT_HIGHSPEED;
    if (type == 30)               return CAT_FISHING;
    if (type == 36 || type == 37) return CAT_SAILING;       // sailing / pleasure craft
    if (type >= 50 && type <= 59) return CAT_SERVICE;       // pilot/tug/dredger/law/etc.
    return CAT_OTHER;
}

inline uint16_t shipTypeColor565(uint16_t type) {
    switch (shipCategory(type)) {
        case CAT_CARGO:     return rgb565(0x66, 0xD1, 0x7A); // green
        case CAT_TANKER:    return rgb565(0xFF, 0x5A, 0x3C); // red
        case CAT_PASSENGER: return rgb565(0x3C, 0xE0, 0xFF); // cyan
        case CAT_HIGHSPEED: return rgb565(0xFF, 0xB2, 0x3C); // amber
        case CAT_FISHING:   return rgb565(0xFF, 0x8A, 0x1E); // orange
        case CAT_SAILING:   return rgb565(0xC8, 0xFF, 0x3C); // lime
        case CAT_SERVICE:   return rgb565(0x8A, 0x93, 0xA6); // grey-blue
        default:            return rgb565(0x9A, 0xFF, 0xC8); // soft (unknown)
    }
}

inline const char* shipTypeName(uint16_t type) {
    switch (shipCategory(type)) {
        case CAT_CARGO:     return "Cargo";
        case CAT_TANKER:    return "Tanker";
        case CAT_PASSENGER: return "Passenger";
        case CAT_HIGHSPEED: return "High-speed";
        case CAT_FISHING:   return "Fishing";
        case CAT_SAILING:   return "Sailing";
        case CAT_SERVICE:   return "Service";
        default:            return "Vessel";
    }
}

// Pick the colour for the active colour mode (0 = nav status, 1 = ship type).
inline uint16_t shipColor565(const Ship& s, int colorMode) {
    return colorMode == 1 ? shipTypeColor565(s.type) : navStatusColor565(s.navStatus);
}
