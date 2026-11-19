#pragma once
#include <cstddef>
namespace Mem { class Heap; }
namespace SourcePort {
// Real CPU arena, not linear/GPU memory. Application heap configuration and
// physical-device capacity remain a full-application startup obligation.
constexpr std::size_t EngineArenaSize = 64u * 1024u * 1024u;
void* AcquireEngineArena();
std::size_t EngineArenaCapacity();
void ReportOldConsoleHeap();
void ReleaseEngineArena();
void EngineMemoryReady(bool ready);
bool EngineMemoryIsReady();
bool IsEngineAddress(const void* pointer);
void DumpHeap(Mem::Heap* heap);
void AnalyzeHeap(Mem::Heap* heap);
// Use the native application heap for renderer-owned payloads with their own
// lifetime. Explicit original/named heap operations keep their allocator.
class NativeHeapScope {
public:
    NativeHeapScope();
    ~NativeHeapScope();
    NativeHeapScope(const NativeHeapScope&)=delete;
    NativeHeapScope& operator=(const NativeHeapScope&)=delete;
private:
    bool previous_;
};
// Backend storage replacing GPU allocations, or process-lifetime registry
// capacity, must not inherit a temporary player's/theme's current context.
// This still uses the REAL bounded original root heap and block ownership.
enum class RootHeapDirection { BottomUp, TopDown };
class RootHeapScope {
public:
    RootHeapScope();~RootHeapScope();
    explicit RootHeapScope(RootHeapDirection);
    RootHeapScope(const RootHeapScope&)=delete;
    RootHeapScope& operator=(const RootHeapScope&)=delete;
};
}
