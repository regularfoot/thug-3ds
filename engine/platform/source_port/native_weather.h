#pragma once
#include <source_port/native_particles.h>
#include <gfx/nxweather.h>
#include <functional>
namespace SourcePort {
using RoofSampler=std::function<float(float,float)>;
struct WeatherParameters {
    bool rain=true;float height=0,length=0,size=0,rate=0;
    int frames=1,splash_life=1;float splash_rate=0,splash_size=0;
    Image::RGBA top,bottom,splash;
    unsigned blend=0,fixed=128,splash_blend=0,splash_fixed=128;
    Nx::CNativeTexture* texture=nullptr;
};
// NGC weather grid/LFSR/drop/snow/splash algorithms. The native storage is
// bounded by the real grid rather than the console's unsafe 45KiB byte pool.
class WeatherField {
public:
    void Build(const Mth::CBBox&,const RoofSampler&);
    void Process(const WeatherParameters&,const Mth::Vector& skater,const Mth::Vector& camera,const RoofSampler&);
    void Gather(const WeatherParameters&,const WorldView&,const Mth::Vector& skater,std::vector<WorldBatch>&)const;
    bool Ready()const{return !roof_.empty();}
    unsigned ActiveDrops()const;
    float Roof(float,float)const;
private:
    std::vector<float> roof_;int width_=0,height_=0;float min_x_=0,min_z_=0;
    std::array<uint8,1024> age_{},x_{},z_{};uint32 sequence_=411;
    float rain_rate_=0,snow_rate_=0,splash_rate_=0;
    struct Splash {Mth::Vector position;int remaining=0;};std::array<Splash,32> splashes_{};unsigned next_splash_=0;
};
}
namespace Nx {
class CNativeWeather final:public CWeather {
    SourcePort::WeatherField field_;
    CNativeTexture* splash_=nullptr;CNativeTexture* snow_=nullptr;
    unsigned rain_blend_=2,splash_blend_=2,snow_blend_=2,rain_fixed_=64,splash_fixed_=64,snow_fixed_=64;
    SourcePort::WeatherParameters parameters()const;
    void plat_update_grid()override;void plat_process(float)override;void plat_render()override;
    void plat_set_rain_blend_mode(uint32,int)override;
    void plat_set_splash_blend_mode(uint32,int)override;
    void plat_set_snow_blend_mode(uint32,int)override;
};
}
