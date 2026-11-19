#include <core/defines.h>
#include <sys/timer.h>
#include <sys/config/config.h>
#include <source_port/timer.h>
#include <source_port/timer_state.h>
#include <source_port/clock_os.h>
#include <source_port/thread_os.h>
#include <cerrno>

namespace {
SourcePort::TimerState state;
SourcePort::TimerVideoSource video{};
bool native_video = false;
std::uint64_t video_total = 0, last_source_counter = 0, last_sync = 0;
std::uint64_t native_total = 0;
std::uint32_t native_previous = 0;
void controller() { Dbg_Assert(source_thread_is_controller()); }
std::uint64_t native_counter(void*) {
    std::uint32_t counter;
    int result = source_clock_video_counter(&counter);
    Dbg_Assert(result == 0);
    return SourcePort::ExtendVideoCounter(counter, native_previous, native_total);
}
bool native_wait(void*, std::uint64_t previous) {
    // Native total is an extension of the same 32-bit IRQ count.
    return source_clock_video_wait(std::uint32_t(previous)) == 0;
}
std::uint64_t read_video() {
    controller(); Dbg_Assert(video.counter);
    auto current = video.counter(video.owner);
    Dbg_Assert(current >= last_source_counter);
    auto delta = current - last_source_counter;
    Dbg_Assert(video_total <= UINT64_MAX - delta);
    video_total += delta; last_source_counter = current;
    return video_total;
}
void wait_video(bool always_wait) {
    controller(); Dbg_Assert(state.Ready()); Dbg_Assert(video.counter);
    auto current = read_video();
    if (always_wait || current == last_sync) {
        auto before = last_source_counter;
        bool success = video.wait_after(video.owner, before);
        Dbg_MsgAssert(success, ("Native vblank wait failed errno=%d", errno));
        auto after = video.counter(video.owner);
        Dbg_Assert(after > before);
        current = read_video();
    }
    last_sync = current;
}
}
namespace SourcePort {
void BindTimerVideoSource(TimerVideoSource source) {
    controller(); Dbg_Assert(state.Ready() && !video.counter);
    Dbg_Assert(source.owner && source.counter && source.wait_after);
    video = source; last_source_counter = video.counter(video.owner); last_sync = video_total;
}
void BindNativeTimerDisplay(void* owner) {
    controller(); Dbg_Assert(state.Ready() && owner && !video.counter);
    int result = source_clock_video_open();
    Dbg_MsgAssert(result == 0, ("Native timer display is unavailable errno=%d", errno));
    BindTimerVideoSource({owner, native_counter, native_wait}); native_video = true;
}
void UnbindTimerVideoSource(void* owner) {
    controller(); Dbg_Assert(video.counter && owner == video.owner);
    (void)read_video();
    if (native_video) {
        int result = source_clock_video_close();
        Dbg_Assert(result == 0);
    }
    video = {}; native_video = false;
}
bool TimerVideoIsBound() { controller(); return video.counter != nullptr; }
float AnimationFrameRatio() { controller(); return state.AnimationFrameRatio(); }
}
namespace Tmr {
void Init() { controller(); state.Initialize(source_clock_now(), source_clock_frequency(), Config::FPS()); }
void DeInit() { controller(); Dbg_Assert(!video.counter); state.Shutdown(); }
void PreUpdateTimerInfo() {
    controller();
    state.Begin(source_clock_now());
}
void OncePerRender() { controller(); state.End(); }
uint64 GetRenderFrame() { controller(); return state.RenderFrame(); }
Time GetTime() { controller(); return state.Milliseconds(source_clock_now()); }
MicroSeconds GetTimeInUSeconds() { return SourcePort::ScaleClockTicks(source_clock_now(), source_clock_frequency(), 1000000); }
CPUCycles GetTimeInCPUCycles() {
    // Original QB/Object profiler consumers divide by 150/150000000. Preserve
    // that ABI unit using REAL reference ticks, not CPU speed or a zero stub.
    return SourcePort::ScaleClockTicks(source_clock_now(), source_clock_frequency(), 150000000);
}
float FrameLength() { controller(); return state.FrameLength(); }
double UncappedFrameLength() { controller(); return state.UncappedFrameLength(); }
float GetSlomo() { controller(); return state.Slomo(); }
void SetSlomo(float value) { controller(); state.SetSlomo(value); }
void RestartClock() { controller(); state.Restart(source_clock_now()); }
void StoreTimerInfo() { controller(); state.Store(source_clock_now()); }
void RecallTimerInfo() { controller(); state.Recall(source_clock_now()); }
void Vblank() {
    int result = source_clock_video_notify();
    Dbg_MsgAssert(result == 0, ("Unowned native vblank IRQ errno=%d", errno));
}
uint64 GetVblanks() { return read_video(); }
void VSync() { wait_video(true); }
void VSync1() { wait_video(false); }
}
