#ifndef __CORE_DEFINES_H
#define __CORE_DEFINES_H

// Platform contract only. Original engine classes/list/task/module headers
// follow this file unchanged except the explicit standard-C++ patch ledger.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>
#include <climits>
#include <iostream>
#include <strings.h>
using int8 = char;
using int16 = short;
using int32 = std::int32_t;
using int64 = long long;
using uint8 = unsigned char;
using uint16 = unsigned short;
using uint32 = std::uint32_t;
using uint64 = unsigned long long;
using uint = unsigned int;
using sint = int;
using sint8 = signed char;
using sint16 = short;
using sint32 = std::int32_t;
using sint64 = long long;
static_assert(sizeof(uint32) == 4 && sizeof(uint64) == 8, "original engine scalar ABI");
#define TRUE 1
#define FALSE 0
#define vUINT32_MAX 0xffffffffU
#define vUINT_MAX vUINT32_MAX
#define vINT32_MAX 0x7fffffff
#define vINT_MAX vINT32_MAX
#define vINT_BITS 32
#define vPTR_BITS (__SIZEOF_POINTER__ * 8)
#define __CPU_BIG_ENDIAN__ 0
// Original Core/defines.h enables this unconditionally (also for the PC data
// build). It controls Skate's actual navigation owner/class layout. Keeping
// the platform ABI header must not silently disable original engine features.
#define TESTING_GUNSLINGER 1
#define nBit(bit) (1U << (bit))
#define nAlign(bits) __attribute__((aligned((bits) / 8)))
#define nAlignUpBy(p,b) reinterpret_cast<void*>((std::uintptr_t(p) + ((std::uintptr_t(1) << (b)) - 1)) & ~((std::uintptr_t(1) << (b)) - 1))
#define nAlignDownBy(p,b) reinterpret_cast<void*>(std::uintptr_t(p) & ~((std::uintptr_t(1) << (b)) - 1))
#define nAlignedBy(p,b) ((std::uintptr_t(p) & ((std::uintptr_t(1) << (b)) - 1)) == 0)
#define nAlignUp(p) nAlignUpBy(p,4)
#define nAlignDown(p) nAlignDownBy(p,4)
#define nAligned(p) nAlignedBy(p,4)
#define stricmp strcasecmp
#define _stricmp strcasecmp
#define strcmpi strcasecmp
#define strnicmp strncasecmp
#define isnanf std::isnan
#define isinff std::isinf
#define __CPU_WORD_BALIGN__ 4
#define CRCC(number, text) (number)
#define CRCD(number, text) (number)
#define CRCX(text) Crc::GenerateCRCFromString(text)
namespace Crc { uint32 GenerateCRCFromString(const char*); }
namespace SourcePort {
[[noreturn]] void Assertion(const char* file, int line, const char* condition);
[[noreturn]] void AssertionMessage(const char* file, int line, const char* condition, const char* format, ...);
void Log(const char* format, ...);
}
#define Dbg_Assert(c) do { if (!(c)) SourcePort::Assertion(__FILE__, __LINE__, #c); } while (0)
#define SOURCE_DEBUG_ARGS(...) __VA_ARGS__
#define Dbg_MsgAssert(c, message) do { if (!(c)) SourcePort::AssertionMessage(__FILE__, __LINE__, #c, SOURCE_DEBUG_ARGS message); } while (0)
#define Dbg_AssertPtr(p) Dbg_Assert((p) != nullptr)
#define Dbg_AssertType(p, type) Dbg_AssertPtr(p)
#define Dbg_DefineProject(...)
#define Dbg_Code(code) code
#define Dbg_Message(...) SourcePort::Log(__VA_ARGS__)
#define Dbg_Notify(...) SourcePort::Log(__VA_ARGS__)
#define Dbg_Warning(...) SourcePort::Log(__VA_ARGS__)
#define Dbg_Printf(...) SourcePort::Log(__VA_ARGS__)
// Original Dbg_MsgLog writes to the optional in-memory developer trace.
// The port has no trace reader. Do not turn a per-object trace into SD writes.
#define Dbg_MsgLog(params) do {} while (0)
#define Dbg_Error(...) SourcePort::AssertionMessage(__FILE__, __LINE__, "original engine Dbg_Error", __VA_ARGS__)

#include <core/support.h>
#include <core/crc.h>
#include <core/flags.h>
#include <sys/mem/memman.h>
// Original non-standard allocation overloads, implemented once in memory.cpp.
void* operator new(std::size_t, bool);
void* operator new[](std::size_t, bool);
void* operator new(std::size_t, Mem::Allocator*, bool = true);
void* operator new[](std::size_t, Mem::Allocator*, bool = true);
void operator delete(void*, bool) noexcept;
void operator delete[](void*, bool) noexcept;
void operator delete(void*, Mem::Allocator*, bool) noexcept;
void operator delete[](void*, Mem::Allocator*, bool) noexcept;
#define Ryan(...) SourcePort::Log(__VA_ARGS__)
#endif
