#pragma once
#include <sk/heap_sizes_win32.h>
#include <source_port/application_os.h>
// Retain the original PC budgets without defining the Win32 hardware macro.
// The old PS2-style 200KiB theme fallback cannot even hold the retail 255KiB
// DE theme image files. Native textures retain bounded source bytes and CPU
// RGBA images: the largest PA theme has 318464 + 1236992 bytes before headers.
// This is a subdivision of the SAME finite 64MiB original arena, not a fallback
// allocator or an increase in the application's promised physical capacity.
namespace SourcePort {
constexpr unsigned NativeThemeHeapBytes=2*1024*1024;
constexpr unsigned NativeFrontendHeapBytes=2550000;
constexpr unsigned NativeSkaterHeapBytes=240000;
// Native meshes retain decoded CPU vertices and editable CPU textures; PC
// keeps much of that representation in D3D allocations instead. The actual
// original-Main default CAS exhausted 899792 bytes before its wristbands.
constexpr unsigned NativeSkaterGeomHeapBytes=2*1024*1024;
constexpr unsigned Old3DSScriptHeapBytes=19*256*1024;
// This path keeps themed images in the native renderer. The observed menu
// peak in the original theme region is under 32 KiB.
constexpr unsigned Old3DSThemeHeapBytes=512*1024;
}
#ifdef __3DS__
#undef SCRIPT_HEAP_SIZE
#define SCRIPT_HEAP_SIZE (source_application_os_is_new_3ds() ? _SCRIPT_HEAP_SIZE : SourcePort::Old3DSScriptHeapBytes)
#endif
#undef THEME_HEAP_SIZE
#ifdef __3DS__
#define THEME_HEAP_SIZE (source_application_os_is_new_3ds() ? SourcePort::NativeThemeHeapBytes : SourcePort::Old3DSThemeHeapBytes)
#else
#define THEME_HEAP_SIZE SourcePort::NativeThemeHeapBytes
#endif
#undef FRONTEND_HEAP_SIZE
#define FRONTEND_HEAP_SIZE SourcePort::NativeFrontendHeapBytes
#undef SKATER_HEAP_SIZE
#define SKATER_HEAP_SIZE SourcePort::NativeSkaterHeapBytes
#undef SKATER_GEOM_HEAP_SIZE
#define SKATER_GEOM_HEAP_SIZE SourcePort::NativeSkaterGeomHeapBytes
