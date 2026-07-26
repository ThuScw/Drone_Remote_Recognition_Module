#include "flight_data.h"
#include "config.h"
#include <math.h>

void getFlightData(FlightData& fd, uint64_t nowMs) {
    const uint64_t cycleMs = SIM_GROUND_WAIT_MS + SIM_TAKEOFF_MS
                           + SIM_CRUISE_MS + SIM_LANDING_MS;
    uint64_t t = nowMs % cycleMs;

    const uint64_t t_takeoff = SIM_GROUND_WAIT_MS;
    const uint64_t t_cruise  = t_takeoff + SIM_TAKEOFF_MS;
    const uint64_t t_landing = t_cruise  + SIM_CRUISE_MS;

    fd.opLat = MOCK_OP_LAT;
    fd.opLon = MOCK_OP_LON;
    fd.opAlt = MOCK_OP_ALT;

    if (t < t_takeoff) {
        fd.opStatus  = STATUS_GROUND;
        fd.lat       = MOCK_LATITUDE;
        fd.lon       = MOCK_LONGITUDE;
        fd.heightAgl = 0.0f;
        fd.geoAlt    = MOCK_GEO_BASE_ALT;
        fd.baroAlt   = MOCK_GEO_BASE_ALT - 2.3f;
        fd.speed     = 0.0f;
        fd.heading   = 0.0f;
        fd.vspeed    = 0.0f;
        fd.validMask = FLD_ALL;
    } else if (t < t_cruise) {
        float p = (float)(t - t_takeoff) / (float)SIM_TAKEOFF_MS;
        fd.opStatus  = STATUS_AIRBORNE;
        fd.heightAgl = p * SIM_CRUISE_ALT;
        fd.geoAlt    = MOCK_GEO_BASE_ALT + fd.heightAgl;
        fd.baroAlt   = fd.geoAlt - 2.3f;
        fd.speed     = p * SIM_CRUISE_SPEED;
        fd.heading   = 45.0f;
        fd.vspeed    = SIM_CRUISE_ALT / ((float)SIM_TAKEOFF_MS / 1000.0f);
        fd.lat       = MOCK_LATITUDE + p * 0.0002f;
        fd.lon       = MOCK_LONGITUDE;
        fd.validMask = FLD_ALL;
    } else if (t < t_landing) {
        float p = (float)(t - t_cruise) / (float)SIM_CRUISE_MS;
        fd.opStatus  = STATUS_AIRBORNE;
        fd.heightAgl = SIM_CRUISE_ALT;
        fd.geoAlt    = MOCK_GEO_BASE_ALT + SIM_CRUISE_ALT;
        fd.baroAlt   = fd.geoAlt - 2.3f;
        fd.speed     = SIM_CRUISE_SPEED;
        fd.heading   = fmodf(45.0f + p * 360.0f, 360.0f);
        fd.vspeed    = 0.0f;
        float angle  = p * 2.0f * 3.14159265f;
        fd.lat       = MOCK_LATITUDE + sinf(angle) * 0.0005f;
        fd.lon       = MOCK_LONGITUDE + cosf(angle) * 0.0005f;
        fd.validMask = FLD_ALL;
    } else {
        float p = (float)(t - t_landing) / (float)SIM_LANDING_MS;
        fd.opStatus  = STATUS_AIRBORNE;
        fd.heightAgl = SIM_CRUISE_ALT * (1.0f - p);
        fd.geoAlt    = MOCK_GEO_BASE_ALT + fd.heightAgl;
        fd.baroAlt   = fd.geoAlt - 2.3f;
        fd.speed     = SIM_CRUISE_SPEED * (1.0f - p);
        fd.heading   = 45.0f;
        fd.vspeed    = -(SIM_CRUISE_ALT / ((float)SIM_LANDING_MS / 1000.0f));
        fd.lat       = MOCK_LATITUDE + 0.0002f;
        fd.lon       = MOCK_LONGITUDE;
        fd.validMask = FLD_ALL;
    }
}
