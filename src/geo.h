#pragma once
// Pure geo math — complete, framework-independent. See docs/DATA_SOURCE.md.
#include <math.h>

namespace geo {

inline double deg2rad(double d) { return d * M_PI / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / M_PI; }

// Great-circle distance in kilometers.
inline double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0088;
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(deg2rad(lat1)) * cos(deg2rad(lat2)) *
               sin(dlon / 2) * sin(dlon / 2);
    return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

// Initial bearing from point 1 to point 2, degrees clockwise from true north [0,360).
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    double y = sin(deg2rad(lon2 - lon1)) * cos(deg2rad(lat2));
    double x = cos(deg2rad(lat1)) * sin(deg2rad(lat2)) -
               sin(deg2rad(lat1)) * cos(deg2rad(lat2)) * cos(deg2rad(lon2 - lon1));
    double b = rad2deg(atan2(y, x));
    return fmod(b + 360.0, 360.0);
}

inline double kmToNm(double km) { return km * 0.539957; }

// Project a target to screen pixels.
// distKm/bearingDeg: from home to target. rangeKm: outer-ring distance.
// rotationDeg: 0 for north-up; pass a value to rotate the whole scope (track-up).
// Returns false if the target is beyond range (caller may clamp to the rim).
struct Point { float x; float y; bool inRange; };
inline Point projectToScreen(double distKm, double bearingDeg, double rangeKm,
                             float cx, float cy, float rOuterPx, double rotationDeg = 0.0) {
    double rPx = (distKm / rangeKm) * rOuterPx;
    bool inRange = (distKm <= rangeKm);
    if (!inRange) rPx = rOuterPx;                 // clamp to rim
    double ang = deg2rad(bearingDeg - rotationDeg);
    Point p;
    p.x = (float)(cx + rPx * sin(ang));
    p.y = (float)(cy - rPx * cos(ang));           // screen Y grows downward; north is up
    p.inRange = inRange;
    return p;
}

} // namespace geo
