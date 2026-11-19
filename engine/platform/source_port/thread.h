#pragma once
// Reuse the WHOLE original SDK-independent PerThreadStruct/PTS declaration.
#include <core/thread/ngc/t_thread.h>
namespace Thread {
// Creation is suspended, matching the original CreateThread contract. All
// lifecycle control belongs to the application's main thread. Entry points may
// use native staging buffers; original Mem contexts are not thread-safe yet.
bool Start(HTHREAD handle, void* argument = nullptr);
bool Join(HTHREAD handle);
bool Destroy(HTHREAD handle); // cancels a never-started thread, otherwise joins
THREADID GetCurrentThreadId();
HTHREAD GetCurrentHandle();
}
