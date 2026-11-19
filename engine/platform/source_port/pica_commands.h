#pragma once
#include <stdint.h>

// The single-parameter libctru encoding is always two words: value, header.
// Leave multi-parameter encoding and the SDK overflow handler to libctru.
static inline int source_pica_append_register(uint32_t* buffer, uint32_t capacity,
    uint32_t* offset, uint32_t header, uint32_t value) {
    const uint32_t at=*offset;
    if(!buffer || at>capacity || capacity-at<2)return 0;
    buffer[at]=value;
    buffer[at+1]=header;
    *offset=at+2;
    return 1;
}
