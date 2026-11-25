#include <core/defines.h>
#include <source_port/launch_paths.h>
#include <source_port/files.h>
#include <source_port/save.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include <sys/stat.h>
#include <source_port/build_id.h>
#include <source_port/application_os.h>
#include <source_port/memory.h>
#include <source_port/graphics_os.h>
#include <source_port/input_os.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <strings.h>
#ifdef __3DS__
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/gamemode.h>
#include <gel/scripting/struct.h>
#endif
namespace SourcePort {
namespace {
std::string startup_save;
bool skip_opening = false;
bool performance_route = false;
int trial_level = -1;
bool hide_vancouver_ambient = true;
bool compare_city_options = false;
bool moscow_turn_trial = false;
bool compare_release_turn = false;
bool compare_stereo_turns = false, manhattan_turn = false;
bool dense_draw_trial=false, dense_walls=false, dense_reference=false;
bool paired_dense_walls=false, reverse_dense_walls=false;
bool tail_trial=false, reverse_effects=false, keep_buffers=false;
bool cpu_trial=false,reverse_cpu=false,cpu_fast=false;
bool overlap_trial=false,reverse_overlap=false,overlap_enabled=false;
bool manhattan_frame_trial=false, reverse_manhattan_trial=false;
unsigned manhattan_frame_options=0;
bool compare_strips = false, reverse_strip_trial = false, source_strips = false;
bool reuse_static_prefix = true;
bool distant_ped_poses = true;
bool compare_vancouver_ambient = false;
bool gpu_skinning = true;
bool test_player_skin = false;
bool early_geometry = true;
bool cpu_mesh_cache = true;

void read_startup_options(const std::string& root) {
    struct stat st{};
    skip_opening = stat((root + "/skip_opening.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode);
    performance_route = stat((root + "/perf_route.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode);
    const auto marker=[&](const char* name){return stat((root + "/" + name).c_str(),&st)==0 && S_ISREG(st.st_mode);};
    hide_vancouver_ambient = !marker("show_vancouver_fountains.txt") || marker("no_vancouver_ambient.txt");
    moscow_turn_trial = performance_route && marker("compare_moscow_turn.txt");
    manhattan_turn = moscow_turn_trial && marker("compare_manhattan_night.txt");
    compare_stereo_turns = moscow_turn_trial && (manhattan_turn || marker("compare_moscow_night.txt"));
    dense_draw_trial=compare_stereo_turns && marker("compare_dense_draws.txt");
    paired_dense_walls=dense_draw_trial && marker("paired_dense_walls.txt");
    reverse_dense_walls=paired_dense_walls && marker("reverse_dense_walls.txt");
    dense_reference=dense_draw_trial && marker("reference_without_walls.txt");
    tail_trial=dense_draw_trial || (manhattan_turn && marker("trace_manhattan_tail.txt"));
    dense_walls=dense_draw_trial ? marker("dense_wall_candidates.txt") : !marker("legacy_dense_walls.txt");
    reverse_effects=tail_trial && marker("reverse_tail_effects.txt");
    keep_buffers=tail_trial ? marker("keep_turn_buffers.txt") : !marker("legacy_turn_buffers.txt");
    cpu_trial=!tail_trial && manhattan_turn && marker("compare_city_cpu.txt");
    reverse_cpu=cpu_trial && marker("reverse_city_cpu.txt");
    cpu_fast=!marker("legacy_city_cpu.txt");
    overlap_trial=!tail_trial && !cpu_trial && manhattan_turn && marker("compare_frame_overlap.txt");
    reverse_overlap=overlap_trial && marker("reverse_frame_overlap.txt");
    overlap_enabled=marker("frame_overlap.txt");
    manhattan_frame_trial=!tail_trial && !cpu_trial && !overlap_trial && manhattan_turn && marker("compare_manhattan_frame.txt");
    reverse_manhattan_trial=manhattan_frame_trial && marker("reverse_manhattan_frame.txt");
    manhattan_frame_options=unsigned(marker("wide_static_materials.txt")) | (unsigned(marker("frame_core2.txt"))<<1);
    compare_strips = !tail_trial && !cpu_trial && !overlap_trial && !manhattan_frame_trial && compare_stereo_turns && marker("compare_source_strips.txt");
    reverse_strip_trial = compare_strips && marker("reverse_strip_trial.txt");
    source_strips = marker("source_strips.txt");
    // The test save unlocks both classic levels in the regular Free Skate menu.
    trial_level = compare_stereo_turns ? (manhattan_turn ? 1 : 7) : -1;
    if (dense_draw_trial && marker("trial_hawaii.txt")) trial_level = 4;
    if (dense_draw_trial && marker("trial_vancouver.txt")) trial_level = 5;
    if (performance_route && marker("trial_slam_city.txt")) trial_level = 6;
    if (performance_route && marker("trial_created_park.txt")) trial_level = 9;
    if (performance_route && marker("trial_school_ii.txt")) trial_level = 10;
    if (performance_route && marker("trial_venice.txt")) trial_level = 11;
    if(performance_route){
        if(FILE* file=std::fopen((root+"/trial_level.txt").c_str(),"r")){
            unsigned selected=0;
            if(std::fscanf(file,"%u",&selected)==1 && selected<=12)trial_level=int(selected);
            std::fclose(file);
        }
    }
    compare_release_turn = moscow_turn_trial && marker("compare_moscow_release.txt");
    compare_city_options = performance_route && marker("compare_city_options.txt");
    reuse_static_prefix = !marker("exact_static_runs.txt");
    distant_ped_poses = !marker("full_ped_poses.txt");
    compare_vancouver_ambient = performance_route && stat((root + "/compare_vancouver_ambient.txt").c_str(), &st)==0 && S_ISREG(st.st_mode);
    gpu_skinning = !(stat((root + "/cpu_skinning.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode));
    test_player_skin = stat((root + "/test_player_skin.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode);
    early_geometry = !(stat((root + "/serial_geometry.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode));
    cpu_mesh_cache = !(stat((root + "/no_cpu_mesh_cache.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode));
    source_graphics_probe_inputs(stat((root + "/capture_gpu_inputs.txt").c_str(), &st)==0 && S_ISREG(st.st_mode));
    source_graphics_single_draw(stat((root + "/single_draw_gpu.txt").c_str(), &st)==0 && S_ISREG(st.st_mode));
    startup_save.clear();
    FILE* file = std::fopen((root + "/autoload_save.txt").c_str(), "rb");
    if (!file) return;

    char bytes[35]{};
    const auto count = std::fread(bytes, 1, sizeof(bytes), file);
    const bool complete = std::feof(file) && !std::ferror(file);
    std::fclose(file);
    std::string name(bytes, count);
    if (!name.empty() && name.back() == '\n') name.pop_back();
    if (!name.empty() && name.back() == '\r') name.pop_back();
    constexpr char allowed[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _-";
    if (complete && !name.empty() && name.size() <= 32 && name.find_first_not_of(allowed) == std::string::npos) {
        startup_save = std::move(name);
    } else {
        Log("STARTUP_SAVE ignored invalid autoload_save.txt");
    }
}
}

const char* StartupSaveName() { return startup_save.c_str(); }

bool FreeSkateLevelMenuOverride(unsigned script, unsigned game_type, bool career, bool network) {
    if (career || network || (game_type != 0x1ca38b5c && game_type != 0x34861a16)) return false;
    // These retail QB scripts read level-unlock flags while building/counting
    // entries and creating map markers. Never change the saved career flags.
    return script == 0x780f8810 || script == 0xb8a8f7ed || script == 0xa2f7cb51;
}

bool OldFreeSkateAmbientDisabled() {
#ifdef __3DS__
    if (!source_application_os_is_old_3ds()) return false;
    auto* skate = Mdl::Skate::Instance();
    if (!skate || !skate->GetGameMode()) return false;
    // The retail High Score / Free Skate menu requests singlesession. The
    // front end also uses these modes for preview stages and Create-A-Trick.
    const auto requested = skate->GetGameType();
    if (requested != 0x1ca38b5c && requested != 0x34861a16) return false;
    if (skate->GetGameMode()->GetNameChecksum() != requested) return false;
    switch (skate->m_requested_level) {
        case 0xf6c822d4: // load_nj
        case 0x7276630a: // load_ny
        case 0xd7720de9: // load_fl
        case 0xee1c63cf: // load_sd
        case 0x399bd4e8: // load_hi
        case 0x0d0f0229: // load_vc
        case 0x09a44ec8: // load_sj
        case 0x9db7727c: // load_ru
        case 0x991b5359: // load_se
        case 0x8ca30405: // load_sc2
        case 0x73be7e94: // load_vn
        case 0xa7ff414b: // load_hn
        case 0x7078f66c: // load_sc
        case 0xcc877c27: // load_ph
        case 0x0c27ca5e: // load_dj
        case 0xfc5833ee: // load_au
        case 0xb664035d: // load_sk5ed_gameplay
            return true;
        default:
            return false;
    }
#else
    return false;
#endif
}

bool OmitSceneryScript(unsigned checksum) {
    switch(checksum){
        case 223617057u: case 2871208853u: // All-star sign
        case 1315561669u: case 475123554u: // Video screens
        case 930395668u: case 2432941472u: // Jumbotron animation
        case 624853004u: case 3157774774u: case 3408978208u:
        case 1431598211u: case 1096100044u: // Crowd visual loops
            return OldFreeSkateAmbientDisabled();
        default:return false;
    }
}

bool IsAmbientModelPath(const char* name) {
    // QB model names are relative to models/. Match whole directory components.
    if (!name) return false;
    // Retain the Moscow tank model and its preloads in Old Free Skate.
    std::string path(name);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!strcasecmp(path.c_str(), "veh/ru/veh_tank_ru/veh_tank_ru.mdl") ||
        !strcasecmp(path.c_str(), "veh/ru/veh_tank_ru/veh_tank_ru") ||
        !strcasecmp(path.c_str(), "veh/veh_skitchnode/veh_skitchnode.mdl") ||
        !strcasecmp(path.c_str(), "veh/veh_skitchnode/veh_skitchnode")) return false;
    for (const char* directory : {"peds", "ped_male", "ped_female", "animals", "veh"}) {
        const auto length = std::strlen(directory);
        if (!strncasecmp(name, directory, length) && (name[length] == '/' || name[length] == '\\')) return true;
    }
    return false;
}

bool OmitAmbientNode(Script::CStruct* node) {
#ifdef __3DS__
    if (!source_application_os_is_old_3ds()) return false;
    uint32 type = 0;
    node->GetChecksum(0x12b4e660 /* Class */, &type);
    uint32 subtype = 0;
    if (type == 0xe47f1b79 &&
        node->GetChecksum(0x7321a8d6 /* Type */, &subtype) && subtype == 0x8ba78fe2 /* SkitchNode */)
        return false;
    const char* model = nullptr;
    if (node->GetText(0x286a8d26 /* Model */, &model) && model) {
        std::string path(model);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (!strcasecmp(path.c_str(), "veh/ru/veh_tank_ru/veh_tank_ru.mdl") ||
            !strcasecmp(path.c_str(), "veh/ru/veh_tank_ru/veh_tank_ru")) return false;
    }
    switch (type) {
        case 0xa0dfac98: // Pedestrian
        case 0x061a741e: // Ped
        case 0xe47f1b79: // Vehicle
        case 0x19b1e241: // ParticleEmitter
        case 0x9e7d469e: // ParticleObject
            break;
        case 0xef59c100: { // GameObject can also be a rail, platform or pickup.
            const char* model = nullptr;
            if (!node->GetText(0x286a8d26 /* Model */, &model) || !IsAmbientModelPath(model)) return false;
            break;
        }
        default:
            return false;
    }
    return OldFreeSkateAmbientDisabled();
#else
    (void)node;
    return false;
#endif
}

bool SkipOpeningMovies() { return skip_opening; }
bool HideVancouverAmbient() { return hide_vancouver_ambient; }
static bool second_turn_variant(unsigned step){
    if(!step || step>1080)return true;
    if(step==5)return true; // warm-up visual check paired with step 4
    constexpr bool abba[]={false,true,true,false,false,true,true,false,false,true,true,false};
    return abba[(step-1)/90];
}
bool CityTailTrial(){return tail_trial;}
const char* DenseDrawScene(){
    if(!dense_draw_trial)return nullptr;
    switch(trial_level){
        case 1:return "NY";
        case 4:return "HI";
        case 5:return "VC";
        case 7:return "RU";
        default:return nullptr;
    }
}
bool DenseWallReference(){return dense_reference;}
bool UseDenseWalls(bool level_default,unsigned step){
    // Only enable the extra tests by default on levels where the camera
    // comparison demonstrated a benefit.
    if(dense_reference)return false;
    if(paired_dense_walls)return step && step<=1080 && (second_turn_variant(step)!=reverse_dense_walls);
    return dense_walls && (dense_draw_trial || (level_default && !moscow_turn_trial));
}
bool KeepTurnBuffers(){return keep_buffers && (tail_trial || !moscow_turn_trial);}
bool OmitTurnEffects(unsigned step){
    return tail_trial && !dense_draw_trial && step && step<=1080 && (((step-1)/90%4==1 || (step-1)/90%4==2)!=reverse_effects);
}
bool CompareCityCpu(){return cpu_trial;}
bool CityCpuFast(unsigned step){
    if(!cpu_trial)return cpu_fast && (tail_trial || !moscow_turn_trial);
    return step && step<=1080 && (second_turn_variant(step)!=reverse_cpu);
}
bool CompareFrameOverlap(){return overlap_trial;}
bool FrameOverlapEnabled(unsigned step){
    if(!overlap_trial)return overlap_enabled;
    if(step==0||step>1080)return false;
    return second_turn_variant(step)!=reverse_overlap;
}
bool CompareManhattanFrame(){return manhattan_frame_trial;}
unsigned ManhattanFrameOptions(unsigned step){
    if(!manhattan_frame_trial)return manhattan_frame_options;
    if(!step || step>1080)return 0;
    constexpr unsigned order[]={0,1,3,2,1,2,0,3,2,3,1,0};
    const unsigned mode=order[(step-1)/90];
    return reverse_manhattan_trial?3-mode:mode;
}
bool CompareSourceStrips(){return compare_strips;}
bool SourceStripsEnabled(unsigned step){return compare_strips ? (second_turn_variant(step) != reverse_strip_trial) : source_strips;}
bool UseOptimizedFramePath(unsigned step){
    if(tail_trial||cpu_trial||compare_strips||manhattan_frame_trial||overlap_trial)return true;
    return compare_stereo_turns ? second_turn_variant(step) : !moscow_turn_trial;
}
int FreeSkateTrialLevel(){return trial_level;}
bool ManhattanTurnTrial(){return manhattan_turn;}
bool CompareStereoCityTurns(){return compare_stereo_turns;}
bool MoscowTurnTrial(){return moscow_turn_trial;}
unsigned TurnOptions(unsigned step){
    constexpr unsigned all=CacheAdmission|OpaqueGroups|AudioMix|SharedPose;
    if(compare_stereo_turns||!moscow_turn_trial||step==0||step>1080)return all;
    if(compare_release_turn){
        constexpr unsigned release_sequence[]={0,all,all,0,0,all,all,0,0,all,all,0};
        return release_sequence[(step-1)/90];
    }
    constexpr unsigned sequence[]={0,CacheAdmission,OpaqueGroups,AudioMix,SharedPose,all,
                                   all,SharedPose,AudioMix,OpaqueGroups,CacheAdmission,0};
    return sequence[(step-1)/90];
}
bool CompareCityOptions(){return compare_city_options;}
bool ReuseStaticPrefix(){return reuse_static_prefix;}
bool DistantPedPoses(){return distant_ped_poses;}
bool CompareVancouverAmbient() { return compare_vancouver_ambient; }
bool PerformanceRouteEnabled() { return performance_route; }
bool GpuSkinningEnabled() { return gpu_skinning; }
bool TestPlayerSkinning() { return test_player_skin; }
bool EarlyGeometryEnabled() { return early_geometry; }
bool CpuMeshCacheEnabled() { return cpu_mesh_cache; }

bool ResolveGameLaunchPaths(const char* executable,const char* fallback,GameLaunchPaths& out){
    std::string base=fallback?fallback:"";
    if(executable&&*executable){std::string file=executable;
        if(file.size()>=5&&file.substr(file.size()-5)==".3dsx"&&(file.front()=='/'||file.compare(0,6,"sdmc:/")==0))base=file.substr(0,file.find_last_of('/'));
    }
    while(base.size()>1&&base.back()=='/')base.pop_back();
    if(base.empty()||base.size()>768||(base.front()!='/'&&base.compare(0,6,"sdmc:/")!=0)){errno=EINVAL;return false;}
    if(base.find('\\')!=std::string::npos||base.find("//")!=std::string::npos){errno=EINVAL;return false;}
    std::size_t start=base.front()=='/'?1:6;while(start<base.size()){auto slash=base.find('/',start);auto part=base.substr(start,slash==std::string::npos?slash:slash-start);if(part=="."||part==".."||part.find(':')!=std::string::npos){errno=EINVAL;return false;}if(slash==std::string::npos)break;start=slash+1;}
    GameLaunchPaths paths;paths.root=base;paths.data=base+"/data";paths.saves=base+"/source-saves";paths.journal=base+"/source_engine_boot.txt";out=std::move(paths);return true;
}
void ConfigureGameLaunchPaths(int argc,char** argv){
    Dbg_Assert(source_thread_is_controller());GameLaunchPaths paths;Dbg_Assert(ResolveGameLaunchPaths(argc>0&&argv?argv[0]:nullptr,"sdmc:/3ds/" THUG_SOURCE_APP_FOLDER,paths));
    struct stat st{};Dbg_MsgAssert(stat(paths.data.c_str(),&st)==0&&S_ISDIR(st.st_mode),("Missing original game Data at %s",paths.data.c_str()));
    Dbg_Assert(ConfigureFileRoots(paths.data.c_str()));Dbg_Assert(ConfigureSaveRoot(paths.saves.c_str()));
    Dbg_MsgAssert(OpenDiagnosticLog(paths.journal.c_str()),("Cannot open this build's journal: %s",paths.journal.c_str()));
    source_input_set_settings_root(paths.root.c_str());
    read_startup_options(paths.root);
    Log("Whole original source-engine launch: resources=%s isolated-saves=%s",paths.data.c_str(),paths.saves.c_str());
#ifdef THUG_ARM_LTO
    Log("ARM_LTO enabled=1");
#else
    Log("ARM_LTO enabled=0");
#endif
#ifdef __3DS__
    Log("CONSOLE model=%s engine_heap_bytes=%u",source_application_os_is_new_3ds()?"New 3DS":"Old 3DS",
        unsigned(EngineArenaCapacity()));
    Log("RENDER_EXPERIMENTS gpu_skin=%u early_geometry=%u test_player_skin=%u",
        unsigned(source_application_os_is_new_3ds()&&gpu_skinning),unsigned(source_application_os_is_new_3ds()&&early_geometry),
        unsigned(source_application_os_is_new_3ds()&&gpu_skinning&&test_player_skin));
#endif
}
}
