#pragma once
#include <core/allmath.h>
#include <array>
#include <gfx/nxlight.h>
namespace SourcePort {
struct LightingFrame {
    std::array<float,3> ambient{};
    std::array<Mth::Vector,3> direction{};
    std::array<std::array<float,3>,3> diffuse{};
};
LightingFrame NativeLighting(Nx::CModelLights*,const Mth::Vector&,bool scene_light);
std::array<float,3> LightNormal(const LightingFrame&,const Mth::Vector&);
}
namespace Nx {
class CNativeModelLights final:public CModelLights {
public:CNativeModelLights();
    const SourcePort::LightingFrame& Frame()const{return frame_;}
private:
    unsigned flags_=0;Image::RGBA ambient_;std::array<Image::RGBA,2> diffuse_{};std::array<Mth::Vector,2> directions_{};
    SourcePort::LightingFrame frame_;Mth::Vector position_;bool scene_light_=false;
    bool plat_set_light_ambient_color(const Image::RGBA&)override;
    bool plat_set_light_direction(int,const Mth::Vector&)override;
    bool plat_set_light_diffuse_color(int,const Image::RGBA&)override;
    Image::RGBA plat_get_light_ambient_color()const override{return ambient_;}
    const Mth::Vector& plat_get_light_direction(int)const override;
    Image::RGBA plat_get_light_diffuse_color(int i)const override{return diffuse_.at(i);}
    void plat_update_engine(Mth::Vector&,bool)override;void plat_update_brightness()override;
    void plat_enable_ambient_light(bool)override;void plat_enable_diffuse_light(int,bool)override;
    bool plat_is_ambient_light_enabled()const override{return (flags_&mUSE_MODEL_AMBIENT)!=0;}
    bool plat_is_diffuse_light_enabled(int i)const override{return (flags_&(mUSE_MODEL_DIFFUSE_0<<i))!=0;}
};
}
