#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t source_clock_now(void);
uint64_t source_clock_frequency(void);
// Renderer owns GSP initialization and its callback slot. After opening this
// sink it must forward REAL top-screen vblank callbacks to Tmr::Vblank, and
// drain/remove that forwarder before closing. No synthetic video interrupts.
int source_clock_video_open(void);
int source_clock_video_close(void);
int source_clock_video_notify(void);
int source_clock_video_counter(uint32_t* output);
int source_clock_video_wait(uint32_t previous);
#ifdef __cplusplus
}
#endif
