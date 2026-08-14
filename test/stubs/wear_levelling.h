#pragma once
// Fake wear_levelling layer backed by an in-memory byte array.
//
// Modeled on real NOR flash semantics that matter for the endurance test:
//   - erased (0xFF) state per sector (4096 B), only produced by wl_erase_range
//   - wl_write copies bytes into an already-erased region
//
// This faithfully reproduces the historical cross-sector corruption bug:
// if a 96 B record spans two 4096 B sectors, erasing the next sector wipes the
// record's tail and its CRC (covering bytes 6..95) then fails.
//
// Storage + wl_* definitions live in test_flight_log.cpp.

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define TEST_FLASH_SIZE       (6u * 1024u * 1024u)  // 6 MB — 49152 × 128B records
#define TEST_FLASH_SECTOR     (4096u)

typedef int32_t wl_handle_t;
#define WL_INVALID_HANDLE (-1)

extern uint8_t g_flash[TEST_FLASH_SIZE];
extern uint64_t g_flash_erase_count;

void g_flash_reset();   // fill with 0xFF, zero erase count

// Mirrors the real ESP-IDF wear_levelling API surface.
esp_err_t wl_mount(const void* partition, wl_handle_t* out_handle);
esp_err_t wl_unmount(wl_handle_t handle);
size_t   wl_size(wl_handle_t handle);
size_t   wl_sector_size(wl_handle_t handle);
esp_err_t wl_erase_range(wl_handle_t handle, size_t start_addr, size_t size);
esp_err_t wl_write(wl_handle_t handle, size_t dst_addr, const void* src, size_t size);
esp_err_t wl_read(wl_handle_t handle, size_t src_addr, void* dst, size_t size);
