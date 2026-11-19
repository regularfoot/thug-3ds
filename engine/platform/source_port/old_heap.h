#pragma once
#include <stdint.h>

enum {
    SOURCE_OLD_LINEAR_BYTES = 7424 * 1024,
    SOURCE_OLD_APP_CAP_BYTES = 36096 * 1024
};

// libctru commits whole pages. Leave the original linear budget available for
// graphics and audio even when a larger executable consumes a few more pages.
static inline uint32_t source_old_app_heap_size(int64_t available_bytes) {
    if (available_bytes <= 2 * SOURCE_OLD_LINEAR_BYTES) return 0;
    int64_t remaining = available_bytes & ~INT64_C(4095);
    int64_t app = remaining - SOURCE_OLD_LINEAR_BYTES;
    return app < SOURCE_OLD_APP_CAP_BYTES ? (uint32_t)app : SOURCE_OLD_APP_CAP_BYTES;
}
