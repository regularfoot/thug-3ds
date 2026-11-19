#include <source_port/clock_os.h>
#include <source_port/thread_os.h>
#include <errno.h>
#ifdef __3DS__
#include <3ds.h>
static uint32_t video_enabled, video_counter;
static LightEvent video_event;
#else
#include <time.h>
#include <stdlib.h>
#endif

uint64_t source_clock_now(void) {
#ifdef __3DS__
    return svcGetSystemTick();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}
uint64_t source_clock_frequency(void) {
#ifdef __3DS__
    // svcGetSystemTick has this fixed reference rate even in New3DS fast mode.
    return SYSCLOCK_ARM11;
#else
    return UINT64_C(1000000000);
#endif
}
int source_clock_video_open(void) {
    if (!source_thread_is_controller()) { errno = EPERM; return -1; }
#ifdef __3DS__
    if (__atomic_load_n(&video_enabled, __ATOMIC_ACQUIRE)) { errno = EALREADY; return -1; }
    if (!*gspGetSessionHandle()) { errno = ENODEV; return -1; }
    LightEvent_Init(&video_event, RESET_ONESHOT);
    __atomic_store_n(&video_enabled, 1, __ATOMIC_RELEASE);
    return 0;
#else
    errno = ENOTSUP; return -1; // Host has no physical GSP display.
#endif
}
int source_clock_video_close(void) {
    if (!source_thread_is_controller()) { errno = EPERM; return -1; }
#ifdef __3DS__
    if (!__atomic_exchange_n(&video_enabled, 0, __ATOMIC_ACQ_REL)) { errno = EINVAL; return -1; }
    return 0;
#else
    errno = ENOTSUP; return -1;
#endif
}
int source_clock_video_notify(void) {
#ifdef __3DS__
    // IRQ-service thread: no game heaps, C++ objects or non-atomic 64-bit writes.
    if (!__atomic_load_n(&video_enabled, __ATOMIC_ACQUIRE)) { errno = ENODEV; return -1; }
    __atomic_fetch_add(&video_counter, 1, __ATOMIC_RELEASE);
    LightEvent_Signal(&video_event);
    return 0;
#else
    errno = ENOTSUP; return -1;
#endif
}
int source_clock_video_counter(uint32_t* output) {
    if (!output) { errno = EINVAL; return -1; }
#ifdef __3DS__
    if (!__atomic_load_n(&video_enabled, __ATOMIC_ACQUIRE)) { errno = ENODEV; return -1; }
    *output = __atomic_load_n(&video_counter, __ATOMIC_ACQUIRE);
    return 0;
#else
    errno = ENOTSUP; return -1;
#endif
}
int source_clock_video_wait(uint32_t previous) {
    if (!source_thread_is_controller()) { errno = EPERM; return -1; }
#ifdef __3DS__
    if (!__atomic_load_n(&video_enabled, __ATOMIC_ACQUIRE)) { errno = ENODEV; return -1; }
    while (__atomic_load_n(&video_counter, __ATOMIC_ACQUIRE) == previous) {
        // Lost forwarder or suspended/deinitialized graphics must not spin/hang
        // the console forever. Renderer/APT ownership must be fixed, not faked.
        if (LightEvent_WaitTimeout(&video_event, 250000000) &&
            __atomic_load_n(&video_counter, __ATOMIC_ACQUIRE) == previous) {
            // A real IRQ arriving exactly at timeout is still a fresh frame.
            errno = ETIMEDOUT; return -1;
        }
    }
    return 0;
#else
    (void)previous; errno = ENOTSUP; return -1;
#endif
}
