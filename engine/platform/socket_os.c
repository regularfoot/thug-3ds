#include <source_port/socket_os.h>
#include <errno.h>
#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
static void* soc_storage;
int source_soc_init(void) {
    if (soc_storage) { errno = EALREADY; return -1; }
    // SOC maps these pages no-access; do not use an original game suballocation
    // whose neighbours/header might share a protected page.
    void* storage = memalign(0x1000, 0x100000);
    if (!storage) return -1;
    Result rc = socInit(storage, 0x100000);
    if (R_FAILED(rc)) { free(storage); errno = EIO; return -1; }
    soc_storage = storage;
    return 0;
}
int source_soc_exit(void) {
    if (!soc_storage) { errno = EINVAL; return -1; }
    Result rc = socExit();
    // A failing shutdown must not free pages still owned by the service.
    if (R_FAILED(rc)) { errno = EIO; return -1; }
    free(soc_storage);
    soc_storage = NULL;
    return 0;
}
#else
// POSIX sockets require no SDK session. Their real syscalls are exercised in
// the host loopback tests; this is not a substitute for device SOC testing.
int source_soc_init(void) { return 0; }
int source_soc_exit(void) { return 0; }
#endif
