#include "ble_rid_broadcaster.h"

// BLE 5 Extended Advertising is NOT available in ESP32-C5 Arduino SDK 3.3.10-cn.
// (CONFIG_BT_NIMBLE_EXT_ADV not set; libbt.a compiled without it).
// Production deploy: use ESP-IDF with CONFIG_BT_NIMBLE_EXT_ADV=y.
//
// Stage 1: NimBLE legacy advertising (max 31 bytes).
// Beacon proves RF TX chain. Full 85B GB46750 packet verified via Serial log.

bool BleRidBroadcaster::begin(const char* deviceName) {
    _initialized = false;
    _advertising = false;
    _consecutiveFailures = 0;

    BLEDevice::init(deviceName);

    // Let the NimBLE controller fully initialize (critical on ESP32-C5 RISC-V)
    delay(200);

    // Verify controller is up by reading our own BLE address
    int rc = ble_hs_id_infer_auto(0, &_ownAddrType);
    if (rc == 0) {
        uint8_t addrBytes[6];
        rc = ble_hs_id_copy_addr(_ownAddrType, addrBytes, NULL);
        if (rc == 0) {
            Serial.printf("[BLE] Addr type=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                          _ownAddrType, addrBytes[5], addrBytes[4], addrBytes[3],
                          addrBytes[2], addrBytes[1], addrBytes[0]);
        } else {
            Serial.printf("[BLE] WARN: Cannot read address, rc=%d. Falling back to public.\n", rc);
            _ownAddrType = BLE_OWN_ADDR_PUBLIC;
        }
    } else {
        Serial.printf("[BLE] WARN: ble_hs_id_infer_auto failed rc=%d. Using public address.\n", rc);
        _ownAddrType = BLE_OWN_ADDR_PUBLIC;
    }

    _initialized = true;
    Serial.println("[BLE] Initialized (NimBLE Legacy Advertising — 31B max)");
    Serial.println("[BLE] NOTE: Full GB46750 85B packet logged to Serial, not BLE.");
    Serial.println("[BLE] NOTE: Production requires ESP-IDF + BLE5 Extended Advertising.");
    return true;
}

bool BleRidBroadcaster::selfTest() {
    if (!_initialized) {
        Serial.println("[SELF-TEST] FAIL: Not initialized");
        return false;
    }
    if (!BLEDevice::getInitialized()) {
        Serial.println("[SELF-TEST] FAIL: BLE device layer not available");
        return false;
    }
    Serial.println("[SELF-TEST] PASS — NimBLE module ready");
    return true;
}

bool BleRidBroadcaster::runtimeCheck() {
    if (!_initialized) return false;

    if (!BLEDevice::getInitialized()) {
        _consecutiveFailures++;
        Serial.printf("[RUNTIME-CHECK] FAIL #%d: BLE uninitialized\n", _consecutiveFailures);
        if (_consecutiveFailures >= 3) {
            Serial.println("[RUNTIME-CHECK] CRITICAL: BLE module failed 3 consecutive checks!");
        }
        return false;
    }

    _consecutiveFailures = 0;
    return true;
}

void BleRidBroadcaster::updateAndBroadcast(const GB46750Packet& pkt) {
    if (!_initialized) return;

    // 1. Log the FULL 85-byte GB46750 packet to Serial (primary stage-1 verification)
    uint8_t payload[GB46750_MAX_PACKET];
    uint16_t payloadLen = gb46750_serialize(pkt, payload, sizeof(payload));
    if (payloadLen == 0) {
        Serial.println("[BLE] ERROR: Serialize failed");
        return;
    }

    Serial.printf("[RID] Full %d-byte packet: ", payloadLen);
    for (uint16_t i = 0; i < payloadLen; i++) {
        Serial.printf("%02X ", payload[i]);
    }
    Serial.println();

    // 2. Stop any existing advertising before updating data
    if (_advertising) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            delay(50);  // NimBLE stop is async; brief wait prevents race
        }
        _advertising = false;
    }

    // 3. Set advertising data via NimBLE structured fields API
    // (ble_gap_adv_set_data bypasses internal serialization — use fields API)
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t*)"ESP32C5_RID";
    fields.name_len = strlen("ESP32C5_RID");
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        Serial.printf("[BLE] ERROR: adv_set_fields failed, rc=%d\n", rc);
        return;
    }

    // 4. Start non-connectable legacy advertising (ADV_NONCONN_IND)
    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode   = BLE_GAP_CONN_MODE_NON;   // non-connectable
    adv_params.disc_mode   = BLE_GAP_DISC_MODE_GEN;   // general discoverable
    adv_params.itvl_min    = 160;  // 100 ms (160 * 0.625ms)
    adv_params.itvl_max    = 160;
    adv_params.channel_map = 0x07;  // channels 37 + 38 + 39

    rc = ble_gap_adv_start(_ownAddrType, NULL, 0,
                           &adv_params, NULL, NULL);
    if (rc != 0) {
        Serial.printf("[BLE] ERROR: adv_start failed, rc=%d\n", rc);
        return;
    }

    _advertising = true;
}

void BleRidBroadcaster::stopBroadcast() {
    if (!_initialized || !_advertising) return;

    ble_gap_adv_stop();
    _advertising = false;
    Serial.println("[BLE] Broadcast stopped");
}
