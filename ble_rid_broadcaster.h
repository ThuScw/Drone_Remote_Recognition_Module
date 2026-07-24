#ifndef BLE_RID_BROADCASTER_H
#define BLE_RID_BROADCASTER_H

#include <stdint.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>

// NimBLE legacy advertising API (ESP32-C5 SDK 3.3.10-cn: EXT_ADV not enabled)
#include <host/ble_gap.h>
#include <host/ble_hs_id.h>
#include <host/ble_hs_adv.h>

#include "rid_messages.h"

// ASTM F3411 RID Service UUID (GB 42590-2023 A.1.1)
#define RID_SERVICE_UUID 0x0D50

// Legacy advertising max: BLE_HCI_MAX_ADV_DATA_LEN = 31 bytes
#define LEGACY_ADV_MAX 31

class BleRidBroadcaster {
public:
    bool begin(const char* deviceName);
    bool selfTest();
    bool runtimeCheck();
    void updateAndBroadcast(const GB46750Packet& pkt);
    void stopBroadcast();
    bool isAdvertising() const { return _advertising; }

private:
    uint8_t  _ownAddrType = 0;
    bool     _initialized = false;
    bool     _advertising = false;
    uint32_t _lastCheckTime = 0;
    uint8_t  _consecutiveFailures = 0;
};

#endif // BLE_RID_BROADCASTER_H
