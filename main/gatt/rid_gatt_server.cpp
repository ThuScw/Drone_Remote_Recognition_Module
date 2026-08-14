#include "rid_gatt_server.h"

#include <string.h>
#include <esp_log.h>
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

static const char* TAG = "RID_GATT";

// 单实例静态状态 (NimBLE 回调为 C 函数, 需静态可达)
static RidConfigStore* g_store = nullptr;
static uint16_t g_connHandle = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE

// 特征角色 (arg 携带): 0=UAS_ID 1=REALNAME 2=OP_CATEGORY 3=UA_CLASS 4=STATE
static int gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt* ctxt, void* arg) {
    if (!g_store) return BLE_ATT_ERR_UNLIKELY;
    g_connHandle = conn_handle;
    int role = (int)(intptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        RidConfig cfg;
        g_store->get(cfg);
        uint8_t v;
        const uint8_t* p = nullptr;
        uint16_t len = 0;
        switch (role) {
        case 0: p = (const uint8_t*)cfg.uasId;      len = RID_CONFIG_UAS_ID_LEN;   break;
        case 1: p = (const uint8_t*)cfg.realNameId; len = RID_CONFIG_REALNAME_LEN; break;
        case 2: v = cfg.opCategory; p = &v; len = 1; break;
        case 3: v = cfg.uaClass;    p = &v; len = 1; break;
        case 4: v = (uint8_t)g_store->state(); p = &v; len = 1; break;
        default: return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, p, len);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (role == 4) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;  // STATE 只读

        RidConfigField field;
        switch (role) {
        case 0: field = RidConfigField::UAS_ID;      break;
        case 1: field = RidConfigField::REALNAME_ID; break;
        case 2: field = RidConfigField::OP_CATEGORY; break;
        case 3: field = RidConfigField::UA_CLASS;    break;
        default: return BLE_ATT_ERR_UNLIKELY;
        }

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[RID_CONFIG_UAS_ID_LEN + 1];
        if (len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        os_mbuf_copydata(ctxt->om, 0, len, buf);

        if (g_store->setField(field, buf, len)) return 0;

        // 空中锁定 → 写不允许; 其余 → 值非法
        return (g_store->state() == RID_STATE_AIRBORNE)
                   ? BLE_ATT_ERR_WRITE_NOT_PERMITTED
                   : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }

    return 0;
}

// 特征定义: FFF1~FFF5
static uint16_t g_uasVal, g_rnVal, g_opcVal, g_uacVal, g_stateVal;

// 16-bit UUID 静态实例 — BLE_UUID16_DECLARE 是 C99 复合字面量, 在 C++ 编译下为右值,
// 对 .uuid 取地址会报 "taking address of rvalue"; 改用 static const + BLE_UUID16_INIT。
static const ble_uuid16_t g_uuidUas   = BLE_UUID16_INIT(RID_GATT_CHR_UAS_ID);
static const ble_uuid16_t g_uuidRn    = BLE_UUID16_INIT(RID_GATT_CHR_REALNAME);
static const ble_uuid16_t g_uuidOpc   = BLE_UUID16_INIT(RID_GATT_CHR_OP_CATEGORY);
static const ble_uuid16_t g_uuidUac   = BLE_UUID16_INIT(RID_GATT_CHR_UA_CLASS);
static const ble_uuid16_t g_uuidState = BLE_UUID16_INIT(RID_GATT_CHR_STATE);
static const ble_uuid16_t g_uuidSvc   = BLE_UUID16_INIT(RID_GATT_SVC_UUID16);

static const struct ble_gatt_chr_def gatt_chrs[] = {
    {
        .uuid       = &g_uuidUas.u,
        .access_cb  = gatt_chr_access,
        .arg        = (void*)0,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
        .val_handle = &g_uasVal,
    },
    {
        .uuid       = &g_uuidRn.u,
        .access_cb  = gatt_chr_access,
        .arg        = (void*)1,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
        .val_handle = &g_rnVal,
    },
    {
        .uuid       = &g_uuidOpc.u,
        .access_cb  = gatt_chr_access,
        .arg        = (void*)2,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
        .val_handle = &g_opcVal,
    },
    {
        .uuid       = &g_uuidUac.u,
        .access_cb  = gatt_chr_access,
        .arg        = (void*)3,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
        .val_handle = &g_uacVal,
    },
    {
        .uuid       = &g_uuidState.u,
        .access_cb  = gatt_chr_access,
        .arg        = (void*)4,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_stateVal,
    },
    { 0 }
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &g_uuidSvc.u,
        .characteristics = gatt_chrs,
    },
    { 0 }
};

bool RidGattServer::init(RidConfigStore& store) {
    g_store = &store;

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed rc=%d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed rc=%d", rc);
        return false;
    }

    _registered = true;
    ESP_LOGI(TAG, "GATT service registered — 0xFFF0 (UAS_ID/REALNAME/OP_CAT/UA_CLASS/STATE)");
    return true;
}

void RidGattServer::notifyState() {
    if (!_registered || !g_store) return;
    if (g_connHandle == 0xFFFF) return;  // 无连接

    uint8_t state = (uint8_t)g_store->state();
    struct os_mbuf* om = os_msys_get_pkthdr(1, 0);
    if (!om) return;
    if (os_mbuf_append(om, &state, 1) != 0) {
        os_mbuf_free_chain(om);
        return;
    }

    int rc = ble_gatts_notify_custom(g_connHandle, g_stateVal, om);  // 消耗 om
    if (rc != 0) {
        ESP_LOGW(TAG, "notifyState rc=%d (conn=%d)", rc, g_connHandle);
        g_connHandle = 0xFFFF;  // 句柄失效
    }
}
