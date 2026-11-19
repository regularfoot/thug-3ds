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
    // The test save unlocks both classic levels in the regular Free Skate menu.
    trial_level = compare_stereo_turns ? (manhattan_turn ? 1 : 7) : -1;
    if (performance_route && marker("trial_school_ii.txt")) trial_level = 10;
    if (performance_route && marker("trial_venice.txt")) trial_level = 11;
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
bool SkipOpeningMovies() { return skip_opening; }
bool HideVancouverAmbient() { return hide_vancouver_ambient; }
bool UseOptimizedFramePath(unsigned step){
    if(!compare_stereo_turns)return !moscow_turn_trial;
    if(!step || step>1080)return true;
    if(step==5)return true; // warm-up visual check paired with step 4
    constexpr bool abba[]={false,true,true,false,false,true,true,false,false,true,true,false};
    return abba[(step-1)/90];
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
