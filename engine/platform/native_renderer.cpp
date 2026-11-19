#include <source_port/stereo.h>
#include <source_port/audio.h>
#include <source_port/native_renderer.h>
#include <source_port/application_os.h>
#include <source_port/native_world.h>
#include <source_port/native_occlusion.h>
#include <source_port/native_projection.h>
#include <source_port/native_diagnostics.h>
#include <gfx/gfxman.h>
#include <source_port/native_particles.h>
#include <source_port/native_ui.h>
#include <source_port/graphics.h>
#include <source_port/texture_cache.h>
#include <source_port/thread_os.h>
#include <source_port/memory.h>
#include <source_port/clock_os.h>
#include <source_port/logic_timing.h>
#include <source_port/timer_state.h>
#include <source_port/diagnostics.h>
#include <source_port/render_options.h>
#include <source_port/launch_paths.h>
#include <source_port/input.h>
#include <gfx/nx.h>
#include <gfx/nxviewman.h>
#include <gfx/nxviewport.h>
#include <gfx/nxparticlemgr.h>
#include <gfx/nxweather.h>
#include <gfx/nxmiscfx.h>
#include <sys/timer.h>
#include <algorithm>
#ifdef __3DS__
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/gamemode.h>
#include <sk/objects/skater.h>
#include <gel/components/modelcomponent.h>
#endif
#ifdef THUG_EXPORT_TEST_SAVE
namespace SourcePort {void ExportRequestedTestSave();}
#endif
namespace {
#ifdef __3DS__
unsigned long long frame_us(uint64 ticks) {
    return SourcePort::ScaleClockTicks(ticks,source_clock_frequency(),1000000);
}
// An opt-in crossover run on one simulation. Four nine-frame blocks per
// quartet use ON, OFF, OFF, ON; omit the first frame of each block and
// the first quartet at each camera to let the GPU and scene settle.
struct VancouverTrial {
    static constexpr unsigned fountain_start=720,block=9,quartet=4*block;
    struct Sample {
        unsigned step,period_us,effects_us,draws,world_elements;
        SourcePort::AmbientEffectCounts fx;
    };
    std::vector<Sample> samples;
    static bool Hidden(unsigned step){
        const auto phase=((step-1)/block)%4;
        return phase==1||phase==2;
    }
    static bool Measured(unsigned step){
        if(!step||step>SourcePort::PerformanceRouteLength)return false;
        const unsigned local=step>fountain_start?step-fountain_start:step;
        return local>quartet&&(local-1)%block!=0;
    }
    static void FountainView(SourcePort::WorldView& view){
        // Fish fountain row in retail VC.qb, centred at (5410, 52, 1394).
        // Only the diagnostic render view moves. Simulation keeps its camera.
        view.position.Set(5410,190,2180,1);
        view.axes[Z].Set(0,0.15f,1,0);view.axes[Z].Normalize();
        view.axes[X].Set(1,0,0,0);view.axes[Y]=Mth::CrossProduct(view.axes[Z],view.axes[X]);
    }
    void Report()const{
        SourcePort::NativeHeapScope storage;
        for(bool fountain:{false,true})for(bool hidden:{false,true}){
            std::vector<unsigned> periods;
            uint64 elapsed=0,effect=0,draws=0,world=0,particles=0,weather=0,legacy=0,other=0,visible=0;
            std::array<unsigned,SourcePort::PerformanceRouteLength/quartet> block_us{};
            for(const auto& row:samples){
                if((row.step>fountain_start)!=fountain||Hidden(row.step)!=hidden)continue;
                periods.push_back(row.period_us);
                elapsed+=row.period_us;effect+=row.effects_us;draws+=row.draws;world+=row.world_elements;
                particles+=row.fx.fountain_elements;weather+=row.fx.weather_elements;visible+=row.fx.fountain_visible;
                legacy+=row.fx.legacy_elements;other+=row.fx.other_particle_elements;
                block_us[(row.step-1)/quartet]+=row.period_us;
            }
            if(periods.empty())continue;
            std::sort(periods.begin(),periods.end());
            const auto n=unsigned(periods.size());
            SourcePort::Log("VC_AMBIENT area=%s hidden=%u frames=%u elapsed_us=%llu p50_us=%u p99_us=%u effects_us=%llu draw_calls=%llu world_elements=%llu fountain_visible=%llu fountain_elements=%llu weather_elements=%llu legacy_elements=%llu other_particle_elements=%llu",
                fountain?"fountain":"route",unsigned(hidden),n,elapsed,periods[(n-1)/2],periods[(99*n+99)/100-1],
                effect,draws,world,visible,particles,weather,legacy,other);
            for(unsigned q=0;q<block_us.size();++q)if(block_us[q])
                SourcePort::Log("VC_AMBIENT_PAIR quartet=%u hidden=%u elapsed_us=%u",q,unsigned(hidden),block_us[q]);
        }
    }
};
// Moscow camera 4 from the recorded retail route. Every block first sweeps
// the same 120-degree arc in 30 warm-up frames, then samples it in 60.
struct TurnTrial {
    static constexpr unsigned block=90;
    struct Sample {unsigned us,upload,submit,pose,loads,evictions,draws,reused,groups,fx,wet,music,blocks;};
    std::array<std::vector<Sample>,16> samples;
    unsigned saved_groups=0;
    Mth::Vector reference_position,reference_back;
    bool have_reference=false;
    void Camera(SourcePort::WorldView& view,unsigned step,bool moscow){
        unsigned k=(step-1)%block;
        const unsigned phase=k<30?2*k:k-30;
        const int half=phase<=30?int(phase):60-int(phase);
        const float angle=(4*half-60)*float(Mth::PI)/180.f;
        if(!have_reference){
            reference_position=view.position;reference_back=view.axes[Z];have_reference=true;
        }
        view.position=reference_position;
        view.axes.Ident();
        view.axes[Z]=reference_back;
        if(moscow){
            view.position.Set(-2911.3f,77.4f,784.2f,1.f);
            view.axes[Z].Set(-.988f,.1505f,.0362f,0);
        }
        view.axes[Z].Normalize();
        view.axes[X]=Mth::CrossProduct(Mth::Vector(0,1,0,0),view.axes[Z]);view.axes[X].Normalize();
        view.axes[Y]=Mth::CrossProduct(view.axes[Z],view.axes[X]);view.axes.RotateY(angle);
        view.hfov=72.f;
    }
    void Record(unsigned step,unsigned period,unsigned upload,unsigned submit,unsigned pose,
                const SourcePort::WorldResidencyStats& resident,unsigned reused){
        if((step-1)%block<30)return;
        const unsigned mode=SourcePort::CompareStereoCityTurns()?unsigned(SourcePort::UseOptimizedFramePath(step)):SourcePort::TurnOptions(step);
        auto& rows=samples[mode];
        SourcePort::NativeHeapScope storage;
        if(rows.empty())rows.reserve(120);
        const auto audio=SourcePort::CurrentAudioMixTicks();
        rows.push_back({period,upload,submit,pose,unsigned(resident.uploads),unsigned(resident.evictions),
            source_graphics_frame_draw_calls(),reused,saved_groups,unsigned(frame_us(audio.effects)),
            unsigned(frame_us(audio.reverb)),unsigned(frame_us(audio.streams)),audio.blocks});
    }
    void Report(){
        for(unsigned mask=0;mask<samples.size();++mask){
            auto& rows=samples[mask];if(rows.empty())continue;
            std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.us<b.us;});
            uint64 total=0,upload=0,submit=0,pose=0,loads=0,evictions=0,draws=0,reused=0,groups=0,fx=0,wet=0,music=0,blocks=0;
            unsigned late=0,peak_loads=0;
            for(const auto& r:rows){
                total+=r.us;upload+=r.upload;submit+=r.submit;pose+=r.pose;loads+=r.loads;evictions+=r.evictions;
                draws+=r.draws;reused+=r.reused;groups+=r.groups;fx+=r.fx;wet+=r.wet;music+=r.music;blocks+=r.blocks;
                peak_loads=std::max(peak_loads,r.loads);
                late+=r.us>33333;
            }
            const unsigned n=rows.size();
            auto percentile=[&](unsigned p){return rows[(n*p+99)/100-1].us;};
            SourcePort::Log("TURN_RESULT mask=%u frames=%u total_us=%llu p50=%u p95=%u p99=%u max=%u late=%u upload_us=%llu submit_us=%llu pose_us=%llu loads=%llu evictions=%llu draws=%llu reused=%llu groups=%llu fx_us=%llu wet_us=%llu music_us=%llu audio_blocks=%llu peak_loads=%u",
                mask,n,total,percentile(50),percentile(95),percentile(99),rows.back().us,late,upload,submit,pose,loads,evictions,draws,reused,groups,fx,wet,music,blocks,peak_loads);
        }
    }
};
struct RouteProfile {
    static constexpr unsigned window_size=30;
    uint32 scene=0;
    unsigned frames=0, late=0, early_frames=0, skin_draws=0;
    uint64 previous_tick=0, previous_vblank=0, elapsed=0, window_sum=0, worst_window=0;
    uint64 geometry=0, pose=0, upload=0, ui=0, submit=0, max_period=0;
    uint64 draw_calls=0,world_packets=0,source_batches=0,triangle_elements=0;
    uint64 static_hits=0,static_uploads=0,static_evictions=0;
    // Five settings in mirrored nine-frame blocks. Rotate their order every
    // 90 frames so no setting always owns the first or last camera segment.
    bool compare_city=false;
    struct CityCase {uint64 us=0,uploads=0,prefix=0,samples=0,replays=0,early=0;unsigned frames=0;};
    std::array<CityCase,8> city_cases{};
    unsigned Options(unsigned step)const{
        if(!running||!compare_city||!step)return 7;
        constexpr unsigned options[]={0,1,2,4,7};
        const unsigned block=(step-1)/9,slot=block%10;
        return options[((slot<5?slot:9-slot)+block/10)%5];
    }
    unsigned max_uploads=0;
    uint64 prefix_hits=0,ped_samples=0,ped_replays=0,ped_visible=0,ped_far=0;
    struct RouteView {unsigned step=0; float position[3]{},backward[3]{};};
    static constexpr unsigned view_interval=180;
    std::array<RouteView,(SourcePort::PerformanceRouteLength+view_interval-1)/view_interval> views{};
    std::array<uint64,window_size> window{};
    std::array<unsigned,251> periods{};
    SourceGraphicsWait waits{};
    bool running=false,stereo=false;

    std::unique_ptr<VancouverTrial> ambient;
    std::unique_ptr<TurnTrial> turn;
    void Begin(uint32 level,bool has_stereo) {
        *this={};
        scene=level;
        compare_city=SourcePort::CompareCityOptions();
        if(level==Crc::GenerateCRCFromString("VC")&&SourcePort::CompareVancouverAmbient()){
            SourcePort::NativeHeapScope storage;
            ambient=std::make_unique<VancouverTrial>();
            ambient->samples.reserve(SourcePort::PerformanceRouteLength);
        }
        if((level==Crc::GenerateCRCFromString("RU")&&SourcePort::MoscowTurnTrial())||
           (level==Crc::GenerateCRCFromString("NY")&&SourcePort::ManhattanTurnTrial())){
            SourcePort::NativeHeapScope storage;
            turn=std::make_unique<TurnTrial>();
        }
        stereo=has_stereo;
        running=true;
        previous_tick=source_clock_now();
        previous_vblank=Tmr::GetVblanks();
        SourcePort::StartPerformanceRoute();
        SourcePort::Log("PERF_ROUTE_BEGIN scene=%08x eyes=%u",scene,stereo?2:1);
    }
    void SampleView(unsigned step,const SourcePort::WorldView& view) {
        if(!step||step>SourcePort::PerformanceRouteLength||(step-1)%view_interval)return;
        auto& sample=views[(step-1)/view_interval];
        sample.step=step;
        for(unsigned axis=0;axis<3;++axis){
            sample.position[axis]=view.position[axis];
            sample.backward[axis]=view.axes[Z][axis];
        }
    }
    void Record(uint64 geometry_ticks,uint64 pose_ticks,uint64 upload_ticks,uint64 ui_ticks,uint64 submit_ticks,
                unsigned skin_count,std::size_t early_packets,unsigned batches,const SourcePort::WorldResidencyStats& cache,unsigned elements,
                uint64 effects_ticks,const SourcePort::AmbientEffectCounts& fx,const SourcePort::WorldDetailStats& detail) {
        const auto now=source_clock_now(), vblank=Tmr::GetVblanks();
        const auto period=now-previous_tick;
        const unsigned step=SourcePort::PerformanceRouteStep();
        if(turn&&step)turn->Record(step,unsigned(frame_us(period)),unsigned(frame_us(upload_ticks)),
            unsigned(frame_us(submit_ticks)),unsigned(frame_us(pose_ticks)),cache,detail.shared_positions);
        if(ambient&&VancouverTrial::Measured(step))
            ambient->samples.push_back({step,unsigned(frame_us(period)),unsigned(frame_us(effects_ticks)),source_graphics_frame_draw_calls(),elements,fx});
        if(compare_city&&step&&(step-1)%9>=2){
            auto& c=city_cases[Options(step)];
            ++c.frames;c.us+=frame_us(period);c.uploads+=cache.uploads;c.prefix+=cache.prefix_hits;
            c.samples+=detail.ped_samples;c.replays+=detail.ped_replays;c.early+=early_packets!=0;
        }
        late+=(vblank-previous_vblank>2);
        previous_tick=now;
        previous_vblank=vblank;
        elapsed+=period;
        max_period=std::max(max_period,period);
        ++periods[std::min<unsigned long long>(periods.size()-1,frame_us(period)/1000)];
        const auto slot=frames%window_size;
        window_sum+=period;
        window_sum-=window[slot];
        window[slot]=period;
        ++frames;
        early_frames+=early_packets>0;
        skin_draws+=skin_count;
        draw_calls+=source_graphics_frame_draw_calls();
        source_batches+=batches;
        world_packets+=cache.draws;
        static_hits+=cache.hits;
        static_uploads+=cache.uploads;prefix_hits+=cache.prefix_hits;
        ped_visible+=detail.ped_visible;ped_far+=detail.ped_far;
        ped_samples+=detail.ped_samples;ped_replays+=detail.ped_replays;
        static_evictions+=cache.evictions;
        max_uploads=std::max(max_uploads,unsigned(cache.uploads));
        triangle_elements+=elements;
        if(frames>=window_size)worst_window=std::max(worst_window,window_sum);
        geometry+=geometry_ticks;
        pose+=pose_ticks;
        upload+=upload_ticks;
        ui+=ui_ticks;
        submit+=submit_ticks;
        const auto wait=source_graphics_frame_wait();
        waits.fill+=wait.fill;
        waits.draw+=wait.draw;
        waits.transfer+=wait.transfer;
        waits.swap+=wait.swap;
    }
    unsigned Percentile(unsigned percent) const {
        unsigned target=(frames*percent+99)/100;
        for(unsigned ms=0;ms<periods.size();++ms) {
            if(target<=periods[ms])return ms;
            target-=periods[ms];
        }
        return periods.size()-1;
    }
    void Finish(bool complete) {
        if(!running)return;
        if(turn)turn->Report();
        if(frames) SourcePort::Log("PERF_ROUTE scene=%08x eyes=%u complete=%u frames=%u elapsed_us=%llu p50_ms=%u p99_ms=%u max_us=%llu worst_30_us=%llu late_slots=%u geometry_us=%llu pose_us=%llu upload_us=%llu ui_us=%llu submit_us=%llu fill_us=%llu draw_wait_us=%llu transfer_us=%llu swap_us=%llu skin_draws=%u early_frames=%u",
            scene,stereo?2:1,unsigned(complete),frames,frame_us(elapsed),Percentile(50),Percentile(99),frame_us(max_period),frame_us(worst_window),late,
            frame_us(geometry)/frames,frame_us(pose)/frames,frame_us(upload)/frames,frame_us(ui)/frames,frame_us(submit)/frames,
            frame_us(waits.fill)/frames,frame_us(waits.draw)/frames,frame_us(waits.transfer)/frames,frame_us(waits.swap)/frames,
            skin_draws,early_frames);
        if(frames){
            for(unsigned i=0;i<city_cases.size();++i){
                const auto& c=city_cases[i];
                if(c.frames)SourcePort::Log("PERF_CITY_CASE scene=%08x mask=%u frames=%u elapsed_us=%llu uploads=%llu prefix=%llu samples=%llu replays=%llu early=%llu",
                    scene,i,c.frames,c.us,c.uploads,c.prefix,c.samples,c.replays,c.early);
            }
            SourcePort::Log("PERF_CITY_OPT scene=%08x prefix_hits=%llu ped_visible=%llu ped_far=%llu ped_samples=%llu ped_replays=%llu early_frames=%u",scene,prefix_hits,ped_visible,ped_far,ped_samples,ped_replays,early_frames);
            SourcePort::Log("PERF_ROUTE_WORK scene=%08x eyes=%u frames=%u draw_calls=%llu world_packets=%llu source_batches=%llu triangle_elements=%llu",
                scene,stereo?2:1,frames,draw_calls,world_packets,source_batches,triangle_elements);
            SourcePort::Log("PERF_ROUTE_CACHE scene=%08x frames=%u static_hits=%llu static_uploads=%llu static_evictions=%llu max_frame_uploads=%u",
                scene,frames,static_hits,static_uploads,static_evictions,max_uploads);
            for(const auto& view:views)if(view.step)
                SourcePort::Log("PERF_ROUTE_VIEW step=%u x=%.1f y=%.1f z=%.1f back_x=%.4f back_y=%.4f back_z=%.4f",
                    view.step,view.position[0],view.position[1],view.position[2],view.backward[0],view.backward[1],view.backward[2]);
        }
        if(ambient&&complete)ambient->Report();
        running=false;
        SourcePort::StopPerformanceRoute();
        SourcePort::RequestDiagnosticCheckpoint();
    }
};
#endif
struct FrameProfile {
    enum Phase {Shadow,Logic,Geometry,Projection,Effects,Upload,UI,Submit,Count};
    uint64 start=0,mark=0,previous_end=0,gap=0,ticks[Count]{};
    unsigned frame=0,batches=0,vertices=0,elements=0,large_frames=0,occluded_meshes=0,occluded_triangles=0,grouped_draws_saved=0;
    float far_clip=0;
    Mth::Vector camera_position;
    SourcePort::WorldDetailStats detail;
    SourcePort::WorldResidencyStats resident;
    SourcePort::AmbientEffectCounts ambient;
    void Begin(){auto now=source_clock_now();gap=previous_end?now-previous_end:0;start=mark=now;std::fill_n(ticks,Count,0);batches=vertices=elements=occluded_meshes=occluded_triangles=grouped_draws_saved=0;far_clip=0;detail={};resident={};ambient={};}
    void End(Phase phase){auto now=source_clock_now();ticks[phase]+=now-mark;mark=now;}
    void Report(float slider){
        auto end=source_clock_now();previous_end=end;++frame;
        // Bounded controller-owned measurements; no per-frame heap/file owner.
        const bool stalled=end-start+gap>=source_clock_frequency()/4;
        if(!stalled&&frame>4&&frame%32&&!(batches>=128&&large_frames++<8))return;
        auto us=[](uint64 ticks){return (unsigned long long)SourcePort::ScaleClockTicks(ticks,source_clock_frequency(),1000000);};
        auto& memory=Mem::Manager::sHandle();auto* bottom=memory.BottomUpHeap();auto* top=memory.TopDownHeap();SourceGraphicsStats gpu{};
        int stats=source_graphics_stats(&gpu);
        SourcePort::ReportLogicTiming(frame,stalled);
        if(detail.local_parts)
            SourcePort::Log("LOCAL_SKATER_DRAW n=%u parts=%u triangles=%u arrays=%u",frame,detail.local_parts,detail.local_triangles,detail.local_array_parts);
        if(occluded_meshes)SourcePort::Log("OCCLUSION n=%u meshes=%u triangles=%u",frame,occluded_meshes,occluded_triangles);
        if(detail.meshes)SourcePort::Log("DISTANT_DETAIL n=%u meshes=%u triangles=%u",frame,detail.meshes,detail.triangles);
        if(detail.lod_meshes)SourcePort::Log("WORLD_LOD n=%u meshes=%u triangles_saved=%u",
            frame,detail.lod_meshes,detail.lod_triangles_saved);
        if(detail.partition_skipped)SourcePort::Log("WORLD_CELLS n=%u sectors_skipped=%u",frame,detail.partition_skipped);
        if(detail.distant_prop_sectors)SourcePort::Log("CITY_DETAIL n=%u sectors=%u",frame,detail.distant_prop_sectors);
        if(detail.range_meshes)SourcePort::Log("WORLD_PROP_RANGE n=%u meshes=%u triangles=%u",frame,detail.range_meshes,detail.range_triangles);
        if(detail.backface_meshes)SourcePort::Log("WORLD_BACKFACES n=%u meshes=%u triangles=%u",frame,detail.backface_meshes,detail.backface_triangles);
        if(detail.occluded_sectors)SourcePort::Log("WORLD_CELL_OCCLUSION n=%u sectors=%u",frame,detail.occluded_sectors);
        if(detail.material_meshes)SourcePort::Log("WORLD_MATERIALS n=%u meshes=%u groups=%u",frame,detail.material_meshes,detail.material_groups);
        SourcePort::Log("GEOMETRY_WORK n=%u gather_us=%llu sort_us=%llu coalesce_us=%llu group_us=%llu pose_us=%llu static_budget=%zu",
            frame,us(detail.gather_ticks),us(detail.sort_ticks),us(detail.coalesce_ticks),us(detail.group_ticks),us(detail.pose_ticks),resident.budget);
        if(stalled||frame<=4||frame%128==0) {
            SourcePort::Log("HEAP_BLOCKS n=%u bottom_max=%d top_max=%d",frame,bottom->LargestFreeBlock(),top->LargestFreeBlock());
            SourcePort::ReportOldConsoleHeap();
        }
        if(far_clip>0&&source_application_os_is_old_3ds())
            SourcePort::Log("WORLD_CAMERA n=%u x=%.0f y=%.0f z=%.0f",frame,camera_position[X],camera_position[Y],camera_position[Z]);
        if(detail.ped_samples||detail.ped_replays||resident.prefix_hits)
            SourcePort::Log("CITY_OPT n=%u prefix_hits=%zu ped_samples=%u ped_replays=%u",frame,resident.prefix_hits,detail.ped_samples,detail.ped_replays);
        if(grouped_draws_saved)SourcePort::Log("DRAW_GROUP n=%u screen_disjoint_saved=%u",frame,grouped_draws_saved);
        if(!stats&&gpu.local_vertex_bindings>gpu.vertex_bindings)
            SourcePort::Log("GPU_BINDINGS n=%u actual=%u local_base=%u",frame,gpu.vertex_bindings,gpu.local_vertex_bindings);
        if(!stats&&gpu.rgba8_texture_bytes>gpu.texture_bytes)
            SourcePort::Log("GPU_TEXTURES n=%u actual=%u rgba8=%u",frame,gpu.texture_bytes,gpu.rgba8_texture_bytes);
        if(!stats&&gpu.mip_textures)
            SourcePort::Log("WORLD_MIPS n=%u textures=%u extra_bytes=%u",frame,gpu.mip_textures,gpu.mip_bytes);
        if(!stats&&(gpu.etc1_textures||gpu.etc1a4_textures))
            SourcePort::Log("TEXTURE_ETC n=%u etc1=%u etc1_bytes=%u etc1a4=%u etc1a4_bytes=%u",
                frame,gpu.etc1_textures,gpu.etc1_bytes,gpu.etc1a4_textures,gpu.etc1a4_bytes);
        if(!stats&&gpu.gpu_complete_frame_us)
            SourcePort::Log("GPU_FRAME n=%u previous_frame_queue_us=%u",frame,gpu.gpu_complete_frame_us);
        if(!stats && (gpu.framebuffer_barriers || gpu.texture_barriers))
            SourcePort::Log("PICA_BARRIERS n=%u framebuffer=%u texture=%u", frame,
                gpu.framebuffer_barriers, gpu.texture_barriers);
        if(!stats)
            SourcePort::Log("DRAW_WORK n=%u draws=%u clip_updates=%u stereo_planned=%u offset_binds=%u slider=%.3f iod=%.3f",
                frame,gpu.draw_calls,gpu.clip_updates,gpu.planned_packets,gpu.offset_bindings,slider,source_stereo_iod(slider));
        SourcePort::Log("FRAME n=%u total_us=%llu gap_us=%llu shadow_us=%llu logic_us=%llu geometry_us=%llu projection_us=%llu effects_us=%llu upload_us=%llu ui_us=%llu submit_us=%llu batches=%u vertices=%u triangle_vertices=%u bottom=%d top=%d region_free=%d gpu_vertices=%u gpu_textures=%u gpu_stats=%d gpu_chunks=%u gpu_cmd_peak=%u gpu_linear_free=%u gpu_vram_free=%u static_bytes=%u static_meshes=%u static_hits=%u static_uploads=%u static_evictions=%u streamed_meshes=%u world_packets=%u stream_vertices=%u stream_uploads=%u indexed_meshes=%u static_indexed_runs=%u stream_indices=%u far_clip=%.0f eyes=%u gpu_wait_us=%llu gpu_previous_draw_us=%u",
            frame,us(end-start),us(gap),us(ticks[Shadow]),us(ticks[Logic]),us(ticks[Geometry]),us(ticks[Projection]),us(ticks[Effects]),us(ticks[Upload]),us(ticks[UI]),us(ticks[Submit]),batches,vertices,elements,bottom->mUsedMem.m_count,top->mUsedMem.m_count,bottom->ParentRegion()->MemAvailable(),gpu.vertex_bytes,gpu.texture_bytes,stats,gpu.command_recycles,gpu.command_peak_words,gpu.linear_free,gpu.vram_free,unsigned(resident.bytes),unsigned(resident.meshes),unsigned(resident.hits),unsigned(resident.uploads),unsigned(resident.evictions),unsigned(resident.streamed),unsigned(resident.draws),unsigned(resident.stream_vertices),unsigned(resident.stream_uploads),unsigned(resident.indexed_meshes),unsigned(resident.static_indexed_runs),unsigned(resident.stream_indices),far_clip,gpu.eyes,us(gpu.wait_ticks),gpu.gpu_previous_draw_us);
    }
};
struct Renderer {
    SourcePort::WorldMeshRetention mesh_retention{4*1024*1024};
    std::vector<std::unique_ptr<SourcePort::NativeWorldBatch>> worlds;
    std::vector<SourcePort::WorldBatch> batches;
    SourcePort::WorldOcclusion occlusion;
    SourcePort::NativeUIBatch ui;std::vector<SourceDrawPacket> packets;
    bool frame=false,letterbox=false;
    float stereo=0;
    bool reported_local_frame=false;
    FrameProfile profile;
#ifdef __3DS__
    RouteProfile route;
    uint32 benchmark_scene=0;
    bool watch_moscow=false,vancouver_early=false,route_started=false;
    uint64 moscow_checkpoint=0;
    unsigned moscow_peak_geometry=0,moscow_peak_lists=0,moscow_peak_words=0,
        moscow_peak_draws=0,moscow_peak_elements=0;
#endif
};
std::unique_ptr<Renderer> renderer;
SourcePort::WorldView viewport(Nx::CViewport* port,unsigned index){
    auto* camera=port->GetCamera();Dbg_Assert(camera);SourcePort::WorldView view;view.axes=camera->GetMatrix();view.position=camera->GetPos();view.hfov=camera->GetAdjustedHFOV();
    view.aspect=port->GetAspectRatio();view.near_clip=camera->GetNearClipPlane();view.far_clip=camera->GetFarClipPlane();view.viewport=index;
    view.performance=source_render_options()->performance;
    view.x=std::clamp(int(port->GetOriginX()*400),0,399);view.y=std::clamp(int(port->GetOriginY()*240),0,239);
    view.width=std::clamp(int(port->GetWidth()*400),1,400-int(view.x));view.height=std::clamp(int(port->GetHeight()*240),1,240-int(view.y));
    return view; // original letterboxing overlays bars; it does not squeeze the world viewport
}
void letterbox(std::vector<SourcePort::UIQuad>& quads){
    for(float top:{0.f,392.f}){SourcePort::UIQuad q;q.priority=1e9f;SourceRenderVertex a{0,top,0,0,0,0,255,0,0},b=a,c=a,d=a;b.y+=56;c.y+=56;c.x=640;d.x=640;q.vertices={a,b,c,a,c,d};quads.push_back(q);}
}
}
namespace SourcePort {
bool WorldRendererReady(){Dbg_Assert(source_thread_is_controller());return renderer!=nullptr;}
bool WorldRendererFrameOpen(){Dbg_Assert(source_thread_is_controller());return renderer&&renderer->frame;}
void OpenWorldRenderer(){Dbg_Assert(source_thread_is_controller()&&GraphicsReady()&&!renderer);renderer=std::make_unique<Renderer>();
    const auto* p=source_render_options();Log("RENDER performance=%d detailed_shadows=%d blur=%d secondary_passes=%d dynamic_lighting=%d texture_limit=%u world_size=400x240 ui_size=400x240 far_clip=original_camera",p->performance,p->detailed_shadows,p->motion_blur,p->secondary_passes,p->dynamic_lighting,p->texture_limit);}
void ReleaseWorldSceneBuffers(){
    Dbg_Assert(source_thread_is_controller());
    if(!renderer)return;
    // Scene changes can occur after the current GPU packets were prepared.
    // Their resident owners must survive until submit, but the optional CPU
    // retention must release its references before the next menu allocates.
    renderer->mesh_retention.Clear();
    if(renderer->frame&&!renderer->packets.empty())return;
    renderer->packets.clear();
    // Drop the batches and their owners before the next scene, but reuse the
    // Old 3DS native allocation: menu images can fragment the freed block
    // before the very first frontend frame tries to reserve it again.
    if(source_application_os_is_old_3ds()) renderer->batches.clear();
    else decltype(renderer->batches){}.swap(renderer->batches);
    renderer->worlds.clear();
    renderer->reported_local_frame=false;
#ifdef __3DS__
    renderer->route.Finish(false);
    renderer->route_started=false;
    renderer->moscow_checkpoint=0;
    renderer->moscow_peak_geometry=renderer->moscow_peak_lists=renderer->moscow_peak_words=
        renderer->moscow_peak_draws=renderer->moscow_peak_elements=0;
#endif
}
void CloseWorldRenderer(){Dbg_Assert(source_thread_is_controller()&&renderer&&!renderer->frame);Dbg_Assert(source_graphics_wait()==0);renderer.reset();ResetTextureCacheIndex();}
}
namespace Nx {
void CEngine::s_plat_pre_render(){
    Dbg_Assert(SourcePort::WorldRendererReady()&&SourcePort::GraphicsReady()&&!renderer->frame);
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Logic);
#ifdef __3DS__
    SourcePort::LoadStartupSaveWhenReady();
#endif
    renderer->profile.Begin();source_graphics_reset_frame_timing();SourcePort::RootHeapScope scratch(SourcePort::RootHeapDirection::TopDown);if(source_render_options()->detailed_shadows)SourcePort::RenderOriginalShadowTargets(Tmr::GetTime());renderer->packets.clear();renderer->frame=true;renderer->profile.End(FrameProfile::Shadow);auto next=renderer->profile.frame+1;SourcePort::BeginLogicTiming(next<=4||next%32==0||(SourcePort::MoscowTurnTrial()&&SourcePort::PerformanceRouteStep()));
}
void CEngine::s_plat_render_world(){
    Dbg_Assert(renderer&&renderer->frame);
    uint32 now=Tmr::GetTime();int count=CViewportManager::sGetNumActiveViewports();
    SourcePort::RootHeapScope scratch(SourcePort::RootHeapDirection::TopDown);
    SourcePort::EndLogicTiming();
    renderer->profile.End(FrameProfile::Logic);
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Geometry);
    if(renderer->worlds.size()>unsigned(count))renderer->worlds.resize(count);
    while(renderer->worlds.size()<unsigned(count))renderer->worlds.push_back(std::make_unique<SourcePort::NativeWorldBatch>());
    renderer->stereo=source_graphics_slider();Dbg_Assert(std::isfinite(renderer->stereo)&&renderer->stereo>=0&&renderer->stereo<=1);
    const Nx::CModel* local_model=nullptr;
    bool local_arrays=false;
    bool city_stereo=false;
#ifdef __3DS__
    bool vancouver=sGetScene(Crc::GenerateCRCFromString("VC"))!=nullptr;
    const auto options=renderer->route.Options(SourcePort::PerformanceRouteStep());
    renderer->watch_moscow=false;
    renderer->vancouver_early=false;
    renderer->benchmark_scene=0;
    if(source_application_os_is_new_3ds()){
        auto* skate=Mdl::Skate::Instance();
        if(!skate->GetGameMode()->IsFrontEnd()){
            if(auto* skater=skate->GetLocalSkater())
                if(auto* component=GetModelComponentFromObject(skater)) local_model=component->GetModel();
            static const auto moscow=Crc::GenerateCRCFromString("RU");
            local_arrays=sGetScene(moscow)!=nullptr;
            static const auto manhattan=Crc::GenerateCRCFromString("NY");
            city_stereo=count==1&&renderer->stereo>0&&(local_arrays||sGetScene(manhattan));
            renderer->vancouver_early=vancouver && count==1 && (options&1);
            renderer->watch_moscow=local_arrays || vancouver ||
                sGetScene(Crc::GenerateCRCFromString("SE"))!=nullptr;
            if(local_model&&count==1&&SourcePort::PerformanceRouteEnabled()) {
                if(local_arrays)renderer->benchmark_scene=moscow;
                else for(const auto* name:{"NY","SC2","VN","VC","SE"}) {
                    const auto key=Crc::GenerateCRCFromString(name);
                    if(sGetScene(key)){renderer->benchmark_scene=key;break;}
                }
            }
        }
    }
#endif
    for(int i=0;i<count;++i){auto* port=CViewportManager::sGetActiveViewport(i);if(!port||!port->GetCamera())continue;auto view=viewport(port,i);if(i==0){renderer->profile.far_clip=view.far_clip;renderer->profile.camera_position=view.position;}
#ifdef __3DS__
        if(i==0 && renderer->route.running && renderer->route.turn){
            const auto step=SourcePort::PerformanceRouteStep();
            if(step){
                if((step-1)%TurnTrial::block==0)renderer->worlds[i]->Release();
                // Adjacent, untimed game-camera captures exercise the visible
                // player before a long comparison on the scripted city turn.
                if(SourcePort::CompareStereoCityTurns()&&(step==4||step==5))
                    Gfx::Manager::Instance()->ScreenShot(step==4?"skater_cpu":"skater_gpu");
                else renderer->route.turn->Camera(view,step,local_arrays);
                if(SourcePort::CompareStereoCityTurns()&&(step==20||step==110))
                    Gfx::Manager::Instance()->ScreenShot(step==20?"turn_before":"turn_after");
            }
        }
        if(vancouver){
            view.hide_vancouver_ambient=SourcePort::HideVancouverAmbient();
            auto& route=renderer->route;
            if(route.running&&route.ambient&&i==0){
                const auto step=SourcePort::PerformanceRouteStep();
                if(step&&step<=SourcePort::PerformanceRouteLength){
                    view.hide_vancouver_ambient=VancouverTrial::Hidden(step);
                    view.ambient_counts=&renderer->profile.ambient;
                    if(step>VancouverTrial::fountain_start)VancouverTrial::FountainView(view);
                }
            }
        }
        if(i==0&&renderer->route.running)renderer->route.SampleView(SourcePort::PerformanceRouteStep(),view);
#endif
        auto& batches=renderer->batches;
        if (source_application_os_is_old_3ds() && !batches.capacity()) {
            SourcePort::NativeHeapScope storage;
            batches.reserve(1400);
        }
        auto& occlusion=renderer->occlusion;
        const bool fast_city=city_stereo && SourcePort::UseOptimizedFramePath(SourcePort::PerformanceRouteStep());
        source_graphics_fast_index_commands(fast_city);
        // Citro3D's tilted projection has a horizontal view-space translation
        // of (iod / 2) * tan(hfov / 2). Use the same stereo sample as submit.
        const float eye_shift=.5f*source_stereo_iod(renderer->stereo)*std::tan(view.hfov*float(Mth::PI)/360.f);
        view.stereo_eye_shift=eye_shift;
        occlusion.Prepare(view.position,view.axes[X],i,eye_shift);view.occlusion=&occlusion;
        if(view.performance){
            const bool old_console = source_application_os_is_old_3ds();
            if(!old_console&&SourcePort::CpuMeshCacheEnabled())view.mesh_retention=&renderer->mesh_retention;
            view.direct_partition_gather=!old_console;
            // Reuse the proven two-eye conservative bounds in mono, too.
            // Full resolution and camera distance are unchanged.
            view.backface_static=true;
            view.occlude_static_cells=true;
            view.static_detail_pixels=old_console ? 8.f : 6.f;
            view.static_prop_radius=old_console ? 2750.f : 4000.f;
            view.small_props_only=true;
            if(city_stereo)view.city_prop_radius=3500.f;
            if(fast_city){
                view.prune_occluders=true;
                view.city_stereo_fast=true;
                view.static_prop_radius=1600.f;
                view.city_prop_radius=1500.f;
                view.static_detail_pixels=12.f;
            }
            view.reclaim_cold_topology=old_console;
            view.static_lod_pixels=2.5f;
            view.static_lod_min_distance=1350.f;
            if(fast_city){view.static_lod_pixels=4.f;view.static_lod_min_distance=900.f;}
            view.local_model=local_model;
            view.local_arrays=local_arrays;
#ifdef __3DS__
            view.profile_pose=renderer->route.running||renderer->profile.frame<4||(renderer->profile.frame+1)%32==0;
            if(count==1&&local_model&&!old_console){
                const auto trial=SourcePort::TurnOptions(SourcePort::PerformanceRouteStep());
                view.defer_cold_runs=(local_arrays||fast_city)&&city_stereo&&(trial&SourcePort::CacheAdmission);
                view.group_opaque_disjoint=!fast_city&&city_stereo&&(trial&SourcePort::OpaqueGroups);
                view.reuse_pose_vertices=(trial&SourcePort::SharedPose)!=0;
            }
            if(count==1&&local_model){
                view.reuse_static_prefix=(options&2)&&(vancouver||city_stereo||local_arrays)&&SourcePort::ReuseStaticPrefix();
                view.distant_ped_poses=(options&4)&&SourcePort::DistantPedPoses();
                view.vancouver_early=renderer->vancouver_early;
            }
            view.gpu_skin=count==1&&(local_arrays||fast_city)&&SourcePort::GpuSkinningEnabled();
            view.test_player_skin=view.gpu_skin&&(fast_city||SourcePort::TestPlayerSkinning());
#endif
            view.detail_stats=&renderer->profile.detail;
        }
        const bool material_sorted=SourcePort::GatherNativeWorld(view,now,batches,view.performance);
        renderer->profile.occluded_meshes+=occlusion.HiddenMeshes();
        renderer->profile.occluded_triangles+=occlusion.HiddenTriangles();
        renderer->profile.End(FrameProfile::Geometry);
        std::vector<SourcePort::WorldBatch> splats;if(source_render_options()->detailed_shadows)SourcePort::GatherOriginalProjectedShadows(view,now,splats);{SourcePort::EffectRenderScope effects(view,splats);TextureSplatRender();}
        renderer->profile.End(FrameProfile::Projection);
        auto transparent=std::find_if(batches.begin(),batches.end(),[](const auto& b){return !b.sky&&!b.opaque;});
        std::size_t sorted_prefix=material_sorted?std::size_t(transparent-batches.begin()):0;
        batches.insert(transparent,std::make_move_iterator(splats.begin()),std::make_move_iterator(splats.end()));
        {
            SourcePort::EffectRenderScope effects(view,batches);
            ShatterRender();
            const auto legacy_begin=batches.size();
            render_particles();
            if(view.ambient_counts)for(auto j=legacy_begin;j<batches.size();++j)
                view.ambient_counts->legacy_elements+=unsigned(batches[j].ElementCount());
            Dbg_Assert(mp_particle_manager&&mp_weather);
            mp_particle_manager->RenderParticles();
            if(source_render_options()->atmospheric_effects&&!view.hide_vancouver_ambient){
                const auto weather_begin=batches.size();
                mp_weather->Render();
                if(view.ambient_counts)for(auto j=weather_begin;j<batches.size();++j)
                    view.ambient_counts->weather_elements+=unsigned(batches[j].ElementCount());
            }
        }
        renderer->profile.End(FrameProfile::Effects);if(view.performance){
            // Snapshot/Shatter retains complete original CPU data; omit its
            // environment layers only at presentation, together with world reflections.
            const auto environment=[](const auto& b){return b.environment;};
            const auto first=std::find_if(batches.begin(),batches.end(),environment);
            sorted_prefix=std::min(sorted_prefix,std::size_t(first-batches.begin()));
            batches.erase(std::remove_if(first,batches.end(),environment),batches.end());
            const auto coalesce_started=source_clock_now();
            SourcePort::CoalesceOpaqueWorldBatches(batches,sorted_prefix);
            const auto group_started=source_clock_now();
            const unsigned saved=SourcePort::GroupScreenDisjointWorldBatches(view,batches);
            renderer->profile.grouped_draws_saved+=saved;
#ifdef __3DS__
            if(renderer->route.turn)renderer->route.turn->saved_groups=saved;
#endif
            renderer->profile.detail.coalesce_ticks+=group_started-coalesce_started;
            renderer->profile.detail.group_ticks+=source_clock_now()-group_started;
        }
        renderer->profile.batches+=batches.size();
        for(const auto& b:batches){
            renderer->profile.vertices+=b.VertexCount();
            renderer->profile.elements+=b.ElementCount();
        }
        std::vector<SourceDrawPacket> additional_view;
        auto& packets=renderer->packets.empty()?renderer->packets:additional_view;
        SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Upload);
        bool prepared=renderer->worlds[i]->Prepare(view,batches,packets);Dbg_MsgAssert(prepared,("Original world GPU upload failed errno=%d",errno));
        auto residency=renderer->worlds[i]->ResidencyStats();auto& total=renderer->profile.resident;total.bytes+=residency.bytes;total.meshes+=residency.meshes;total.hits+=residency.hits;total.prefix_hits+=residency.prefix_hits;total.uploads+=residency.uploads;total.evictions+=residency.evictions;total.streamed+=residency.streamed;total.draws+=residency.draws;total.stream_vertices+=residency.stream_vertices;total.stream_uploads+=residency.stream_uploads;total.indexed_meshes+=residency.indexed_meshes;total.static_indexed_runs+=residency.static_indexed_runs;total.stream_indices+=residency.stream_indices;total.budget+=residency.budget;
        renderer->packets.insert(renderer->packets.end(),additional_view.begin(),additional_view.end());
        batches.clear();
        renderer->profile.End(FrameProfile::Upload);
    }
}
void CEngine::s_plat_post_render(){
    Dbg_Assert(renderer&&renderer->frame);
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::HUD);
    float stereo=renderer->stereo;
    // The retail renderer clears ordinary frames even when a script asks
    // to disable color clearing. Its blur has a separate history buffer.
    const uint32 clear_color=renderer->letterbox?0x000000ff:0x506070ff;
    std::size_t early=0;
#ifdef __3DS__
    if(source_application_os_is_new_3ds()&&SourcePort::EarlyGeometryEnabled()&&(!renderer->watch_moscow||renderer->vancouver_early)&&
       CViewportManager::sGetNumActiveViewports()==1&&Mdl::Skate::Instance()->GetLocalSkater()&&
       !Mdl::Skate::Instance()->GetGameMode()->IsFrontEnd()&&!renderer->letterbox){
        early=source_graphics_early_prefix(renderer->packets.data(),renderer->packets.size(),stereo>0?2:1);
        if(renderer->vancouver_early){
            // Preserve the sky preamble and require actual static scenery in this prefix.
            while(early && renderer->packets[early-1].early_opaque!=2)--early;
        }
        if(early==renderer->packets.size())early=0;
        if(early){
            SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Submit);
            Dbg_MsgAssert(source_graphics_submit_early(renderer->packets.data(),early,clear_color,stereo,renderer->vancouver_early)==0,
                ("Early opaque GPU submit failed errno=%d",errno));
            renderer->profile.End(FrameProfile::Submit);
        }
    }
#endif
    SourcePort::RootHeapScope scratch(SourcePort::RootHeapDirection::TopDown);
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::HUD);
    std::vector<SourcePort::UIQuad> quads;
    SourcePort::GatherNativeUI(quads);
    if(renderer->letterbox)letterbox(quads);
    renderer->profile.End(FrameProfile::UI);
    if(early){
        SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Submit);
        Dbg_MsgAssert(source_graphics_wait_early()==0,("Early opaque GPU fence failed errno=%d",errno));
        renderer->profile.End(FrameProfile::Submit);
    }
    std::vector<SourceDrawPacket> ui;
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Upload);
    Dbg_Assert(renderer->ui.Prepare(quads,ui));
    renderer->packets.insert(renderer->packets.end(),ui.begin(),ui.end());
    renderer->profile.End(FrameProfile::UI);
    const auto& packets=renderer->packets;
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame+1,SourcePort::DiagnosticFramePhase::Submit);
    int result;
    if(early)result=source_graphics_submit_remaining(packets.data()+early,packets.size()-early,stereo);
#ifdef __3DS__
    else if(renderer->watch_moscow)result=source_graphics_submit_bounded(packets.data(),packets.size(),clear_color,stereo);
#endif
    else result=source_graphics_submit(packets.data(),packets.size(),clear_color,stereo);
    Dbg_MsgAssert(result==0,("Original frame GPU submit failed errno=%d",errno));
    renderer->profile.End(FrameProfile::Submit);
    renderer->frame=false;renderer->profile.Report(stereo);SourcePort::FinishNativeDiagnostics();
    if(renderer->profile.frame<=4||renderer->profile.frame%32==0)
        SourcePort::Log("CPU_MESH_CACHE n=%u bytes=%zu meshes=%zu hits=%u builds=%u",renderer->profile.frame,
            renderer->mesh_retention.Bytes(),renderer->mesh_retention.Meshes(),
            renderer->profile.detail.retained_hits,renderer->profile.detail.retained_builds);
    SourcePort::MarkDiagnosticFrame(renderer->profile.frame,SourcePort::DiagnosticFramePhase::Idle);
#ifdef __3DS__
    auto& profile=renderer->profile;
    if(renderer->route.running) {
        const auto step=SourcePort::PerformanceRouteStep();
        if(!step||renderer->route.scene!=renderer->benchmark_scene||renderer->route.stereo!=(stereo>0))renderer->route.Finish(false);
        else {
            renderer->route.Record(profile.ticks[FrameProfile::Geometry],profile.detail.pose_ticks,
                profile.ticks[FrameProfile::Upload],profile.ticks[FrameProfile::UI],profile.ticks[FrameProfile::Submit],profile.detail.skin_draws,early,
                profile.batches,profile.resident,profile.elements,profile.ticks[FrameProfile::Effects],profile.ambient,profile.detail);
            if(step==SourcePort::PerformanceRouteLength)renderer->route.Finish(true);
        }
    }else if(renderer->benchmark_scene&&!renderer->route_started&&profile.detail.local_parts) {
        renderer->route_started=true;
        renderer->route.Begin(renderer->benchmark_scene,stereo>0);
    }
    const auto now=source_clock_now();
    if(renderer->watch_moscow) {
        const auto lists=source_graphics_frame_lists();
        renderer->moscow_peak_geometry=std::max(renderer->moscow_peak_geometry,profile.elements);
        renderer->moscow_peak_lists=std::max(renderer->moscow_peak_lists,lists.count);
        renderer->moscow_peak_words=std::max(renderer->moscow_peak_words,lists.peak_words);
        renderer->moscow_peak_draws=std::max(renderer->moscow_peak_draws,lists.peak_draws);
        renderer->moscow_peak_elements=std::max(renderer->moscow_peak_elements,lists.peak_elements);
    }
    if(renderer->watch_moscow&&now-renderer->moscow_checkpoint>=5*source_clock_frequency()) {
        renderer->moscow_checkpoint=now;
        const auto wait=source_graphics_frame_wait();
        SourcePort::Log("MOSCOW_PRESENT n=%u eyes=%u skater_parts=%u triangles=%u arrays=%u draw_wait_us=%llu transfer_us=%llu skin_draws=%u early_packets=%zu",
            profile.frame,stereo>0?2:1,profile.detail.local_parts,profile.detail.local_triangles,
            profile.detail.local_array_parts,frame_us(wait.draw),frame_us(wait.transfer),profile.detail.skin_draws,early);
        SourcePort::Log("MOSCOW_GPU n=%u eyes=%u frame_elements_peak=%u list_count_peak=%u list_words_peak=%u list_draws_peak=%u list_elements_peak=%u",
            profile.frame,stereo>0?2:1,renderer->moscow_peak_geometry,renderer->moscow_peak_lists,
            renderer->moscow_peak_words,renderer->moscow_peak_draws,renderer->moscow_peak_elements);
        // Keep regular samples in the rolling RAM journal. A bulk append on
        // the game's SD card can block soundtrack reads for several seconds.
    }
    if (renderer->profile.frame == 1) SourcePort::BootPhase("first_engine_frame");
    if (source_application_os_is_new_3ds() && renderer->profile.detail.local_parts &&
        !renderer->reported_local_frame){
        renderer->reported_local_frame=true;
        SourcePort::Log("LOCAL_SKATER_FIRST_FRAME n=%u parts=%u triangles=%u",renderer->profile.frame,
            renderer->profile.detail.local_parts,renderer->profile.detail.local_triangles);
    }
#endif
#ifdef THUG_EXPORT_TEST_SAVE
    SourcePort::ExportRequestedTestSave();
#endif
}
void CEngine::s_plat_set_color_buffer_clear(bool){
    // Match the original PC/Xbox callback, which leaves clearing enabled.
    Dbg_Assert(renderer);
}
void CEngine::s_plat_set_letterbox(bool on){Dbg_Assert(renderer);renderer->letterbox=on;}
bool CEngine::s_plat_is_visible(Mth::Vector& sphere,float radius){
    for(int i=0;i<CViewportManager::sGetNumActiveViewports();++i){auto* port=CViewportManager::sGetActiveViewport(i);if(port&&port->GetCamera()){auto copy=sphere;copy[3]=radius;if(SourcePort::SphereVisible(viewport(port,i),copy))return true;}}return false;
}
}
