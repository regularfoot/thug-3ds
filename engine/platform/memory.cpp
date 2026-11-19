#include <core/defines.h>
#include <source_port/memory.h>
#include <source_port/thread_os.h>
#include <source_port/application_os.h>
#include <source_port/diagnostics.h>
#include <algorithm>
#include <limits>
#ifdef __3DS__
#include <malloc.h>
extern "C" unsigned int __ctru_heap_size;
#endif

namespace {
void* arena_storage = nullptr;
std::uintptr_t arena_start = 0;
std::size_t arena_size = 0;
bool ready = false;
bool native_storage = false;
FILE* diagnostic = nullptr;

void* allocate(std::size_t bytes, bool required, Mem::Allocator* heap = nullptr) {
    // Static C++ runtime initialization precedes original Mem::Manager startup.
    // It stays on the native heap; allocations after startup use the REAL
    // original current/named allocator with its actual original block header.
    if (heap) Dbg_Assert(SourcePort::EngineMemoryIsReady());
    if (SourcePort::EngineMemoryIsReady()) Dbg_Assert(source_thread_is_controller());
    void* result = SourcePort::EngineMemoryIsReady() && (heap || !native_storage)
        ? Mem::Manager::sHandle().New(bytes ? bytes : 1, required, heap)
        : std::malloc(bytes ? bytes : 1);
    if (!result && required) {
#ifdef __3DS__
        SourcePort::Log("ALLOCATION_FAILED bytes=%zu", bytes);
        if (source_thread_is_controller()) SourcePort::CheckpointDiagnosticLog();
#endif
        throw std::bad_alloc();
    }
    return result;
}
void release(void* p) noexcept {
    if (!p) return;
    if (SourcePort::IsEngineAddress(p)) {
        Dbg_Assert(source_thread_is_controller());
        Dbg_Assert(SourcePort::EngineMemoryIsReady());
        Mem::Manager::sHandle().Delete(p);
    } else std::free(p);
}
void* aligned_allocate(std::size_t bytes, std::align_val_t alignment, bool required) {
    std::size_t align = std::size_t(alignment);
    Dbg_Assert(align >= alignof(void*) && (align & (align - 1)) == 0);
    if (bytes > std::numeric_limits<std::size_t>::max() - align - sizeof(void*)) {
        if (required) throw std::bad_alloc();
        return nullptr;
    }
    void* base = allocate(bytes + align + sizeof(void*), required);
    if (!base) return nullptr;
    auto address = (std::uintptr_t(base) + sizeof(void*) + align - 1) & ~(align - 1);
    reinterpret_cast<void**>(address)[-1] = base;
    return reinterpret_cast<void*>(address);
}
void aligned_release(void* p) noexcept { if (p) release(static_cast<void**>(p)[-1]); }
}

namespace SourcePort {
NativeHeapScope::NativeHeapScope():previous_(native_storage){
    Dbg_Assert(source_thread_is_controller()&&EngineMemoryIsReady());
    native_storage=true;
}
NativeHeapScope::~NativeHeapScope(){native_storage=previous_;}
RootHeapScope::RootHeapScope():RootHeapScope(RootHeapDirection::BottomUp){}
RootHeapScope::RootHeapScope(RootHeapDirection direction){
    Dbg_Assert(source_thread_is_controller()&&EngineMemoryIsReady());
    Dbg_Assert(direction==RootHeapDirection::BottomUp||direction==RootHeapDirection::TopDown);
    auto& memory=Mem::Manager::sHandle();memory.PushContext(direction==RootHeapDirection::TopDown?memory.TopDownHeap():memory.BottomUpHeap());
}
RootHeapScope::~RootHeapScope(){Mem::Manager::sHandle().PopContext();}
void* AcquireEngineArena() {
    Dbg_Assert(!arena_storage && !ready);
    arena_size=EngineArenaSize;
#ifdef __3DS__
    if(!source_application_os_is_new_3ds()){
        // The old model's large PRE buffers can use linear storage. Keep the
        // regular application heap primarily available to the original arena.
        constexpr std::size_t native_reserve=17u*256u*1024u;
        constexpr std::size_t minimum_arena=16u*1024u*1024u;
        constexpr std::size_t maximum_arena=48u*1024u*1024u;
        Dbg_MsgAssert(__ctru_heap_size>=native_reserve+minimum_arena,
            ("Insufficient Old 3DS application heap: %u bytes",__ctru_heap_size));
        arena_size=std::min(maximum_arena,std::size_t(__ctru_heap_size)-native_reserve);
    }
#endif
    arena_storage = std::malloc(arena_size + 15);
    Dbg_MsgAssert(arena_storage,("Could not allocate %u-byte original engine arena",unsigned(arena_size)));
    arena_start = (std::uintptr_t(arena_storage) + 15) & ~std::uintptr_t(15);
    return reinterpret_cast<void*>(arena_start);
}
std::size_t EngineArenaCapacity(){return arena_size;}
void ReportOldConsoleHeap() {
#ifdef __3DS__
    if (!source_application_os_is_new_3ds()) {
        const auto usage = mallinfo();
        Log("OLD_APP_HEAP arena_bytes=%zu libc_used_bytes=%zu libc_committed_bytes=%zu heap_limit_bytes=%u",
            arena_size, std::size_t(usage.uordblks), std::size_t(usage.arena), __ctru_heap_size);
        auto& memory = Mem::Manager::sHandle();
        Log("OLD_NAMED_HEAPS script_used=%d script_free=%d theme_used=%d theme_free=%d",
            memory.ScriptHeap()->mUsedMem.m_count, memory.ScriptHeap()->LargestFreeBlock(),
            memory.ThemeHeap()->mUsedMem.m_count, memory.ThemeHeap()->LargestFreeBlock());
    }
#endif
}
void ReleaseEngineArena() {
    Dbg_Assert(arena_storage);
    ready = false;
    std::free(arena_storage);
    arena_storage = nullptr;
    arena_start = 0;
    arena_size = 0;
}
void EngineMemoryReady(bool value) { Dbg_Assert(arena_storage); ready = value; }
bool EngineMemoryIsReady() { return ready; }
bool IsEngineAddress(const void* p) {
    auto address = std::uintptr_t(p);
    return arena_start && address >= arena_start && address - arena_start < arena_size;
}
void DumpHeap(Mem::Heap* heap) {
    Dbg_AssertPtr(heap);
    Log("heap %s used-blocks=%d used-bytes=%d free-fragments=%d region-free=%d",
        heap->GetName(), heap->mUsedBlocks.m_count, heap->mUsedMem.m_count,
        heap->mFreeBlocks.m_count, heap->ParentRegion()->MemAvailable());
}
void AnalyzeHeap(Mem::Heap* heap) {
    Dbg_AssertPtr(heap);
    int count=0;
    for (auto* block=heap->first_block(); block; block=block->mp_next_used) {
        Dbg_Assert(++count <= heap->mUsedBlocks.m_count);
        Dbg_Assert(block->mpAlloc == heap && block->mId == Mem::vALLOC_MAGIC);
    }
    Dbg_Assert(count == heap->mUsedBlocks.m_count);
}
}

// Original global allocation API, moved out of defines.h to valid non-inline
// modern C++. Every named/current heap operation uses the real engine owner.
void* operator new(std::size_t n) { return allocate(n,true); }
void* operator new[](std::size_t n) { return allocate(n,true); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate(n,false); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate(n,false); }
void* operator new(std::size_t n, bool required) { return allocate(n,required); }
void* operator new[](std::size_t n, bool required) { return allocate(n,required); }
void* operator new(std::size_t n, Mem::Allocator* h, bool required) { return allocate(n,required,h); }
void* operator new[](std::size_t n, Mem::Allocator* h, bool required) { return allocate(n,required,h); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, bool) noexcept { release(p); }
void operator delete[](void* p, bool) noexcept { release(p); }
void operator delete(void* p, Mem::Allocator*, bool) noexcept { release(p); }
void operator delete[](void* p, Mem::Allocator*, bool) noexcept { release(p); }
void* operator new(std::size_t n, std::align_val_t a) { return aligned_allocate(n,a,true); }
void* operator new[](std::size_t n, std::align_val_t a) { return aligned_allocate(n,a,true); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return aligned_allocate(n,a,false); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return aligned_allocate(n,a,false); }
void operator delete(void* p, std::align_val_t) noexcept { aligned_release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { aligned_release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { aligned_release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { aligned_release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { aligned_release(p); }

int dumping_printfs = 0;
bool dump_open(const char* name) {
    Dbg_Assert(!diagnostic);
    diagnostic=std::fopen(name,"w");
    dumping_printfs=diagnostic != nullptr;
    return diagnostic != nullptr;
}
void dump_printf(char* text) { std::fputs(text,diagnostic ? diagnostic : stderr); }
void dump_close() { if (diagnostic) std::fclose(diagnostic); diagnostic=nullptr; dumping_printfs=0; }
