#pragma once
#include <cstdint>
namespace SourcePort {
// Integer conversion for the fixed SDK/monotonic clock rates. Division before
// multiplication prevents long-uptime overflow and float millisecond drift.
std::uint64_t ScaleClockTicks(std::uint64_t ticks, std::uint64_t frequency, std::uint64_t units);
std::uint64_t ExtendVideoCounter(std::uint32_t current, std::uint32_t& previous, std::uint64_t& total);

// Native platform state behind the ACTUAL original sys/timer.h API. The four
// samples and 1/4/>10-tick policy follow the original console timer, not the PC
// fixed-1/60 clock. Tests drive this same class with explicit synthetic times;
// the production instance only receives the real OS clock.
class TimerState {
public:
    void Initialize(std::uint64_t now, std::uint64_t frequency, int fps);
    void Shutdown();
    void Begin(std::uint64_t now);
    void End();
    void Restart(std::uint64_t now);
    void Store(std::uint64_t now);
    void Recall(std::uint64_t now);
    std::uint32_t Milliseconds(std::uint64_t now) const;
    float FrameLength() const;
    double UncappedFrameLength() const;
    float AnimationFrameRatio() const;
    void SetSlomo(float value);
    float Slomo() const;
    std::uint64_t RenderFrame() const;
    bool Ready() const { return initialized_; }
    bool FrameOpen() const { return frame_open_; }
private:
    struct History {
        std::uint64_t samples[4]{};
        std::uint64_t last_tick = 0, last_raw = 0;
        float animation_ratio = 0;
        unsigned index = 0;
        float frame_length = 0;
        double uncapped_length = 0;
    } history_, stored_history_;
    std::uint64_t Elapsed(std::uint64_t now) const;
    void ResetHistory(std::uint64_t now);
    std::uint64_t base_raw_ = 0, frequency_ = 0;
    std::uint64_t rendered_frames_ = 0;
    int fps_ = 0;
    float slomo_ = 1;
    bool initialized_ = false, frame_open_ = false, saved_ = false;
};
}
