#include <source_port/native_fog.h>
#include <source_port/render_options.h>
#include <source_port/thread_os.h>
#include <algorithm>
namespace {SourcePort::FogSettings fog;}
namespace SourcePort {
const FogSettings& NativeFog(){return fog;}
void ApplyNativeFog(SourceDrawPacket& packet,bool sky){
    packet.fog_enabled=source_render_options()->atmospheric_effects&&fog.enabled&&!sky;packet.fog_start=fog.start;packet.fog_alpha=fog.color.a;
    packet.fog_color=uint32(fog.color.r)|(uint32(fog.color.g)<<8)|(uint32(fog.color.b)<<16);
}
}
namespace Nx {
void CFog::s_plat_enable_fog(bool enable){Dbg_Assert(source_thread_is_controller());fog.enabled=enable;}
void CFog::s_plat_set_fog_near_distance(float distance){Dbg_Assert(source_thread_is_controller()&&std::isfinite(distance));fog.start=distance;}
void CFog::s_plat_set_fog_exponent(float exponent){Dbg_Assert(source_thread_is_controller()&&std::isfinite(exponent));fog.exponent=exponent;}
void CFog::s_plat_set_fog_rgba(Image::RGBA rgba){Dbg_Assert(source_thread_is_controller());fog.color=rgba;sEnableFog(rgba.a!=0);}
void CFog::s_plat_set_fog_color(){s_plat_set_fog_rgba(sGetFogRGBA());}
void CFog::s_plat_fog_update(){s_plat_set_fog_rgba(sGetFogRGBA());s_plat_set_fog_near_distance(sGetFogNearDistance());}
}
