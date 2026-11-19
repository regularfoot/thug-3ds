#include <source_port/native_lighting.h>
#include <source_port/thread_os.h>
#include <algorithm>
#include <cmath>
namespace {
SourcePort::LightingFrame world;
void color(std::array<float,3>& out,Image::RGBA c,float brightness){out={c.r/128.f*brightness,c.g/128.f*brightness,c.b/128.f*brightness};}
void index(int i){Dbg_Assert(i>=0&&i<Nx::CLightManager::MAX_LIGHTS);}
}
namespace Nx {
CNativeModelLights::CNativeModelLights(){position_.Set(0,0,0,1);ambient_=Image::RGBA(0,0,0,128);for(auto& d:directions_)d.Set(0,1,0,0);for(auto& d:diffuse_)d=Image::RGBA(0,0,0,128);}
bool CNativeModelLights::plat_set_light_ambient_color(const Image::RGBA& c){ambient_=c;return true;}
bool CNativeModelLights::plat_set_light_direction(int i,const Mth::Vector& d){index(i);directions_[i]=d;return true;}
bool CNativeModelLights::plat_set_light_diffuse_color(int i,const Image::RGBA& c){index(i);diffuse_[i]=c;return true;}
const Mth::Vector& CNativeModelLights::plat_get_light_direction(int i)const{index(i);return plat_is_diffuse_light_enabled(i)?directions_[i]:CLightManager::sGetLightDirection(i);}
void CNativeModelLights::plat_enable_ambient_light(bool on){if(on)flags_|=mUSE_MODEL_AMBIENT;else flags_&=~mUSE_MODEL_AMBIENT;plat_update_brightness();}
void CNativeModelLights::plat_enable_diffuse_light(int i,bool on){index(i);if(on)flags_|=mUSE_MODEL_DIFFUSE_0<<i;else flags_&=~(mUSE_MODEL_DIFFUSE_0<<i);plat_update_brightness();}
void CNativeModelLights::plat_update_brightness(){plat_update_engine(position_,scene_light_);}
void CNativeModelLights::plat_update_engine(Mth::Vector& position,bool scene_light){
    Dbg_Assert(source_thread_is_controller());position_=position;scene_light_=scene_light;
    color(frame_.ambient,plat_is_ambient_light_enabled()?ambient_:CLightManager::sGetLightAmbientColor(),m_ambient_brightness);
    for(int i=0;i<2;++i){color(frame_.diffuse[i],plat_is_diffuse_light_enabled(i)?diffuse_[i]:CLightManager::sGetLightDiffuseColor(i),m_diffuse_brightness[i]);frame_.direction[i]=plat_get_light_direction(i);}
    frame_.diffuse[2]={};frame_.direction[2].Set(0,0,0,0);
    if(scene_light){auto pos=position;auto* light=CLightManager::sGetOptimumSceneLight(pos);if(light){
        auto delta=light->GetLightPosition()-position;float distance=delta.Length();float ratio=distance*light->GetLightReciprocalRadius();
        if(distance>0&&ratio<=1){delta/=distance;frame_.direction[2]=delta;auto c=light->GetLightColor();float factor=std::sqrt(std::max(0.f,1-ratio))*light->GetLightIntensity()/255.f;
            frame_.diffuse[2]={c.r*factor,c.g*factor,c.b*factor};}
    }}
}
void CLightManager::s_plat_update_engine(){s_plat_update_colors();for(int i=0;i<MAX_LIGHTS;++i)s_plat_set_light_direction(i);world.diffuse[2]={};}
void CLightManager::s_plat_update_lights(){s_plat_update_engine();}
void CLightManager::s_plat_update_colors(){s_plat_set_light_ambient_color();for(int i=0;i<MAX_LIGHTS;++i)s_plat_set_light_diffuse_color(i);}
bool CLightManager::s_plat_set_light_ambient_color(){color(world.ambient,s_world_lights.m_light_ambient_rgba,s_ambient_brightness);return true;}
Image::RGBA CLightManager::s_plat_get_light_ambient_color(){return s_world_lights.m_light_ambient_rgba;}
bool CLightManager::s_plat_set_light_direction(int i){index(i);world.direction[i]=s_world_lights.m_light_direction[i];return true;}
const Mth::Vector& CLightManager::s_plat_get_light_direction(int i){index(i);return s_world_lights.m_light_direction[i];}
bool CLightManager::s_plat_set_light_diffuse_color(int i){index(i);color(world.diffuse[i],s_world_lights.m_light_diffuse_rgba[i],s_diffuse_brightness[i]);return true;}
Image::RGBA CLightManager::s_plat_get_light_diffuse_color(int i){index(i);return s_world_lights.m_light_diffuse_rgba[i];}
CModelLights* CLightManager::s_plat_create_model_lights(){return new CNativeModelLights;}
bool CLightManager::s_plat_free_model_lights(CModelLights* lights){if(!lights)return false;delete lights;return true;}
}
namespace SourcePort {
LightingFrame NativeLighting(Nx::CModelLights* lights,const Mth::Vector& position,bool scene_light){
    if(lights){auto* native=dynamic_cast<Nx::CNativeModelLights*>(lights);Dbg_Assert(native);auto pos=position;lights->UpdateEngine(pos,scene_light);return native->Frame();}
    Nx::CLightManager::sUpdateEngine();return world;
}
std::array<float,3> LightNormal(const LightingFrame& lights,const Mth::Vector& normal){
    auto result=lights.ambient;float length=normal.Length();if(length>0&&std::isfinite(length))for(unsigned i=0;i<3;++i){float dot=0;for(unsigned c=0;c<3;++c)dot+=normal[c]*lights.direction[i][c]/length;
        dot=std::max(0.f,dot);for(unsigned c=0;c<3;++c)result[c]+=lights.diffuse[i][c]*dot;}
    return result;
}
}
