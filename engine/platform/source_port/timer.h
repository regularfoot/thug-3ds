#pragma once
#include <cstdint>
namespace SourcePort {
// Display owner supplies actual refresh generation/wait operations. Test-only
// fixtures are explicitly bound by host tests; never installed in production.
struct TimerVideoSource {
    void* owner;
    std::uint64_t (*counter)(void* owner);
    bool (*wait_after)(void* owner, std::uint64_t previous);
};
void BindTimerVideoSource(TimerVideoSource source);
void BindNativeTimerDisplay(void* owner);
void UnbindTimerVideoSource(void* owner);
bool TimerVideoIsBound();
// Retail skeletal playback and blend deltas, in reference 60 Hz frames.
float AnimationFrameRatio();
}
