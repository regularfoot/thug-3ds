#pragma once
#include <source_port/native_ui.h>
namespace SourcePort {
struct LoadingBar {
    int x=258,y=400,width=140,height=8,border_width=5,border_height=5;
    Image::RGBA start{0,76,129,128},end{172,211,115,128},border{40,40,40,128};
};
void BuildOriginalLoadingScreen(Nx::CNativeTexture*,bool bar,const LoadingBar&,float progress,std::vector<UIQuad>&);
// Called only on the controller, between bounded disk reads or original frames.
// The source's time-based loading bar is not advertised as actual I/O progress.
void PumpLoadingScreen(bool force=false);
void ReleaseLoadingScreen();
}
