#pragma once

#include <cstdio>

// ESP_LOG shims — route to fprintf for test visibility
#define ESP_LOGI(tag, fmt, ...)  fprintf(stdout,  "I [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...)  fprintf(stderr, "E [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)  fprintf(stderr, "W [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  /* silenced in test */

#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, level) /* not used in tested modules */
