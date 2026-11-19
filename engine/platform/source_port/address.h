#pragma once
#include <core/defines.h>
namespace SourcePort {
// Packed original BSP/file layouts contain 32-bit addresses. ARM32 preserves
// that ABI. Host diagnostics must reject unrepresentable native addresses,
// never silently truncate them or widen a structure mapped from retail data.
inline uint32 Address32(const void* pointer) {
    auto address = std::uintptr_t(pointer);
    Dbg_Assert(address <= UINT32_MAX);
    return uint32(address);
}
}
