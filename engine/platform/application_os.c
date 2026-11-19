#include <source_port/application_os.h>
#include <errno.h>
#include <stddef.h>
#ifdef __3DS__
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/services/apt.h>
#include <3ds/os.h>
#else
#include <signal.h>
#endif

static int initialized;
#ifdef __3DS__
static bool new_3ds;
#endif
#ifndef __3DS__
static struct sigaction previous_int, previous_term;
static volatile sig_atomic_t quit_signal;
static void on_signal(int number) { quit_signal = number; }
#endif
int source_application_os_init(void) {
    if (initialized) { errno = EALREADY; return -1; }
#ifdef __3DS__
    if (R_FAILED(APT_CheckNew3DS(&new_3ds))) { errno = EIO; return -1; }
    if (new_3ds) osSetSpeedupEnable(true);
#else
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous_int)) return -1;
    if (sigaction(SIGTERM, &action, &previous_term)) {
        int failure = errno;
        sigaction(SIGINT, &previous_int, NULL);
        errno = failure; return -1;
    }
    quit_signal = 0;
#endif
    // 3DS service initialization remains owned by libctru's __appInit.
    // userAppInit runs after APT/HID/FS, before __libc_init_array.
    initialized = 1;
    return 0;
}
int source_application_os_poll(void) {
    if (!initialized) { errno = EINVAL; return -1; }
#ifdef __3DS__
    return aptMainLoop() ? 1 : 0;
#else
    return quit_signal ? 0 : 1;
#endif
}
int source_application_os_exit(void) {
    if (!initialized) { errno = EINVAL; return -1; }
#ifdef __3DS__
    if (new_3ds) osSetSpeedupEnable(false);
    new_3ds=false;
#endif
#ifndef __3DS__
    // Restore the host's real signal ownership, not a permanent test handler.
    if (sigaction(SIGTERM, &previous_term, NULL)) return -1;
    if (sigaction(SIGINT, &previous_int, NULL)) return -1;
#endif
    initialized = 0;
    return 0;
}
int source_application_os_is_new_3ds(void) {
#ifdef __3DS__
    return new_3ds;
#else
    return 0;
#endif
}
int source_application_os_is_old_3ds(void) {
#ifdef __3DS__
    return !new_3ds;
#else
    return 0;
#endif
}
