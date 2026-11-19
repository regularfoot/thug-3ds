#include <core/defines.h>
#include <source_port/logic_timing.h>
#include <source_port/timer_state.h>
#include "audio_internal.h"

namespace SourcePort {
LogicTiming logic_timing;
bool logic_timing_active=false,logic_timing_detailed=false;
namespace {
bool sample_ready=false;
}
void BeginLogicTiming(bool sample){
    sample_ready=false;
    logic_timing_active=true;
    logic_timing_detailed=sample;
    logic_timing.Begin(source_clock_now());
    if(sample)ResetAudioTiming();
}
void EndLogicTiming(){
    if(!logic_timing_active)return;
    logic_timing.Switch(LogicArea::Other,source_clock_now());
    logic_timing_active=false;
    sample_ready=true;
}
void ReportLogicTiming(unsigned frame,bool stalled){
    if(!sample_ready||(!logic_timing_detailed&&!stalled))return;
    sample_ready=false;
    auto us=[](LogicArea area){
        auto ticks=logic_timing.ticks[static_cast<std::size_t>(area)];
        return static_cast<unsigned long long>(ScaleClockTicks(ticks,source_clock_frequency(),1000000));
    };
    Log("LOGIC n=%u detail=%u system_us=%llu object_us=%llu qb_us=%llu physics_us=%llu collision_us=%llu animation_us=%llu goals_us=%llu audio_us=%llu other_us=%llu",
        frame,unsigned(logic_timing_detailed),us(LogicArea::System),us(LogicArea::Objects),us(LogicArea::Script),us(LogicArea::Physics),
        us(LogicArea::Collision),us(LogicArea::Animation),us(LogicArea::Goals),us(LogicArea::Audio),us(LogicArea::Other));
    if(logic_timing_detailed)ReportAudioTiming(frame);
}
}
