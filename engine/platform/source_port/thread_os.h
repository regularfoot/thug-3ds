#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* C boundary: libctru's global Thread typedef must never collide with the
 * original engine's Thread namespace. OS storage uses malloc, not game heaps. */
typedef struct SourceThreadOS SourceThreadOS;
typedef void (*SourceThreadEntry)(void* context, void* argument);
SourceThreadOS* source_thread_create(SourceThreadEntry entry, void* context,
                                    size_t stack_size, int priority, uint32_t* os_id);
int source_thread_start(SourceThreadOS* thread, void* argument);
int source_thread_join(SourceThreadOS* thread);
int source_thread_destroy(SourceThreadOS* thread);
typedef struct SourceFrameWorker SourceFrameWorker;
SourceFrameWorker* source_frame_worker_create(void);
int source_frame_worker_core(const SourceFrameWorker*);
void source_frame_worker_submit(SourceFrameWorker*, void (*job)(void*), void* input);
void source_frame_worker_wait(SourceFrameWorker*);
void source_frame_worker_destroy(SourceFrameWorker*);
int source_thread_is_controller(void);
void source_thread_sleep_ms(unsigned milliseconds);
#ifdef __cplusplus
}
#endif
