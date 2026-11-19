#include <3ds.h>

extern void __real___system_allocateHeaps(void);

void __wrap___system_allocateHeaps(void) {
    // Use the same Old 3DS budget on hardware and in the emulator.
    if (OS_KernelConfig->app_memtype < 6) {
        Handle limit = 0;
        const ResourceLimitType commit = RESLIMIT_COMMIT;
        s64 maximum = 0, used = 0;
        if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
            const Result max_result = svcGetResourceLimitLimitValues(&maximum, limit, &commit, 1);
            const Result used_result = svcGetResourceLimitCurrentValues(&used, limit, &commit, 1);
            svcCloseHandle(limit);
            if (R_SUCCEEDED(max_result) && R_SUCCEEDED(used_result) &&
                maximum - used >= 42LL * 1024 * 1024 + 512 * 1024) {
                extern u32 __ctru_heap_size, __ctru_linear_heap_size;
                __ctru_heap_size = 35 * 1024 * 1024 + 256 * 1024;
                __ctru_linear_heap_size = 7 * 1024 * 1024 + 256 * 1024;
            }
        }
    }
    __real___system_allocateHeaps();
}
