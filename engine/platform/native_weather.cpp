#include <source_port/native_weather.h>
#include <source_port/render_options.h>
#include <gfx/nx.h>
#include <gfx/nxtexman.h>
#include <gfx/nxviewman.h>
#include <gfx/nxviewport.h>
#include <engine/supersector.h>
#include <sk/engine/feeler.h>
#include <sk/modules/skate/skate.h>
#include <sk/objects/skater.h>
#include <cmath>
namespace {
float roof(float x,float z){CFeeler feeler;feeler.SetStart(Mth::Vector(x,50000,z,1));feeler.SetEnd(Mth::Vector(x,-50000,z,1));return feeler.GetCollision(false,false)?feeler.GetPoint()[Y]:50000;}
Gfx::Camera* camera(){if(!Nx::CViewportManager::sGetNumActiveViewports())return nullptr;auto* viewport=Nx::CViewportManager::sGetActiveViewport(0);return viewport?viewport->GetCamera():nullptr;}
unsigned blend(uint32 crc,int fixed){Dbg_Assert(fixed>=0&&fixed<=255);return SourcePort::OriginalParticleBlend(crc);}
}
namespace Nx {
SourcePort::WeatherParameters CNativeWeather::parameters()const{
    SourcePort::WeatherParameters p;p.rain=m_raining;p.height=m_raining?m_rain_height:m_snow_height;p.frames=m_raining?m_rain_frames:m_snow_frames;p.rate=m_raining?m_rain_drops_per_frame:m_snow_flakes_per_frame;
    p.length=m_rain_length;p.size=m_snow_size;p.top=m_raining?m_rain_top_color:m_snow_color;p.bottom=m_rain_bottom_color;
    p.splash=m_splash_color;p.splash_life=m_splash_life;p.splash_size=m_splash_size;p.splash_rate=m_splash_per_frame;
    p.blend=m_raining?rain_blend_:snow_blend_;p.fixed=m_raining?rain_fixed_:snow_fixed_;p.splash_blend=splash_blend_;p.splash_fixed=splash_fixed_;p.texture=m_raining?splash_:snow_;return p;
}
void CNativeWeather::plat_update_grid(){
    m_system_active=false;
    m_active_drops=0;
    if(!source_render_options()->atmospheric_effects)return;
    Mth::Line ignored;auto* sectors=CEngine::sGetNearestSuperSectorManager(ignored);if(!sectors)return;
    field_.Build(*sectors->GetWorldBBox(),roof);auto* dict=CTexDictManager::sp_particle_tex_dict;Dbg_Assert(dict);
    splash_=dynamic_cast<CNativeTexture*>(dict->GetTexture(0x45c7eb0f));snow_=dynamic_cast<CNativeTexture*>(dict->GetTexture(0xc97c5a4c));m_system_active=field_.Ready();
}
void CNativeWeather::plat_process(float dt){
    Dbg_Assert(std::isfinite(dt)&&dt>=0);if(!m_system_active||dt==0)return;auto* skater=Mdl::Skate::Instance()->GetSkater(0);auto* cam=camera();if(!skater||!cam)return;
    field_.Process(parameters(),skater->m_pos,cam->GetPos(),roof);m_active_drops=field_.ActiveDrops();
}
void CNativeWeather::plat_render(){if(!m_system_active)return;auto* skater=Mdl::Skate::Instance()->GetSkater(0);if(skater)field_.Gather(parameters(),SourcePort::EffectRenderScope::View(),skater->m_pos,SourcePort::EffectRenderScope::Output());}
void CNativeWeather::plat_set_rain_blend_mode(uint32 crc,int fixed){rain_blend_=blend(crc,fixed);rain_fixed_=fixed;}
void CNativeWeather::plat_set_splash_blend_mode(uint32 crc,int fixed){splash_blend_=blend(crc,fixed);splash_fixed_=fixed;}
void CNativeWeather::plat_set_snow_blend_mode(uint32 crc,int fixed){snow_blend_=blend(crc,fixed);snow_fixed_=fixed;}
}
