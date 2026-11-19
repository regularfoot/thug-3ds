#include <3ds.h>
#include <source_port/old_heap.h>

extern void __real___system_allocateHeaps(void);

void __wrap___system_allocateHeaps(void) {
    // Use the same Old 3DS budget on hardware and in the emulator.
    if (OS_KernelConfig->app_memtype < 6) {
        Handle limit = 0;
        ResourceLimitType commit = RESLIMIT_COMMIT;
        s64 maximum = 0, used = 0;
        if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
            const Result max_result = svcGetResourceLimitLimitValues(&maximum, limit, &commit, 1);
            const Result used_result = svcGetResourceLimitCurrentValues(&used, limit, &commit, 1);
            svcCloseHandle(limit);
            if (R_SUCCEEDED(max_result) && R_SUCCEEDED(used_result) && maximum >= used && used >= 0) {
                const u32 app = source_old_app_heap_size(maximum - used);
                if (app) {
                    extern u32 __ctru_heap_size, __ctru_linear_heap_size;
                    __ctru_heap_size = app;
                    __ctru_linear_heap_size = SOURCE_OLD_LINEAR_BYTES;
                }
            }
        }
    }
    __real___system_allocateHeaps();
}
