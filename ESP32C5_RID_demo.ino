#include "config.h"
#include "rid_messages.h"
#include "ble_rid_broadcaster.h"

BleRidBroadcaster broadcaster;
GB46750Packet      gbPacket;
FlightData         currentFlightData;

uint32_t lastBroadcastTime   = 0;
uint32_t lastDataUpdateTime  = 0;
uint32_t lastSelfTestTime    = 0;
uint8_t  prevStatus          = 0xFF;  // force first status check
bool     broadcastActive     = false;

const char* statusNames[] = {"UNRPT", "GND", "AIR", "EMERG", "FAILSAFE", "FAILEND"};

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n============================================");
    Serial.println("ESP32-C5 RID Broadcaster - GB 46750-2025");
    Serial.println("Light Show Drone (35cm, Class 1)");
    Serial.println("BLE5 Extended Advertising Mode");
    Serial.println("============================================");

    if (!broadcaster.begin("ESP32C5_RID")) {
        Serial.println("[SYS] FATAL: BLE init failed. Halting.");
        while (1) { delay(1000); }
    }

    if (!broadcaster.selfTest()) {
        Serial.println("[SYS] FATAL: Self-test failed. Halting.");
        while (1) { delay(1000); }
    }

    memset(&currentFlightData, 0, sizeof(currentFlightData));
    currentFlightData.lat       = MOCK_LATITUDE;
    currentFlightData.lon       = MOCK_LONGITUDE;
    currentFlightData.geoAlt    = MOCK_GEO_ALT;
    currentFlightData.baroAlt   = MOCK_BARO_ALT;
    currentFlightData.heightAgl = MOCK_HEIGHT_AGL;
    currentFlightData.speed     = MOCK_SPEED;
    currentFlightData.heading   = MOCK_HEADING;
    currentFlightData.vspeed    = MOCK_VSPEED;
    currentFlightData.opStatus  = STATUS_GROUND;
    currentFlightData.opLat     = MOCK_OP_LAT;
    currentFlightData.opLon     = MOCK_OP_LON;
    currentFlightData.opAlt     = MOCK_OP_ALT;

    gb46750_buildPacket(gbPacket, currentFlightData, UAS_ID, REALNAME_ID,
                        OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                        HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC);

    Serial.printf("[SYS] Packet=%d bytes | Broadcast every %d ms | Self-test every %d ms\n",
                  gbPacket.totalLen, BROADCAST_INTERVAL_MS, SELF_TEST_INTERVAL_MS);
    Serial.println("[SYS] Ready. Monitor with nRF Connect.");
}

void loop() {
    uint32_t now = millis();

    // --- Data Update (independent of broadcast) ---
    if (now - lastDataUpdateTime >= DATA_UPDATE_INTERVAL_MS) {
        lastDataUpdateTime = now;
        updateFlightData(now);
        gb46750_buildPacket(gbPacket, currentFlightData, UAS_ID, REALNAME_ID,
                            OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                            HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC);
    }

    // --- Broadcast Working Interval ---
    // GB 42590-2023 A.2.3.5.5: auto-start when airborne, stop on ground
    if (currentFlightData.opStatus != prevStatus) {
        handleStatusTransition(currentFlightData.opStatus);
        prevStatus = currentFlightData.opStatus;
    }

    // --- Broadcast (BLE5 Extended Advertising, single complete packet) ---
    // GB 46750-2025 6.1.1: 单次完整传输报文内容
    if (broadcastActive && (now - lastBroadcastTime >= BROADCAST_INTERVAL_MS)) {
        lastBroadcastTime = now;
        broadcaster.updateAndBroadcast(gbPacket);

        Serial.printf("[TX] Status=%s | Alt=%.1fm | Spd=%.1fm/s | Hdg=%.0f°\n",
                      statusNames[currentFlightData.opStatus],
                      currentFlightData.geoAlt, currentFlightData.speed,
                      currentFlightData.heading);
    }

    // --- Runtime Continuous Self-Test ---
    // GB 42590-2023 A.2.3.5.5: 全飞行周期持续监测模块状态
    if (now - lastSelfTestTime >= SELF_TEST_INTERVAL_MS) {
        lastSelfTestTime = now;
        broadcaster.runtimeCheck();
    }

    delay(10);
}

void updateFlightData(uint32_t now) {
    // Cycle through flight phases every 30 seconds for demo
    uint32_t cycle = (now / 30000) % 4;
    switch (cycle) {
        case 0: currentFlightData.opStatus = STATUS_GROUND;    break;
        case 1: currentFlightData.opStatus = STATUS_AIRBORNE;  break;
        case 2: currentFlightData.opStatus = STATUS_EMERGENCY; break;
        case 3: currentFlightData.opStatus = STATUS_AIRBORNE;  break;
    }

    currentFlightData.lat       = MOCK_LATITUDE;
    currentFlightData.lon       = MOCK_LONGITUDE;
    currentFlightData.geoAlt    = MOCK_GEO_ALT;
    currentFlightData.baroAlt   = MOCK_BARO_ALT;
    currentFlightData.heightAgl = MOCK_HEIGHT_AGL;
    currentFlightData.speed     = MOCK_SPEED;
    currentFlightData.heading   = MOCK_HEADING;
    currentFlightData.vspeed    = MOCK_VSPEED;
    currentFlightData.opLat     = MOCK_OP_LAT;
    currentFlightData.opLon     = MOCK_OP_LON;
    currentFlightData.opAlt     = MOCK_OP_ALT;
}

void handleStatusTransition(uint8_t newStatus) {
    // GB 42590-2023: broadcast during entire flight, stop on ground
    bool shouldBroadcast = (newStatus == STATUS_AIRBORNE ||
                            newStatus == STATUS_EMERGENCY);

    if (shouldBroadcast && !broadcastActive) {
        Serial.println("[SYS] Broadcast START (airborne)");
        broadcastActive = true;
    } else if (!shouldBroadcast && broadcastActive) {
        Serial.println("[SYS] Broadcast STOP (ground)");
        broadcaster.stopBroadcast();
        broadcastActive = false;
    }
}
