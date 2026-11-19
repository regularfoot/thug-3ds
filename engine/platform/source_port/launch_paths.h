#pragma once
#include <string>
namespace SourcePort {
struct GameLaunchPaths {std::string root,data,saves,journal;};
bool ResolveGameLaunchPaths(const char* executable,const char* fallback,GameLaunchPaths&);
void ConfigureGameLaunchPaths(int argc,char** argv);
const char* StartupSaveName();
bool SkipOpeningMovies();
bool PerformanceRouteEnabled();
bool HideVancouverAmbient();
bool CompareVancouverAmbient();
bool CompareCityOptions();
bool MoscowTurnTrial();
bool CompareStereoCityTurns();
bool ManhattanTurnTrial();
bool UseOptimizedFramePath(unsigned step);
enum TurnOption : unsigned { CacheAdmission=1, OpaqueGroups=2, AudioMix=4, SharedPose=8 };
unsigned TurnOptions(unsigned step);
bool ReuseStaticPrefix();
bool DistantPedPoses();
bool GpuSkinningEnabled();
bool TestPlayerSkinning();
bool EarlyGeometryEnabled();
bool CpuMeshCacheEnabled();
void LoadStartupSaveWhenReady();
}
