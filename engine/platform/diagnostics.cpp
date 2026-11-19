#include <core/defines.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include <source_port/clock_os.h>
#include <source_port/build_id.h>
#include <source_port/fatal.h>
#include <source_port/graphics_os.h>
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#ifdef __3DS__
#include <unwind.h>
#include <3ds.h>
#endif

namespace {
bool ready = false;
std::atomic<bool> fatal_started{false};
bool screen_enabled = false;
SourcePort::AssertionDisplay display = nullptr;
void* display_owner = nullptr;
FILE* journal = nullptr;
bool journal_error = false;
char gpu_capture_path[1024]{};
#ifdef __3DS__
constexpr char boot_trace_option[] = "sdmc:/3ds/" THUG_SOURCE_APP_FOLDER "/trace_boot.txt";
constexpr char boot_trace_path[] = "sdmc:/3ds/" THUG_SOURCE_APP_FOLDER "/boot_trace.txt";
bool boot_trace_enabled = false;
#endif

// Keep journal text and file ownership under separate locks. A slow SD write
// must never hold the lock used by ordinary frame and audio logging.
std::atomic_flag journal_lock = ATOMIC_FLAG_INIT;
std::atomic_flag journal_file_lock = ATOMIC_FLAG_INIT;
struct JournalGuard {
    explicit JournalGuard(std::atomic_flag& lock = journal_lock) : lock(lock) {
        while (lock.test_and_set(std::memory_order_acquire)) source_thread_sleep_ms(1);
    }
    ~JournalGuard() { lock.clear(std::memory_order_release); }
    std::atomic_flag& lock;
};

struct JournalPage {
    char bytes[128 * 1024];
    std::size_t used = 0;
    unsigned lines = 0;

    void Clear() { used = lines = 0; }
};

struct JournalBuffer {
    // Keep the launch record even if a long session overwrites older frames.
    char first_record[2048];
    std::size_t first_size = 0;
    JournalPage pages[2];
    unsigned newest = 0;
    unsigned long long overwritten = 0;

    void Append(const char* text) {
        const auto length = std::strlen(text);
        if (!first_size && length < sizeof(first_record)) {
            std::memcpy(first_record, text, length);
            first_record[length] = '\n';
            first_size = length + 1;
            return;
        }
        if (length + 1 > sizeof(pages[0].bytes)) {
            ++overwritten;
            return;
        }
        if (length + 1 > sizeof(pages[0].bytes) - pages[newest].used) {
            newest ^= 1;
            overwritten += pages[newest].lines;
            pages[newest].Clear();
        }
        auto& page = pages[newest];
        std::memcpy(page.bytes + page.used, text, length);
        page.used += length;
        page.bytes[page.used++] = '\n';
        ++page.lines;
    }

    bool Flush(FILE* file) {
        bool ok = std::fwrite(first_record, 1, first_size, file) == first_size;
        if (overwritten && std::fprintf(file, "JOURNAL overwritten_records=%llu\n", overwritten) < 0) ok = false;
        for (const unsigned index : {newest ^ 1, newest}) {
            const auto& page = pages[index];
            if (std::fwrite(page.bytes, 1, page.used, file) != page.used) ok = false;
        }
        Clear();
        return std::fflush(file) == 0 && ok;
    }

    void Clear() {
        first_size = 0;
        newest = 0;
        overwritten = 0;
        for (auto& page : pages) page.Clear();
    }
};
JournalBuffer buffers[2];
JournalBuffer* recent = &buffers[0];
JournalBuffer* queued = nullptr;
SourceThreadOS* journal_worker = nullptr;
std::atomic<bool> stop_worker{false};
std::atomic<unsigned> frame_activity{0}, frame_number{0}, frame_phase{0};
std::atomic<unsigned> gpu_command{0}, gpu_serial{0};

// Caller owns the file lock. Keep queued immutable until the write completes.
void flush_queued() {
    JournalBuffer* pending;
    {
        JournalGuard guard;
        pending = queued;
    }
    if (!pending) return;
    const auto started = source_clock_now();
    if (!pending->Flush(journal)) journal_error = true;
    {
        JournalGuard guard;
        queued = nullptr;
    }
    const auto elapsed = source_clock_now() - started;
    const auto frequency = source_clock_frequency();
    if (elapsed >= frequency / 10)
        SourcePort::Log("DIAGNOSTIC_SD_WRITE ms=%llu", (unsigned long long)(elapsed*1000/frequency));
}

bool flush_synchronously() {
    JournalGuard file_guard(journal_file_lock);
    flush_queued();
    JournalGuard guard;
    if (journal && !recent->Flush(journal)) journal_error = true;
    return !journal_error;
}

void drain_journal(void*, void*) {
    constexpr const char* phases[]{"idle", "logic", "geometry", "upload", "hud", "submit"};
    unsigned observed = 0;
    bool reported = false;
    uint64_t last_progress = source_clock_now();
    while (!stop_worker.load(std::memory_order_acquire)) {
        const auto activity = frame_activity.load(std::memory_order_acquire);
        const auto phase = frame_phase.load(std::memory_order_relaxed);
        const auto now = source_clock_now();
        if (activity != observed || !phase) {
            observed = activity;
            last_progress = now;
            reported = false;
        } else if (!reported && now - last_progress >= 3 * source_clock_frequency()) {
            const auto frame = frame_number.load(std::memory_order_relaxed);
            const auto command = gpu_command.load(std::memory_order_relaxed);
            const auto serial = gpu_serial.load(std::memory_order_relaxed);
            if (activity == frame_activity.load(std::memory_order_acquire)) {
                JournalGuard file_guard(journal_file_lock);
                flush_queued();
                // In particular, don't wait for a GSP IPC or query GPU
                // registers from the watchdog. The controller may be there.
                if (std::fprintf(journal, "FRAME_STALL frame=%u phase=%s gx_command=%u serial=%u\n",
                                 frame, phases[phase], command, serial) < 0 || std::fflush(journal))
                    journal_error = true;
                reported = true;
            }
        }
        {
            JournalGuard file_guard(journal_file_lock);
            flush_queued();
        }
        source_thread_sleep_ms(100);
    }
}

void write_message(const char* text, bool urgent = false) {
    {
        JournalGuard guard;
#ifdef __3DS__
        if (!journal || urgent)
#endif
        {
            std::fprintf(stderr, "%s\n", text);
            std::fflush(stderr);
        }
        if (journal) recent->Append(text);
    }
    if (urgent) flush_synchronously();
}
#ifdef __3DS__
struct AssertionStack { uintptr_t pc[12]; unsigned count = 0; };
_Unwind_Reason_Code save_assertion_frame(_Unwind_Context* context, void* opaque) {
    auto& stack = *static_cast<AssertionStack*>(opaque);
    if (stack.count == sizeof(stack.pc) / sizeof(stack.pc[0])) return _URC_END_OF_STACK;
    stack.pc[stack.count++] = _Unwind_GetIP(context);
    return _URC_NO_REASON;
}
#endif
[[noreturn]] void fatal(const char* file, int line, const char* condition, const char* reason, void* caller) {
    fatal_started.store(true, std::memory_order_relaxed);
    char text[1536];
    std::snprintf(text, sizeof(text), "SOURCE ENGINE ASSERT %s:%d: %s\n%s\nassert_return=%p",
                  file, line, condition, reason ? reason : "", caller);
    write_message(text,true);
#ifdef __3DS__
    // Keep the original assertion even if a damaged stack cannot unwind.
    if (source_thread_is_controller()) {
        AssertionStack stack;
        _Unwind_Backtrace(save_assertion_frame, &stack);
        char trace[192] = "assert_stack=";
        std::size_t used = std::strlen(trace);
        for (unsigned i = 0; i < stack.count; ++i) {
            used += std::snprintf(trace + used, sizeof(trace) - used, "%s0x%08lx",
                                  i ? " " : "", static_cast<unsigned long>(stack.pc[i]));
        }
        write_message(trace, true);
    }
#endif
    // Never call the renderer from an IO worker, or recursively on an error in
    // the display callback itself. The textual record remains available.
    if (source_thread_is_controller() && screen_enabled && display) {
        screen_enabled = false;
        display(text, display_owner);
    }
#ifdef __3DS__
    // newlib abort calls libctru _exit, which unmaps the shared heaps before
    // the loader takes over. NDSP and IO threads can still be using them.
    // A terminal assertion has already flushed its evidence; the kernel
    // ends every thread together without running a second teardown.
    svcExitProcess();
#else
    std::abort();
#endif
}
}
extern "C" void source_diagnostics_gpu_command(unsigned command,unsigned serial) {
    gpu_serial.store(serial,std::memory_order_relaxed);
    gpu_command.store(command,std::memory_order_relaxed);
}
extern "C" void source_graphics_report_fence_failure(const char* waiter,const SourceGpuFenceReport* r) {
    char text[640];
    std::snprintf(text,sizeof(text),
        "%s waiter=%s packets=%u eyes=%u indexed=%u words=%u serial=%u fill=%u/%u p3d=%u/%u ppf=%u/%u vblank=%u gx_command=%u gx_result=0x%08x list_bytes=%u packet_span=%u..%u eye_mask=%u elements=%u largest_draw=%u largest_packet=%u last_element=%u overlay_packets=%u",
        r->queue_error ? "GPU_QUEUE_REJECTED" : "GPU_FENCE_TIMEOUT wait_ms=2000",
        waiter,r->packets,r->eyes,r->indexed,r->words,r->serial,
        r->fill_irq,r->fill,r->draw_irq,r->draw,r->transfer_irq,r->transfer,r->vblank_irq,
        r->command,unsigned(r->queue_error),r->list_bytes,r->first_packet,r->last_packet,r->eye_mask,
        r->elements,r->largest_draw,r->largest_packet,r->last_element,r->overlay_packets);
    write_message(text,true);
}
extern "C" void source_graphics_report_gpu_status(uint32_t busy,int32_t read_result) {
    char text[128];
    std::snprintf(text,sizeof(text),"GPU_BUSY register=0x%08x read_result=0x%08x",
                  unsigned(busy),unsigned(read_result));
    write_message(text,true);
}
extern "C" void source_graphics_report_command_engine(uint32_t expected_address,unsigned expected_bytes,
    uint32_t address_units,uint32_t size_units,uint32_t trigger,int32_t read_result) {
    char text[256];
    const char* match=read_result ? "unknown" :
        (address_units==expected_address/8 && size_units==expected_bytes/8 ? "yes" : "no");
    std::snprintf(text,sizeof(text),
        "GPU_COMMAND_ENGINE expected_address=0x%08x expected_bytes=%u address_units=0x%08x size_units=0x%08x trigger=0x%08x read_result=0x%08x match=%s",
        unsigned(expected_address),expected_bytes,unsigned(address_units),unsigned(size_units),
        unsigned(trigger),unsigned(read_result),match);
    write_message(text,true);
}
extern "C" void source_graphics_report_present_timeout(unsigned vblanks) {
    char text[128];
    std::snprintf(text,sizeof(text),"GPU_PRESENT_TIMEOUT screen=top wait_ms=250 vblank=%u",vblanks);
    write_message(text,true);
}
extern "C" void source_graphics_record_failed_commands(unsigned serial,const uint32_t* list,std::size_t bytes) {
    if(!source_thread_is_controller() || !gpu_capture_path[0] || !list || !bytes || bytes>1024*1024)return;
    bool saved=false;
    char path[1088]{};
    const auto tick=static_cast<unsigned long long>(source_clock_now());
    for(unsigned i=0;i<8;++i){
        const int n=std::snprintf(path,sizeof(path),"%s.pica-%u-%llx-%u.bin",gpu_capture_path,serial,tick,i);
        if(n<0||n>=int(sizeof(path)))break;
        const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0){if(errno==EEXIST)continue;break;}
        if(auto* file=fdopen(fd,"wb")){
            const bool written=std::fwrite(list,1,bytes,file)==bytes;
            saved=std::fclose(file)==0&&written;
        }else close(fd);
        if(!saved)unlink(path);
        break;
    }
    char text[1280];
    std::snprintf(text,sizeof(text),"GPU_COMMAND_CAPTURE serial=%u bytes=%zu saved=%u path=%s",
        serial,bytes,unsigned(saved),saved?path:"-");
    write_message(text,true);
}
extern "C" void source_graphics_record_failed_inputs(unsigned serial,int (*produce)(FILE*,void*),void* context) {
    if(!source_thread_is_controller() || !gpu_capture_path[0] || !produce)return;
    char path[1088]{};
    bool saved=false;
    const auto tick=static_cast<unsigned long long>(source_clock_now());
    for(unsigned i=0;i<8;++i){
        const int n=std::snprintf(path,sizeof(path),"%s.inputs-%u-%llx-%u.bin",gpu_capture_path,serial,tick,i);
        if(n<0 || n>=int(sizeof(path)))break;
        const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0){if(errno==EEXIST)continue;break;}
        if(auto* file=fdopen(fd,"wb")){
            const bool written=produce(file,context)==0;
            saved=std::fclose(file)==0 && written;
        }else close(fd);
        if(!saved)unlink(path);
        break;
    }
    char text[1280];
    std::snprintf(text,sizeof(text),"GPU_INPUT_CAPTURE serial=%u saved=%u path=%s",
        serial,unsigned(saved),saved?path:"-");
    write_message(text,true);
}
namespace SourcePort {
#ifdef __3DS__
void OpenBootTrace() {
    const int previous_errno = errno;
    const int option = open(boot_trace_option, O_RDONLY);
    if (option >= 0) {
        close(option);
        boot_trace_enabled = true;
        BootPhase("userAppInit");
    }
    errno = previous_errno;
}

void BootPhase(const char* phase) {
    if (!boot_trace_enabled) return;
    const int previous_errno = errno;
    // Runs before C++ constructors on the first call. Each short record is
    // flushed independently in case a reset is needed before normal exit.
    char record[160];
    const int size = std::snprintf(record, sizeof(record), "%s heap=%u linear=%u\n", phase,
                                   static_cast<unsigned>(envGetHeapSize()),
                                   static_cast<unsigned>(envGetLinearHeapSize()));
    if (size > 0 && static_cast<std::size_t>(size) < sizeof(record)) {
        const int fd = open(boot_trace_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            if (write(fd, record, static_cast<std::size_t>(size)) == size) fsync(fd);
            close(fd);
        }
    }
    errno = previous_errno;
}
#endif

bool FatalDiagnosticInProgress() noexcept { return fatal_started.load(std::memory_order_relaxed); }
[[noreturn]] void Assertion(const char* file, int line, const char* condition) {
    fatal(file, line, condition, nullptr, __builtin_extract_return_addr(__builtin_return_address(0)));
}
[[noreturn]] void AssertionMessage(const char* file, int line, const char* condition, const char* format, ...) {
    char reason[768];
    va_list args; va_start(args, format);
    std::vsnprintf(reason, sizeof(reason), format, args);
    va_end(args);
    fatal(file, line, condition, reason, __builtin_extract_return_addr(__builtin_return_address(0)));
}
void Log(const char* format, ...) {
    char text[2048];
    va_list args; va_start(args, format);
    int length = std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0) { write_message("diagnostic format error"); return; }
    write_message(text);
    if (length >= int(sizeof(text))) write_message("[diagnostic truncated]");
}
bool DiagnosticSessionReady() { return ready; }
bool OpenDiagnosticLog(const char* path) {
    if (!source_thread_is_controller()) { errno = EPERM; return false; }
    if (!ready || !path || !*path) { errno = EINVAL; return false; }
    JournalGuard guard;
    if (journal) { errno = EALREADY; return false; }
    journal = std::fopen(path, "a");
    if (!journal) return false;
    auto n=std::snprintf(gpu_capture_path,sizeof(gpu_capture_path),"%s",path);
    if(n<0 || n>=int(sizeof(gpu_capture_path)))gpu_capture_path[0]=0;
    recent = &buffers[0];
    for (auto& buffer : buffers) buffer.Clear();
    queued = nullptr;
    journal_error = false;
    frame_phase.store(0,std::memory_order_relaxed);
    gpu_command.store(0,std::memory_order_relaxed);
    stop_worker.store(false,std::memory_order_release);
    uint32_t id;
    journal_worker = source_thread_create(drain_journal,nullptr,32768,13,&id);
    if (journal_worker && source_thread_start(journal_worker,nullptr)) {
        source_thread_destroy(journal_worker);
        journal_worker = nullptr;
    }
    return true;
}
bool CheckpointDiagnosticLog() {
    if (!source_thread_is_controller()) { errno = EPERM; return false; }
    return flush_synchronously();
}
void RequestDiagnosticCheckpoint() {
    if (!source_thread_is_controller() || !journal_worker) return;
    JournalGuard guard;
    if (queued) return;
    queued = recent;
    recent = recent == &buffers[0] ? &buffers[1] : &buffers[0];
    recent->Clear();
}
void MarkDiagnosticFrame(unsigned frame, DiagnosticFramePhase phase) {
    frame_number.store(frame,std::memory_order_relaxed);
    frame_phase.store(unsigned(phase),std::memory_order_relaxed);
    frame_activity.fetch_add(1,std::memory_order_release);
}
bool CloseDiagnosticLog() {
    if (!source_thread_is_controller()) { errno = EPERM; return false; }
    stop_worker.store(true,std::memory_order_release);
    if (journal_worker) {
        if (source_thread_destroy(journal_worker)) return false;
        journal_worker = nullptr;
    }
    const bool flushed = flush_synchronously();
    JournalGuard guard;
    if (!journal) return true;
    const bool closed = std::fclose(journal) == 0;
    journal = nullptr;
    gpu_capture_path[0]=0;
    return flushed && closed && !journal_error;
}

void BindAssertionDisplay(AssertionDisplay callback, void* owner) {
    Dbg_Assert(source_thread_is_controller());
    Dbg_Assert(ready && callback && owner && !display);
    display = callback; display_owner = owner;
}
void UnbindAssertionDisplay(void* owner) {
    Dbg_Assert(source_thread_is_controller());
    Dbg_Assert(display && display_owner == owner);
    screen_enabled = false; display = nullptr; display_owner = nullptr;
}
}
namespace Dbg {
void SetUp() {
    Dbg_Assert(source_thread_is_controller());
    Dbg_Assert(!ready);
    ready = true;
    SourcePort::Log("Native diagnostics started (renderer display unbound)");
}
void CloseDown() {
    Dbg_Assert(source_thread_is_controller());
    Dbg_Assert(ready && !display);
    SourcePort::Log("Native diagnostics closed");
    bool closed = SourcePort::CloseDiagnosticLog();
    Dbg_Assert(closed);
    screen_enabled = false; ready = false;
}
void screen_assert(bool on) {
    Dbg_Assert(source_thread_is_controller());
    Dbg_Assert(!on || (ready && display));
    screen_enabled = on;
}
}
