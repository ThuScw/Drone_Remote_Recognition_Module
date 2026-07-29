#include "ble_rid_broadcaster.h"
#include "config.h"

#include <string.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_bt.h>
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
static std::atomic<bool> s_synced{false};
static std::atomic<bool> s_needsRecovery{false};

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_util_ensure_addr failed, rc=%d — using fallback", rc);
    }

    rc = ble_hs_id_infer_auto(0, &s_ownAddrType);
    if (rc != 0) {
        s_ownAddrType = BLE_OWN_ADDR_PUBLIC;
        ESP_LOGW(TAG, "Cannot infer addr type, rc=%d — using public", rc);
    }

    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(s_ownAddrType, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "Addr type=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X",
                 s_ownAddrType, addr[5], addr[4], addr[3],
                 addr[2], addr[1], addr[0]);
    } else {
        ESP_LOGW(TAG, "Cannot read address, rc=%d", rc);
    }

    s_synced = true;
    xSemaphoreGive(s_syncSemaphore);
}

static void on_reset(int reason) {
    s_synced = false;
    s_needsRecovery = true;
    ESP_LOGE(TAG, "NimBLE controller reset; reason=%d — recovery required", reason);
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
    _updateFailures = 0;
    s_synced = false;
    s_needsRecovery = false;

    strncpy(_deviceName, deviceName, sizeof(_deviceName) - 1);
    _deviceName[sizeof(_deviceName) - 1] = '\0';

    s_syncSemaphore = xSemaphoreCreateBinary();
    if (!s_syncSemaphore) {
        ESP_LOGE(TAG, "Failed to create sync semaphore");
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_init();

    ble_svc_gap_device_name_set(_deviceName);

    nimble_port_freertos_init(host_task);

    if (xSemaphoreTake(s_syncSemaphore, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE sync timeout (3s)");
        return false;
    }

    _ownAddrType = s_ownAddrType;

    // Set BLE TX power to maximum for GB 46750-2025 compliance
    // 6.1.3: 轻型无人机 EIRP ≥ 4 dBm (360°) or ≥ 6 dBm (avg)
    // ESP32-S3 max TX power +9 dBm + antenna gain ≈ 11 dBm EIRP → compliant
    esp_err_t txRet = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BLE_TX_POWER_LEVEL);
    if (txRet == ESP_OK) {
        ESP_LOGI(TAG, "BLE advertising TX power set OK");
    } else {
        ESP_LOGW(TAG, "BLE TX power set failed (rc=%d) — continuing with default", txRet);
    }

    _initialized = true;
    ESP_LOGI(TAG, "Initialized — BLE5 Extended Advertising ready");
    return true;
}

bool BleRidBroadcaster::selfTest() {
    if (!_initialized) {
        ESP_LOGE(TAG, "Self-test FAIL: not initialized");
        return false;
    }
    if (!s_synced) {
        ESP_LOGE(TAG, "Self-test FAIL: NimBLE host not synced");
        return false;
    }

    // Build a minimal valid test packet — all fields encoded as unknown (0)
    FlightData testFd;
    memset(&testFd, 0, sizeof(testFd));
    testFd.validMask = FLD_ALL;
    testFd.freshness = FRESH_OK;

    GB46750Packet testPkt;
    gb46750_buildPacket(testPkt, testFd, UAS_ID, REALNAME_ID,
                        OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                        HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC, 0);

    // Functional test: actually start BLE extended advertising
    if (!startBroadcast(testPkt)) {
        ESP_LOGE(TAG, "Self-test FAIL: BLE advertising start failed");
        return false;
    }

    stopBroadcast();
    ESP_LOGI(TAG, "Self-test PASS — BLE extended advertising functional");
    return true;
}

bool BleRidBroadcaster::runtimeCheck() {
    if (!_initialized) return false;

    if (!s_synced) {
        _consecutiveFailures++;
        ESP_LOGW(TAG, "Runtime check FAIL #%d: BLE not synced", _consecutiveFailures);
        if (_consecutiveFailures >= 3) {
            ESP_LOGE(TAG, "Runtime check CRITICAL: BLE lost sync — triggering recovery");
            s_needsRecovery = true;
        }
        return false;
    }

    _consecutiveFailures = 0;

    if (_updateFailures > 0) {
        ESP_LOGW(TAG, "Runtime check: %d broadcast update failures in last interval",
                 _updateFailures);
    }

    return true;
}

// --- Self-Healing: 三级递进恢复 ---
// Tier 1: 原地重启广播 (stop + start, 同参数)
// Tier 2: 切换 PHY 重试 (1M ↔ Coded, 对调物理层避开干扰)
// Tier 3: 完整 NimBLE 重初始化 (复位协议栈, 代价最大)

bool BleRidBroadcaster::needsRecovery() const {
    return s_needsRecovery;
}

bool BleRidBroadcaster::reinitNimble() {
    ESP_LOGI(TAG, "Reinitializing NimBLE stack...");

    // 停掉当前 host task
    nimble_port_freertos_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));

    // 重建信号量
    if (s_syncSemaphore) {
        vSemaphoreDelete(s_syncSemaphore);
    }
    s_syncSemaphore = xSemaphoreCreateBinary();
    if (!s_syncSemaphore) {
        ESP_LOGE(TAG, "reinitNimble: semaphore create failed");
        return false;
    }

    s_synced = false;
    s_needsRecovery = false;

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_init();
    ble_svc_gap_device_name_set(_deviceName);
    nimble_port_freertos_init(host_task);

    if (xSemaphoreTake(s_syncSemaphore, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "reinitNimble: sync timeout (5s)");
        return false;
    }

    _ownAddrType = s_ownAddrType;
    _consecutiveFailures = 0;
    _updateFailures = 0;
    _advertising = false;

    ESP_LOGI(TAG, "Nimble reinit complete");
    return true;
}

BleRidBroadcaster::RecoveryResult BleRidBroadcaster::attemptSelfHeal(const GB46750Packet& pkt) {
    if (!_initialized) return RecoveryResult::FAILED;

    ESP_LOGI(TAG, "Self-heal: starting recovery sequence...");

    // --- Tier 1: 原地重启 (保持当前 PHY, 仅重建广播实例) ---
    ESP_LOGI(TAG, "Self-heal Tier 1: restart advertising");
    stopBroadcast();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (startBroadcast(pkt)) {
        ESP_LOGI(TAG, "Self-heal Tier 1 OK — broadcast restarted");
        _degraded = false;
        s_needsRecovery = false;
        return RecoveryResult::RECOVERED;
    }

    // --- Tier 2: 切换 PHY (对调 1M/Coded 物理层, 规避信道干扰) ---
    ESP_LOGI(TAG, "Self-heal Tier 2: switching PHY to %s",
             _useAltPhy ? "1M" : "Coded");
    _useAltPhy = !_useAltPhy;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (startBroadcast(pkt)) {
        ESP_LOGI(TAG, "Self-heal Tier 2 OK — alternate PHY (degraded)");
        _degraded = true;
        s_needsRecovery = false;
        return RecoveryResult::DEGRADED;
    }

    // --- Tier 3: 完整 NimBLE 协议栈重初始化 ---
    ESP_LOGI(TAG, "Self-heal Tier 3: full Nimble reinit");
    _useAltPhy = false;  // 恢复默认 PHY
    if (reinitNimble()) {
        if (startBroadcast(pkt)) {
            ESP_LOGI(TAG, "Self-heal Tier 3 OK — Nimble restarted (degraded)");
            _degraded = true;
            return RecoveryResult::DEGRADED;
        }
    }

    ESP_LOGE(TAG, "Self-heal ALL TIERS FAILED — module requires manual intervention");
    _advertising = false;
    return RecoveryResult::FAILED;
}

// --- Private helper ---

struct os_mbuf* BleRidBroadcaster::buildAdvData(const GB46750Packet& pkt, uint16_t& outLen) {
    uint8_t payload[GB46750_MAX_PACKET];
    uint16_t payloadLen = gb46750_serialize(pkt, payload, sizeof(payload));
    if (payloadLen == 0) {
        ESP_LOGE(TAG, "gb46750_serialize failed");
        return NULL;
    }

    // 编译期保证 ADV data 不超 buffer
    // AD Flags(3) + AD Name(2+maxName) + AD ServiceData(2+2+payload) ≤ buffer
    constexpr size_t kAdvOverhead = 3 + (2 + 31) + (2 + 2);  // 40
    uint8_t advData[kAdvOverhead + GB46750_MAX_PACKET];       // 40 + 128 = 168
    static_assert(sizeof(advData) >= kAdvOverhead + GB46750_MAX_PACKET,
                  "ADV data exceeds buffer");
    uint8_t *p = advData;

    // AD Flags: LE General Discoverable + BR/EDR Not Supported
    *p++ = 0x02;
    *p++ = 0x01;
    *p++ = 0x06;

    // AD Complete Local Name
    size_t nameLen = strlen(_deviceName);
    *p++ = (uint8_t)(1 + nameLen);
    *p++ = 0x09;
    memcpy(p, _deviceName, nameLen);
    p += nameLen;

    // AD Service Data (16-bit UUID) with GB46750 payload
    *p++ = 1 + 2 + (uint8_t)payloadLen;
    *p++ = 0x16;
    *p++ = RID_SERVICE_UUID & 0xFF;
    *p++ = (RID_SERVICE_UUID >> 8) & 0xFF;
    memcpy(p, payload, payloadLen);
    p += payloadLen;

    outLen = p - advData;

    struct os_mbuf *data = os_msys_get_pkthdr(outLen, 0);
    if (!data) {
        ESP_LOGE(TAG, "os_msys_get_pkthdr OOM");
        return NULL;
    }

    if (os_mbuf_append(data, advData, outLen) != 0) {
        ESP_LOGE(TAG, "os_mbuf_append failed");
        os_mbuf_free_chain(data);
        return NULL;
    }

    return data;
}

// --- Broadcast control ---

bool BleRidBroadcaster::startBroadcast(const GB46750Packet& pkt) {
    if (!_initialized || !s_synced) {
        ESP_LOGE(TAG, "startBroadcast: not ready");
        return false;
    }

    if (!gb46750_packetVerify(pkt)) {
        ESP_LOGE(TAG, "startBroadcast: packet verify failed — refusing to send");
        return false;
    }

    uint16_t advDataLen;
    struct os_mbuf *data = buildAdvData(pkt, advDataLen);
    if (!data) return false;

    struct ble_gap_ext_adv_params params = {};
    params.own_addr_type = _ownAddrType;
    params.legacy_pdu = 0;  // Extended Advertising PDU — required for 98-byte payload
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = _useAltPhy ? BLE_HCI_LE_PHY_CODED : BLE_HCI_LE_PHY_1M;
    params.itvl_min = (uint16_t)(BLE_ADV_INTERVAL_MS * 1000 / 625);
    params.itvl_max = (uint16_t)(BLE_ADV_INTERVAL_MS * 1000 / 625);
    params.channel_map = 0x07;
    params.sid = 0;

    int rc = ble_gap_ext_adv_configure(0, &params, NULL, NULL, NULL);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        ESP_LOGE(TAG, "ext_adv_configure failed, rc=%d", rc);
        return false;
    }

    rc = ble_gap_ext_adv_set_data(0, data);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        ESP_LOGE(TAG, "ext_adv_set_data failed, rc=%d", rc);
        return false;
    }

    rc = ble_gap_ext_adv_start(0, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_start failed, rc=%d", rc);
        return false;
    }

    _advertising = true;
    _updateFailures = 0;
    ESP_LOGI(TAG, "Broadcast started — %d bytes AD data", advDataLen);
    return true;
}

bool BleRidBroadcaster::updateBroadcastData(const GB46750Packet& pkt) {
    if (!_initialized || !s_synced) return false;
    if (!_advertising) return false;

    if (!gb46750_packetVerify(pkt)) {
        ESP_LOGE(TAG, "updateBroadcastData: packet verify failed");
        _updateFailures++;
        return false;
    }

    uint16_t advDataLen;
    struct os_mbuf *data = buildAdvData(pkt, advDataLen);
    if (!data) {
        _updateFailures++;
        return false;
    }

    int rc = ble_gap_ext_adv_set_data(0, data);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        _updateFailures++;
        ESP_LOGE(TAG, "ext_adv_set_data update failed #%d, rc=%d",
                 _updateFailures, rc);
        if (_updateFailures >= 3) {
            ESP_LOGE(TAG, "CRITICAL: %d consecutive update failures — data may be stale!",
                     _updateFailures);
        }
        return false;
    }

    _updateFailures = 0;
    return true;
}

void BleRidBroadcaster::stopBroadcast() {
    if (!_initialized || !_advertising) return;

    ble_gap_ext_adv_stop(0);
    _advertising = false;
    ESP_LOGI(TAG, "Broadcast stopped");
}
