// Actual native disk operations with the original completion queue/poll owner.
// SD calls execute on the game thread; callbacks are deferred by original
// io_callback. This backend does not claim a background I/O worker.
#include <core/defines.h>
#include <sys/file/asyncfilesys.h>
#include <sys/file/filesys.h>
#include <source_port/thread_os.h>
#include <cerrno>
namespace { bool initialized = false; void controller() { Dbg_Assert(source_thread_is_controller()); } }
namespace File {
volatile bool CAsyncFileHandle::IsDone() { controller(); return !IsBusy(); }
char* CAsyncFileHandle::GetS(char* buffer, int max) {
    controller();
    if (mp_pre_file) { errno = ENOTSUP; return nullptr; } // original PRE forbids this API
    m_current_function = FUNC_READ;
    return plat_get_s(buffer, max);
}
void CAsyncFileHandle::plat_init() { controller(); mp_native_file = nullptr; }
bool CAsyncFileHandle::plat_open(const char* name) {
    controller(); Dbg_Assert(!mp_native_file);
    mp_native_file = File::Open(name, "rb");
    if (!mp_native_file) { m_file_size = 0; m_position = 0; m_last_result = -errno; return false; }
    auto size = File::GetFileSize(mp_native_file);
    if (size < 0 || size > INT_MAX) { File::Close(mp_native_file); mp_native_file = nullptr; m_file_size = 0; return false; }
    m_file_size = int(size); m_position = 0;
    inc_busy_count(); io_callback(FUNC_OPEN, 1, 0);
    return true;
}
bool CAsyncFileHandle::plat_close() {
    controller(); Dbg_Assert(mp_native_file);
    int result = File::Close(mp_native_file); mp_native_file = nullptr;
    inc_busy_count(); io_callback(FUNC_CLOSE, result, 0);
    return result == 0;
}
volatile bool CAsyncFileHandle::plat_is_done() { controller(); return get_busy_count() == 0; }
volatile bool CAsyncFileHandle::plat_is_busy() { controller(); return get_busy_count() != 0; }
bool CAsyncFileHandle::plat_is_eof() const { controller(); return m_position >= m_file_size; }
void CAsyncFileHandle::plat_set_priority(int priority) { controller(); Dbg_Assert(m_priority == priority); /* no competing disk workers */ }
void CAsyncFileHandle::plat_set_stream(bool stream) { controller(); Dbg_Assert(m_stream == stream); /* original caller controls individual transfers */ }
void CAsyncFileHandle::plat_set_destination(EAsyncMemoryType destination) {
    controller(); Dbg_MsgAssert(int(destination) == DEFAULT_MEMORY_TYPE, ("Native I/O only targets CPU memory"));
}
void CAsyncFileHandle::plat_set_buffer_size(size_t bytes) { controller(); Dbg_Assert(bytes && bytes <= INT_MAX && m_buffer_size == bytes); }
void CAsyncFileHandle::plat_set_blocking(bool block) { controller(); Dbg_Assert(block == m_blocking); }
size_t CAsyncFileHandle::plat_load(void* buffer) {
    controller(); Dbg_Assert(mp_native_file);
    if (File::Seek(mp_native_file, 0, SEEK_SET) != 0) return 0;
    m_position = 0;
    return plat_read(buffer, 1, std::size_t(m_file_size));
}
size_t CAsyncFileHandle::plat_read(void* buffer, size_t size, size_t count) {
    controller(); Dbg_Assert(mp_native_file);
    auto bytes = File::Read(buffer, size, count, mp_native_file);
    m_position = File::Tell(mp_native_file);
    inc_busy_count(); io_callback(m_current_function, int(bytes), 0);
    return bytes;
}
size_t CAsyncFileHandle::plat_write(void* buffer, size_t size, size_t count) {
    controller(); Dbg_Assert(mp_native_file);
    auto bytes = File::Write(buffer, size, count, mp_native_file);
    m_position = File::Tell(mp_native_file);
    inc_busy_count(); io_callback(m_current_function, int(bytes), 0);
    return bytes;
}
char* CAsyncFileHandle::plat_get_s(char* buffer, int max) {
    controller(); Dbg_Assert(mp_native_file);
    char* result = File::GetS(buffer, max, mp_native_file);
    m_position = File::Tell(mp_native_file);
    inc_busy_count(); io_callback(m_current_function, result ? int(std::strlen(result)) : 0, 0);
    return result;
}
int CAsyncFileHandle::plat_seek(long offset, int origin) {
    controller(); Dbg_Assert(mp_native_file);
    auto result = File::Seek(mp_native_file, offset, origin);
    m_position = File::Tell(mp_native_file);
    inc_busy_count(); io_callback(FUNC_SEEK, result, 0);
    return result;
}
void CAsyncFileLoader::s_plat_init() {
    controller(); Dbg_Assert(!initialized);
    Dbg_Assert(s_manager_busy_count == 0 && s_num_callbacks[0] == 0 && s_num_callbacks[1] == 0);
    for (int i = 0; i < MAX_FILE_HANDLES; ++i) { Dbg_Assert(!s_file_handles[i]); s_file_handles[i] = new CAsyncFileHandle; }
    initialized = true;
}
void CAsyncFileLoader::s_plat_cleanup() {
    controller(); Dbg_Assert(initialized && s_free_handle_index == 0);
    s_execute_callback_list();
    Dbg_Assert(s_manager_busy_count == 0);
    for (auto*& handle : s_file_handles) { delete handle; handle = nullptr; }
    initialized = false;
}
bool CAsyncFileLoader::s_plat_async_supported() { return true; } // real original deferred completion API, synchronous disk calls
bool CAsyncFileLoader::s_plat_exist(const char* name) { return File::Exist(name); }
CAsyncFileHandle* CAsyncFileLoader::s_plat_open(const char* name, int priority) { return sOpen(name, true, priority); }
bool CAsyncFileLoader::s_plat_close(CAsyncFileHandle* handle) { return sClose(handle); }
void CAsyncFileLoader::s_plat_swap_callback_list() {
    controller(); s_cur_callback_list_index ^= 1; s_new_io_completion = false;
}
void CAsyncFileLoader::s_plat_update() { controller(); Dbg_Assert(initialized); /* reads already completed by native stdio; original poll drains callbacks */ }
}
