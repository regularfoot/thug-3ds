#pragma once
#include <stdint.h>
#include <string.h>

// The single-parameter libctru encoding is always two words: value, header.
// If both words do not fit, the caller uses the SDK overflow handler.
static inline int source_pica_append_register(uint32_t* buffer, uint32_t capacity,
    uint32_t* offset, uint32_t header, uint32_t value) {
    const uint32_t at=*offset;
    if(!buffer || at>capacity || capacity-at<2)return 0;
    buffer[at]=value;
    buffer[at+1]=header;
    *offset=at+2;
    return 1;
}

// Common short register runs, including sampler and uniform uploads. The
// SDK handles split packets, null sources and its own overflow reporting.
static inline int source_pica_append_short_run(uint32_t* buffer,uint32_t capacity,
    uint32_t* offset,uint32_t header,const uint32_t* values,unsigned count) {
    if(!buffer || !values || count<2 || count>16)return 0;
    uint32_t at=*offset,words=(count+2)&~1u;
    if(at>capacity || words>capacity-at)return 0;
    uint32_t* out=buffer+at;
    out[0]=values[0];
    out[1]=header|((count-1)<<20);
    if(count==5)memcpy(out+2,values+1,4*sizeof(*out));
    else memcpy(out+2,values+1,(count-1)*sizeof(*out));
    if(!(count&1))out[count+1]=0;
    *offset=at+words;
    return 1;
}
