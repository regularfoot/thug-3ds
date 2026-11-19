#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <source_port/clock_os.h>

namespace SourcePort {
enum class LogicArea {Other,System,Objects,Script,Physics,Collision,Animation,Goals,Audio,Count};
struct LogicTiming {
    std::array<std::uint64_t,static_cast<std::size_t>(LogicArea::Count)> ticks{};
    std::uint64_t mark=0;
    LogicArea area=LogicArea::Other;
    void Begin(std::uint64_t now){ticks.fill(0);mark=now;area=LogicArea::Other;}
    void Switch(LogicArea next,std::uint64_t now){
        ticks[static_cast<std::size_t>(area)]+=now-mark;
        mark=now;
        area=next;
    }
};
extern LogicTiming logic_timing;
extern bool logic_timing_active, logic_timing_detailed;
void BeginLogicTiming(bool sample);
void EndLogicTiming();
void ReportLogicTiming(unsigned frame, bool stalled);

// Only coarse scopes run on every frame. Detailed collision, QB and other
// scopes retain their sampled cost while an unexpected stall still has an owner.
class ScopedLogicTiming {
public:
    explicit ScopedLogicTiming(LogicArea area){
        if(logic_timing_active && logic_timing.area!=area &&
           (logic_timing_detailed || area==LogicArea::System || area==LogicArea::Objects || area==LogicArea::Physics || area==LogicArea::Audio)){
            previous_=logic_timing.area;
            logic_timing.Switch(area,source_clock_now());
        }
    }
    ~ScopedLogicTiming(){
        if(previous_!=LogicArea::Count)logic_timing.Switch(previous_,source_clock_now());
    }
    ScopedLogicTiming(const ScopedLogicTiming&)=delete;
    ScopedLogicTiming& operator=(const ScopedLogicTiming&)=delete;
private:
    LogicArea previous_=LogicArea::Count;
};
}
