#include <source_port/native_loadscreen.h>
#include <source_port/application_os.h>
#include <source_port/native_renderer.h>
#include <source_port/graphics.h>
#include <source_port/clock_os.h>
#include <source_port/thread_os.h>
#include <gfx/nxloadscreen.h>
#include <algorithm>
namespace {
struct Screen {
    std::unique_ptr<Nx::CNativeTexture> picture;
    SourcePort::NativeUIBatch batch;
    SourcePort::LoadingBar bar;
    bool displayed=false,blank=false,timed=false,drawing=false;
    uint64 started=0,last=0;float seconds=0;
};
std::unique_ptr<Screen> screen;
void quad(std::vector<SourcePort::UIQuad>& out,Nx::CNativeTexture* texture,float x,float y,float w,float h,Image::RGBA left,Image::RGBA right){
    if(w<=0||h<=0)return;SourcePort::UIQuad q;q.texture=texture;
    SourceRenderVertex a{x,y,0,left.r,left.g,left.b,left.a,0,0},b=a,c=a,d=a;
    b.y+=h;b.v=1;c.x+=w;c.y+=h;c.u=c.v=1;d.x+=w;d.u=1;
    c.r=d.r=right.r;c.g=d.g=right.g;c.b=d.b=right.b;c.a=d.a=right.a;
    q.vertices={a,b,c,a,c,d};out.push_back(q);
}
}
namespace SourcePort {
void BuildOriginalLoadingScreen(Nx::CNativeTexture* texture,bool bar,const LoadingBar& p,float progress,std::vector<UIQuad>& out){
    if(texture)quad(out,texture,0,0,640,448,Image::RGBA(128,128,128,128),Image::RGBA(128,128,128,128));
    if(!bar)return;Dbg_Assert(std::isfinite(progress));progress=std::clamp(progress,0.f,1.f);
    quad(out,nullptr,float(p.x)-p.border_width,float(p.y)-p.border_height,float(p.width)+2.f*p.border_width,float(p.height)+2.f*p.border_height,p.border,p.border);
    // Original console loading-bar geometry/color rules, driven by elapsed
    // wall time on the controller instead of a GPU-mutating timer interrupt.
    Image::RGBA end;for(unsigned i=0;i<4;++i)reinterpret_cast<uint8*>(&end)[i]=uint8(reinterpret_cast<const uint8*>(&p.start)[i]*(1-progress)+reinterpret_cast<const uint8*>(&p.end)[i]*progress);
    quad(out,nullptr,p.x,p.y,float(p.width)*progress,p.height,p.start,end);
}
void PumpLoadingScreen(bool force){
    Dbg_Assert(source_thread_is_controller());if(!screen||!screen->displayed||screen->drawing||!GraphicsReady()||WorldRendererFrameOpen())return;
    uint64 now=source_clock_now(),frequency=source_clock_frequency();if(!force&&now-screen->last<frequency/30)return;
    screen->drawing=true;screen->last=now;float progress=screen->timed?std::clamp(float(double(now-screen->started)/frequency/screen->seconds),0.f,1.f):0;
    std::vector<UIQuad> quads;BuildOriginalLoadingScreen(screen->picture.get(),screen->timed,screen->bar,progress,quads);
    std::vector<SourceDrawPacket> packets;Dbg_Assert(screen->batch.Prepare(quads,packets));
    Dbg_Assert(source_graphics_submit_frame(packets.data(),packets.size(),0x000000ff,0,screen->blank||screen->picture!=nullptr)==0);screen->drawing=false;
}
void ReleaseLoadingScreen(){Dbg_Assert(source_thread_is_controller());if(screen){Dbg_Assert(!screen->drawing);screen.reset();}}
}
namespace Nx {
void CLoadScreen::s_plat_display(const char* filename,bool just_freeze,bool blank){
    Dbg_Assert(source_thread_is_controller()&&SourcePort::GraphicsReady());screen=std::make_unique<Screen>();screen->blank=blank;
    if(!just_freeze&&!blank){
#ifdef __3DS__
        if(!source_application_os_is_new_3ds()) screen->blank=true;
#endif
        if(!screen->blank){
            Dbg_Assert(filename&&*filename);screen->picture=std::make_unique<CNativeTexture>();
            Dbg_MsgAssert(screen->picture->LoadTexture(filename,true,false),("Missing original loading screen: %s",filename));
        }
    }
    screen->displayed=true;if(!just_freeze)SourcePort::PumpLoadingScreen(true);
}
void CLoadScreen::s_plat_clear(){SourcePort::ReleaseLoadingScreen();}
void CLoadScreen::s_plat_hide(){if(screen){Dbg_Assert(!screen->drawing);screen->displayed=false;}}
void CLoadScreen::s_plat_start_loading_bar(float seconds){
    Dbg_Assert(std::isfinite(seconds)&&seconds>0&&screen&&screen->displayed);screen->seconds=seconds;screen->started=source_clock_now();screen->timed=true;SourcePort::PumpLoadingScreen(true);
}
void CLoadScreen::s_plat_update_bar_properties(){
    if(!screen)return;screen->bar={s_bar_x,s_bar_y,s_bar_width,s_bar_height,s_bar_border_width,s_bar_border_height,s_bar_start_color,s_bar_end_color,s_bar_border_color};
    Dbg_Assert(s_bar_width>=0&&s_bar_height>=0&&s_bar_border_width>=0&&s_bar_border_height>=0);if(screen->timed)SourcePort::PumpLoadingScreen(true);
}
}
