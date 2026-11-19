#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { SOURCE_AUDIO_RATE = 48000, SOURCE_AUDIO_FRAMES = 1024, SOURCE_AUDIO_BUFFERS = 4 };
unsigned source_audio_rate(void);
int source_audio_open(void);
int source_audio_available(void); // number of unqueued device buffers
int source_audio_submit(const int16_t* stereo, unsigned frames);
void source_audio_discard(void);
void source_audio_close(void);
#ifdef __cplusplus
}
#endif
