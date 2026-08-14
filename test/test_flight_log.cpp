// FlightLog endurance + correctness tests.
//
// Compiles the REAL ../main/logging/flight_log.cpp against an in-memory fake
// flash (stubs/wear_levelling.h + definitions below) with NOR-flash semantics
// (sector erase → 0xFF, wl_write copies into erased region).
//
// Invariant under test: after writing 1.5 full volumes (ring wrap included),
// every 128 B slot on disk is either erased (0xFF) or a valid "RIDL"+CRC
// record — corrupted==0. The historical 96 B layout (4096/96 = 42.67) lets a
// record straddle a sector boundary; erasing the next sector then wipes the
// record tail and its CRC, so ~1 in 43 records silently fails CRC. Any such
// corruption makes this test fail loudly.
//
// `#define private public` exposes FlightLog::writeRecord / crc16 for direct
// host-side access (test-only; access specifiers do not affect layout).

#include "test_common.h"
#include "wear_levelling.h"
#include "esp_partition.h"
#include "esp_err.h"
#include <string.h>

// ==================== Fake flash storage (single TU) ====================

uint8_t g_flash[TEST_FLASH_SIZE];
uint64_t g_flash_erase_count = 0;

void g_flash_reset() {
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_flash_erase_count = 0;
}

esp_err_t wl_mount(const void* partition, wl_handle_t* out_handle) {
    (void)partition;
    if (out_handle) *out_handle = 0;
    return ESP_OK;
}

esp_err_t wl_unmount(wl_handle_t handle) {
    (void)handle;
    return ESP_OK;
}

size_t wl_size(wl_handle_t handle) {
    (void)handle;
    return TEST_FLASH_SIZE;
}

size_t wl_sector_size(wl_handle_t handle) {
    (void)handle;
    return TEST_FLASH_SECTOR;
}

esp_err_t wl_erase_range(wl_handle_t handle, size_t start_addr, size_t size) {
    (void)handle;
    size_t sector = start_addr / TEST_FLASH_SECTOR;
    while (sector * TEST_FLASH_SECTOR < start_addr + size) {
        memset(g_flash + sector * TEST_FLASH_SECTOR, 0xFF, TEST_FLASH_SECTOR);
        g_flash_erase_count++;
        sector++;
    }
    return ESP_OK;
}

esp_err_t wl_write(wl_handle_t handle, size_t dst_addr, const void* src, size_t size) {
    (void)handle;
    memcpy(g_flash + dst_addr, src, size);
    return ESP_OK;
}

esp_err_t wl_read(wl_handle_t handle, size_t src_addr, void* dst, size_t size) {
    (void)handle;
    memcpy(dst, g_flash + src_addr, size);
    return ESP_OK;
}

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char* label) {
    (void)type; (void)subtype; (void)label;
    static esp_partition_t part;
    static bool inited = false;
    if (!inited) {
        part.type = ESP_PARTITION_TYPE_DATA;
        part.subtype = ESP_PARTITION_SUBTYPE_ANY;
        part.address = 0;
        part.size = TEST_FLASH_SIZE;
        part.erase_size = TEST_FLASH_SECTOR;
        strncpy(part.label, "flight_log", sizeof(part.label) - 1);
        part.label[sizeof(part.label) - 1] = '\0';
        inited = true;
    }
    return &part;
}

// ==================== FlightLog under test ====================

#define private public
#include "flight_log.h"
#undef private

void test_flight_log() {
    printf("--- FlightLog endurance (128B records / 4096B sectors) ---\n");

    const uint32_t capacity    = TEST_FLASH_SIZE / FlightLog::kRecordSize;  // 49152
    const uint32_t totalWrites = capacity + capacity / 2;                   // 1.5 volumes

    g_flash_reset();

    {
        FlightLog log;
        CHECK(log.init());
        CHECK_EQ(log.getMaxRecords(), capacity);

        uint8_t payload[FlightLog::kMaxDataLen];
        const uint64_t baseTs = 1700000000000ULL;
        for (uint32_t i = 0; i < totalWrites; i++) {
            memset(payload, (int)(i & 0xFF), sizeof(payload));
            uint16_t len = (uint16_t)(10 + (i % (FlightLog::kMaxDataLen - 9)));  // 10..80
            CHECK(log.writeRecord(payload, len, baseTs + (uint64_t)i * 1000ULL));
        }
        CHECK_EQ(log.getRecordCount(), (uint32_t)totalWrites);

        // API layer: the whole ring reads back as valid records
        uint32_t readOk = 0;
        for (uint32_t i = 0; i < capacity; i++) {
            uint8_t out[FlightLog::kRecordSize];
            if (log.readRecordRaw(i, out) != 0) readOk++;
        }
        CHECK_EQ(readOk, capacity);
    }

    // Byte-level full-disk scan of every 128 B slot.
    uint32_t corrupted   = 0;
    uint32_t valid       = 0;
    uint32_t blank       = 0;
    uint32_t crossSector = 0;
    uint8_t  buf[FlightLog::kRecordSize];

    for (uint32_t off = 0; off + FlightLog::kRecordSize <= TEST_FLASH_SIZE;
         off += FlightLog::kRecordSize) {
        wl_read(0, off, buf, sizeof(buf));

        bool allFF = true;
        for (uint32_t b = 0; b < sizeof(buf); b++) {
            if (buf[b] != 0xFF) { allFF = false; break; }
        }
        if (allFF) { blank++; continue; }

        uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                       | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        if (magic != FlightLog::kMagic) { corrupted++; continue; }

        uint16_t storedCrc = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
        uint16_t calcCrc   = FlightLog::crc16(buf + 6, sizeof(buf) - 6);
        if (storedCrc != calcCrc) { corrupted++; continue; }

        valid++;

        // A valid 128 B record must never straddle a 4096 B sector boundary.
        if ((off % TEST_FLASH_SECTOR) + FlightLog::kRecordSize > TEST_FLASH_SECTOR) {
            crossSector++;
        }
    }

    CHECK_EQ(corrupted, 0u);   // invariant — old 96 B layout violates this
    CHECK_EQ(crossSector, 0u);
    CHECK_EQ(blank, 0u);       // 1.5 volumes cover every slot
    CHECK_EQ(valid, capacity);

    printf("    capacity=%u writes=%u valid=%u blank=%u corrupted=%u erase_count=%llu\n",
           (unsigned)capacity, (unsigned)totalWrites, (unsigned)valid,
           (unsigned)blank, (unsigned)corrupted,
           (unsigned long long)g_flash_erase_count);
}
