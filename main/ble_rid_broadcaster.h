#ifndef BLE_RID_BROADCASTER_H
#define BLE_RID_BROADCASTER_H

#include <stdint.h>
#include "rid_messages.h"

// ASTM F3411 / GB 42590-2023 RID Service UUID (16-bit SIG-assigned)
#define RID_SERVICE_UUID 0x0D50

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
    uint8_t  _consecutiveFailures = 0;
};

#endif // BLE_RID_BROADCASTER_H
