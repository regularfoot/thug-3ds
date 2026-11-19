#include <core/defines.h>
#include <source_port/timer_state.h>
#include <limits>

namespace SourcePort {
std::uint64_t ScaleClockTicks(std::uint64_t ticks, std::uint64_t frequency, std::uint64_t units) {
    Dbg_Assert(frequency >= 1 && frequency <= 1000000000 && units >= 1 && units <= frequency);
    return (ticks / frequency) * units + ((ticks % frequency) * units) / frequency;
}
std::uint64_t ExtendVideoCounter(std::uint32_t current, std::uint32_t& previous, std::uint64_t& total) {
    auto delta = std::uint32_t(current - previous);
    Dbg_Assert(total <= UINT64_MAX - delta);
    total += delta; previous = current;
    return total;
}
void TimerState::Initialize(std::uint64_t now, std::uint64_t frequency, int fps) {
    Dbg_Assert(!initialized_);
    Dbg_Assert(frequency >= 1000 && frequency <= 1000000000 && (fps == 50 || fps == 60));
    frequency_ = frequency; fps_ = fps; initialized_ = true; frame_open_ = false;
    ResetHistory(now); // RenderFrame and Slomo retain their process lifetime.
}
void TimerState::Shutdown() {
    Dbg_Assert(initialized_ && !frame_open_);
    initialized_ = false; saved_ = false;
}
std::uint64_t TimerState::Elapsed(std::uint64_t now) const {
    Dbg_Assert(initialized_);
    Dbg_Assert(now >= base_raw_);
    return now - base_raw_;
}
void TimerState::ResetHistory(std::uint64_t now) {
    base_raw_ = now; history_ = {};
    for (auto& sample : history_.samples) sample = 1;
    history_.frame_length = slomo_ / float(fps_);
    history_.uncapped_length = 1.0 / fps_;
    history_.animation_ratio = 60.0f / fps_;
    saved_ = false;
}
void TimerState::Begin(std::uint64_t now) {
    Dbg_Assert(initialized_ && !frame_open_);
    const auto elapsed = Elapsed(now);
    const auto delta = elapsed - history_.last_raw;
    history_.last_raw = elapsed;
    // Playback can advance by fractional visual frames. Retain the original
    // exceptional-stall cutoff so an I/O pause cannot skip an entire clip.
    history_.animation_ratio = delta > frequency_ * 10 / fps_ ?
        60.0f / fps_ : float(60.0 * double(delta) / double(frequency_));
    auto tick = ScaleClockTicks(elapsed, frequency_, fps_);
    Dbg_Assert(tick >= history_.last_tick);
    history_.samples[history_.index] = tick - history_.last_tick;
    history_.last_tick = tick; history_.index = (history_.index + 1) % 4;
    std::uint64_t uncapped = 0;
    unsigned capped = 0;
    for (auto sample : history_.samples) {
        Dbg_Assert(uncapped <= UINT64_MAX - sample);
        uncapped += sample;
        // Original console smoothing and exceptional-stall policy.
        if (sample > 10 || sample <= 1) sample = 1;
        if (sample > 4) sample = 4;
        capped += unsigned(sample);
    }
    history_.frame_length = (float(capped) / 4.0f) / float(fps_) * slomo_;
    history_.uncapped_length = (double(uncapped) / 4.0) / fps_;
    frame_open_ = true;
}
void TimerState::End() {
    Dbg_Assert(initialized_ && frame_open_);
    ++rendered_frames_; frame_open_ = false;
}
void TimerState::Restart(std::uint64_t now) {
    (void)Elapsed(now); ResetHistory(now); // May be called inside a logical frame.
}
void TimerState::Store(std::uint64_t now) {
    Dbg_Assert(initialized_ && !saved_);
    (void)Elapsed(now); stored_history_ = history_; saved_ = true;
}
void TimerState::Recall(std::uint64_t now) {
    Dbg_Assert(initialized_ && saved_);
    history_ = stored_history_;
    // Original shipped Store/Recall have no active replay-clock rollback.
    // Keep GetTime continuous: networking/UI may hold absolute deadlines.
    // Restore predictive history, rebasing its sample marker to the resume
    // instant so time spent in the pause menu isn't a giant next physics step.
    history_.last_raw = Elapsed(now);
    history_.last_tick = ScaleClockTicks(history_.last_raw, frequency_, fps_);
    saved_ = false;
    // Don't rewind game milliseconds, RenderFrame, physical IRQs or profiling.
}
std::uint32_t TimerState::Milliseconds(std::uint64_t now) const {
    return std::uint32_t(ScaleClockTicks(Elapsed(now), frequency_, 1000));
}
float TimerState::FrameLength() const { Dbg_Assert(initialized_); return history_.frame_length; }
double TimerState::UncappedFrameLength() const { Dbg_Assert(initialized_); return history_.uncapped_length; }
float TimerState::AnimationFrameRatio() const { Dbg_Assert(initialized_); return history_.animation_ratio * slomo_; }
void TimerState::SetSlomo(float value) { Dbg_Assert(std::isfinite(value) && value >= 0); slomo_ = value; }
float TimerState::Slomo() const { return slomo_; }
std::uint64_t TimerState::RenderFrame() const { return rendered_frames_; }
}
