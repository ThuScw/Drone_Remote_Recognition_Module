#include "ble_rid_broadcaster.h"

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include<freertos/task.h>
#include <esp_log.h>
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char* TAG = "BLE_RID";

// --- NimBLE async init bridge (C callbacks need static linkage) ---
static SemaphoreHandle_t s_syncSemaphore = NULL;
static uint8_t           s_ownAddrType = 0;
static bool              s_synced = false;

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_ownAddrType);
    if (rc != 0) {
        s_ownAddrType = BLE_OWN_ADDR_PUBLIC;
    }

    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(s_ownAddrType, addr, NULL);
    if (rc == 0) {
        printf("[BLE] Addr type=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
               s_ownAddrType, addr[5], addr[4], addr[3],
               addr[2], addr[1], addr[0]);
    } else {
        printf("[BLE] WARN: Cannot read address, rc=%d\n", rc);
    }

    s_synced = true;
    xSemaphoreGive(s_syncSemaphore);
}

static void on_reset(int reason) {
    s_synced = false;
    ESP_LOGE(TAG, "NimBLE controller reset; reason=%d", reason);
}

static void host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// --- Public API ---

bool BleRidBroadcaster::begin(const char* deviceName) {
    _initialized = false;
    _advertising = false;
    _consecutiveFailures = 0;
    s_synced = false;

    s_syncSemaphore = xSemaphoreCreateBinary();
    if (!s_syncSemaphore) {
        ESP_LOGE(TAG, "Failed to create sync semaphore");
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_init();

    ble_svc_gap_device_name_set(deviceName);

    nimble_port_freertos_init(host_task);

    // Wait for NimBLE host to sync (3s timeout)
    if (xSemaphoreTake(s_syncSemaphore, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE sync timeout (3s)");
        return false;
    }

    _ownAddrType = s_ownAddrType;
    _initialized = true;
    ESP_LOGI(TAG, "Initialized — BLE5 Extended Advertising ready");
    return true;
}

bool BleRidBroadcaster::selfTest() {
    if (!_initialized) {
        printf("[SELF-TEST] FAIL: Not initialized\n");
        return false;
    }
    if (!s_synced) {
        printf("[SELF-TEST] FAIL: NimBLE host not synced\n");
        return false;
    }
    printf("[SELF-TEST] PASS — NimBLE + EXT_ADV ready\n");
    return true;
}

bool BleRidBroadcaster::runtimeCheck() {
    if (!_initialized) return false;

    if (!s_synced) {
        _consecutiveFailures++;
        printf("[RUNTIME-CHECK] FAIL #%d: BLE not synced\n", _consecutiveFailures);
        if (_consecutiveFailures >= 3) {
            printf("[RUNTIME-CHECK] CRITICAL: BLE failed 3 consecutive checks!\n");
        }
        return false;
    }

    _consecutiveFailures = 0;
    return true;
}

void BleRidBroadcaster::updateAndBroadcast(const GB46750Packet& pkt) {
    if (!_initialized || !s_synced) return;

    // Stage 1: 首次配置后保持广播持续运行，不再重复重启
    // 避免频繁 stop/start 导致手机扫描错过
    if (_advertising) {
        return;  // 已在广播，直接返回
    }

    // 1. Serialize GB46750 packet (78 bytes)
    uint8_t payload[GB46750_MAX_PACKET];
    uint16_t payloadLen = gb46750_serialize(pkt, payload, sizeof(payload));
    if (payloadLen == 0) {
        ESP_LOGE(TAG, "gb46750_serialize failed");
        return;
    }

    // Log full GB46750 hex packet to serial for verification
    printf("[RID] Full %d-byte packet: ", payloadLen);
    for (uint16_t i = 0; i < payloadLen; i++) {
        printf("%02X ", payload[i]);
    }
    printf("\n");

    // 2. Build raw BLE advertising data:
    //    AD Flags (3B) + AD Complete Local Name (variable) + AD Service Data (variable)
    uint8_t advData[128];
    uint8_t *p = advData;

    // AD Flags: LE General Discoverable + BR/EDR Not Supported
    *p++ = 0x02;  // Length = Type(1) + Data(1) = 2
    *p++ = 0x01;  // AD Type: Flags
    *p++ = 0x06;  // Flags value

    // AD Complete Local Name (Type 0x09)
    const char* deviceName = "ESP32C5_RID";
    size_t nameLen = strlen(deviceName);
    *p++ = (uint8_t)(1 + nameLen);  // Length = Type(1) + Name(N)
    *p++ = 0x09;                     // AD Type: Complete Local Name
    memcpy(p, deviceName, nameLen);
    p += nameLen;

    // AD Service Data (16-bit UUID)
    *p++ = 1 + 2 + (uint8_t)payloadLen;  // Length = Type(1) + UUID(2) + Data(N)
    *p++ = 0x16;                          // AD Type: Service Data - 16-bit UUID
    *p++ = RID_SERVICE_UUID & 0xFF;       // UUID LSB
    *p++ = (RID_SERVICE_UUID >> 8) & 0xFF; // UUID MSB
    memcpy(p, payload, payloadLen);
    p += payloadLen;

    uint16_t advDataLen = p - advData;

    // 3. Configure EXT_ADV parameters (only once)
    struct ble_gap_ext_adv_params params = {};
    params.own_addr_type = _ownAddrType;
    params.legacy_pdu = 0;                   // Extended Advertising PDU
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.itvl_min = 160;                   // 100 ms (160 * 0.625 ms)
    params.itvl_max = 160;
    params.channel_map = 0x07;               // Channels 37 + 38 + 39
    params.sid = 0;

    int rc = ble_gap_ext_adv_configure(0, &params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_configure failed, rc=%d", rc);
        return;
    }

    // 4. Set advertising data via os_mbuf chain
    struct os_mbuf *data = os_msys_get_pkthdr(advDataLen, 0);
    if (!data) {
        ESP_LOGE(TAG, "os_msys_get_pkthdr OOM");
        return;
    }

    rc = os_mbuf_append(data, advData, advDataLen);
    if (rc != 0) {
        ESP_LOGE(TAG, "os_mbuf_append failed, rc=%d", rc);
        os_mbuf_free_chain(data);
        return;
    }

    rc = ble_gap_ext_adv_set_data(0, data);
    // NimBLE takes ownership of mbuf on success; only free on failure
    if (rc != 0) {
        os_mbuf_free_chain(data);
        ESP_LOGE(TAG, "ext_adv_set_data failed, rc=%d", rc);
        return;
    }

    // 5. Start extended advertising (0 duration = infinite, 0 max_events = unlimited)
    rc = ble_gap_ext_adv_start(0, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_start failed, rc=%d", rc);
        return;
    }

    _advertising = true;
}

void BleRidBroadcaster::stopBroadcast() {
    if (!_initialized || !_advertising) return;

    ble_gap_ext_adv_stop(0);
    _advertising = false;
    printf("[BLE] Broadcast stopped\n");
}
