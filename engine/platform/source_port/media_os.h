#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// FFmpeg structs/enums stay outside the SDK/original game's enum ABI.
typedef struct SourceMediaCodec SourceMediaCodec;
enum { SOURCE_MEDIA_SEEK_SIZE=0x10000, SOURCE_MEDIA_SEEK_FORCE=0x20000 };
typedef struct SourceMediaIO {
    void* owner;
    int (*read)(void*,uint8_t*,int); // bytes, 0 EOF, negative errno on error
    int64_t (*seek)(void*,int64_t,int);
} SourceMediaIO;
SourceMediaCodec* source_media_open(SourceMediaIO);
void source_media_close(SourceMediaCodec*);
int source_media_rewind(SourceMediaCodec*);
unsigned source_media_rate(const SourceMediaCodec*);
size_t source_media_read(SourceMediaCodec*,float*,size_t);
int source_media_ended(const SourceMediaCodec*);
int source_media_failed(const SourceMediaCodec*);
#ifdef __cplusplus
}
#endif
