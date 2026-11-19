#include <core/defines.h>
#include <source_port/thread.h>
#include <source_port/thread_os.h>
#include <cerrno>

extern "C" { Thread::PerThreadStruct _rootThreadStruct = {}; }
namespace {
struct Slot {
    Thread::HTHREAD id;
    Thread::PerThreadStruct* data;
    SourceThreadOS* os;
};
Slot slots[32] = {};
Thread::HTHREAD next_id = 1;
thread_local Slot* current = nullptr;
Slot* lookup(Thread::HTHREAD id) {
    if (!source_thread_is_controller()) { errno = EPERM; return nullptr; }
    for (auto& slot : slots) if (slot.id == id && slot.os) return &slot;
    errno = ESRCH;
    return nullptr;
}
void entry(void* context, void* argument) {
    current = static_cast<Slot*>(context);
    auto function = reinterpret_cast<void (*)(void*)>(current->data->m_pEntry);
    function(argument);
    current = nullptr;
}
}
namespace Thread {
int dummy = -1;
HTHREAD CreateThread(PerThreadStruct* data) {
    if (!source_thread_is_controller()) { errno = EPERM; return 0; }
    if (!data || !data->m_pEntry || data->m_iStackSize < 4096) { errno = EINVAL; return 0; }
    // libctru owns TLS + its stack. Never pretend a caller-provided PS2 stack
    // is honoured; active native engine workers must request OS-owned storage.
    if (data->m_pStackBase) { errno = ENOTSUP; return 0; }
    for (auto& slot : slots) if (slot.os && slot.data == data) { errno = EALREADY; return 0; }
    if (next_id == 0) { errno = EOVERFLOW; return 0; }
    for (auto& slot : slots) if (!slot.os) {
        slot.data = data;
        uint32_t os_id;
        slot.os = source_thread_create(entry, &slot, data->m_iStackSize,
                                       data->m_iInitialPriority, &os_id);
        if (!slot.os) { slot.data = nullptr; return 0; }
        data->m_osId = static_cast<OSTHREADID>(os_id);
        slot.id = next_id++;
        return slot.id;
    }
    errno = EAGAIN;
    return 0;
}
bool Start(HTHREAD id, void* argument) {
    auto* slot = lookup(id);
    return slot && source_thread_start(slot->os, argument) == 0;
}
bool Join(HTHREAD id) {
    auto* slot = lookup(id);
    return slot && source_thread_join(slot->os) == 0;
}
bool Destroy(HTHREAD id) {
    auto* slot = lookup(id);
    if (!slot || source_thread_destroy(slot->os)) return false;
    slot->data->m_osId = 0;
    *slot = {};
    return true;
}
THREADID GetCurrentThreadId() { return current ? current->data->m_utid : _rootThreadStruct.m_utid; }
HTHREAD GetCurrentHandle() { return current ? current->id : 0; }
}
