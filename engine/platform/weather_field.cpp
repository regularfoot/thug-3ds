#include <source_port/native_weather.h>
#include <source_port/legacy_particle_draw.h>
#include <algorithm>
#include <cmath>
namespace {
constexpr float cell=128,missing=50000;constexpr int side=16,layers=4;
Mth::Vector camera_right(const SourcePort::WorldView& view){auto right=Mth::CrossProduct(view.axes[Z],Mth::Vector(0,1,0,0));if(right.LengthSqr()<1e-12f)right=-view.axes[X];return right.Normalize();}
void quad(SourcePort::WorldBatch& batch,const Mth::Vector& position,const Mth::Vector& right,const Mth::Vector& up,Image::RGBA color){
    Mth::Vector p[]={position-right+up,position+right+up,position+right-up,position-right-up};float u[]={0,1,1,0},v[]={0,0,1,1};
    for(unsigned i:{0u,1u,2u,0u,2u,3u})batch.vertices.push_back({p[i][X],p[i][Y],p[i][Z],color.r,color.g,color.b,color.a,u[i],v[i]});
}
}
namespace SourcePort {
void WeatherField::Build(const Mth::CBBox& box,const RoofSampler& sample){
    for(unsigned axis:{0u,2u})Dbg_Assert(std::isfinite(box.GetMin()[axis])&&std::isfinite(box.GetMax()[axis])&&box.GetMin()[axis]<=box.GetMax()[axis]&&std::abs(box.GetMin()[axis])<1e8f&&std::abs(box.GetMax()[axis])<1e8f);
    int x0=int(box.GetMin()[X])/int(cell)-1,x1=int(box.GetMax()[X])/int(cell)+1;
    int z0=int(box.GetMin()[Z])/int(cell)-1,z1=int(box.GetMax()[Z])/int(cell)+1;
    if(x1-x0>350){int excess=(x1-x0-350)/2;x0+=excess;x1-=excess;}
    if(z1-z0>300){int excess=(z1-z0-300)/2;z0+=excess;z1-=excess;}
    width_=x1-x0+1;height_=z1-z0+1;Dbg_Assert(width_>0&&width_<=352&&height_>0&&height_<=302);
    min_x_=x0*cell;min_z_=z0*cell;std::vector<float> corners(std::size_t(width_+1)*(height_+1)),levels;
    for(int z=0;z<=height_;++z)for(int x=0;x<=width_;++x){float y=sample(min_x_+x*cell,min_z_+z*cell);Dbg_Assert(std::isfinite(y));
        // Original console merges roof heights within 30 inches. Dynamic
        // indices retain that rule without overflowing the 256-height palette.
        auto at=std::find_if(levels.begin(),levels.end(),[&](float a){return std::abs(a-y)<30;});if(at!=levels.end())y=*at;else levels.push_back(y);
        corners[std::size_t(z)*(width_+1)+x]=y;
    }
    roof_.resize(std::size_t(width_)*height_);for(int z=0;z<height_;++z)for(int x=0;x<width_;++x){std::size_t i=std::size_t(z)*(width_+1)+x;
        roof_[std::size_t(z)*width_+x]=std::max({corners[i],corners[i+1],corners[i+width_+1],corners[i+width_+2]});}
    age_.fill(255);for(unsigned i=0;i<age_.size();++i){x_[i]=Mth::Rnd(255);z_[i]=Mth::Rnd(255);}sequence_=411;
    rain_rate_=snow_rate_=splash_rate_=0;next_splash_=0;for(auto& splash:splashes_)splash.remaining=0;
}
float WeatherField::Roof(float x,float z)const{
    int ix=int(std::floor((x-min_x_)/cell)),iz=int(std::floor((z-min_z_)/cell));
    if(ix<0||iz<0||ix>=width_||iz>=height_)return missing;return roof_[std::size_t(iz)*width_+ix];
}
unsigned WeatherField::ActiveDrops()const{return std::count_if(age_.begin(),age_.end(),[](uint8 a){return a!=255;});}
void WeatherField::Process(const WeatherParameters& p,const Mth::Vector& skater,const Mth::Vector& camera,const RoofSampler& sample){
    if(!Ready())return;Dbg_Assert(p.frames>0&&p.frames<=254&&std::isfinite(p.rate)&&p.rate>=0&&std::isfinite(p.splash_rate)&&p.splash_rate>=0&&p.splash_rate<1000000);
    // Original frame-based sequence: age previous particles once, then create
    // this logical frame's drops. GPU stereo replay never calls Process again.
    for(auto& age:age_)if(age<255)++age;for(auto& splash:splashes_)if(splash.remaining>0)--splash.remaining;
    float& rate=p.rain?rain_rate_:snow_rate_;float next=rate+std::min(p.rate,float(1024/p.frames));int count=int(next)-int(rate);rate=next-std::floor(next);
    for(int i=0;i<count;++i){if(age_[sequence_]!=255)break;age_[sequence_]=254-p.frames;x_[sequence_]=Mth::Rnd(255);z_[sequence_]=Mth::Rnd(255);
        if(sequence_==411)sequence_=0;else{if(sequence_==0)sequence_=411;sequence_=(sequence_&1)?(sequence_>>1)^0x0240:(sequence_>>1);}}
    if(!p.rain)return;float next_splash=splash_rate_+p.splash_rate;int count_splash=int(next_splash)-int(splash_rate_);splash_rate_=next_splash-std::floor(next_splash);
    // Source creates one splash when the accumulated rate crosses an integer.
    if(count_splash&&p.splash_life>0){auto direction=skater-camera;direction[Y]=direction[W]=0;direction.Normalize();
        float r1=Mth::Rnd(32768)/32768.f,r2=1-Mth::Rnd(32768)/32768.f;auto position=camera+direction*(cell*16*r1);Mth::Vector lateral(direction[Z],0,-direction[X],0);
        position+=lateral*(cell*16*r1*r2*((next_splash_&1)?1.f:-1.f));position[Y]=sample(position[X],position[Z])+p.splash_size;position[W]=1;
        splashes_[next_splash_]={position,p.splash_life};next_splash_=(next_splash_+1)%32;}
}
void WeatherField::Gather(const WeatherParameters& p,const WorldView& view,const Mth::Vector& skater,std::vector<WorldBatch>& out)const{
    if(!Ready())return;Dbg_Assert(p.frames>0&&p.frames<=254);
    auto first_effect=out.size();
    WorldBatch batch;batch.texture=p.rain?nullptr:p.texture;batch.blend=p.blend;batch.fixed_alpha=p.fixed;batch.depth_write=false;batch.u_address=batch.v_address=1;
    auto right=camera_right(view),up=Mth::CrossProduct(right,view.axes[Z]).Normalize();
    int cx=int((skater[X]-min_x_)/cell),cz=int((skater[Z]-min_z_)/cell);
    for(int z=std::max(0,cz-16);z<=std::min(height_-1,cz+16);++z)for(int x=std::max(0,cx-16);x<=std::min(width_-1,cx+16);++x){
        float roof=roof_[std::size_t(z)*width_+x];if(roof==missing)continue;
        unsigned id=(z&15)*16+(x&15);
        for(unsigned layer=0;layer<layers;++layer,id+=side*side)if(age_[id]!=255){
            float fraction=float(int(age_[id])-254+p.frames)/p.frames;
            float px=min_x_+x*cell+x_[id]*cell/(p.rain?255.f:510.f),pz=min_z_+z*cell+z_[id]*cell/(p.rain?255.f:510.f);
            float py=roof+p.height*(1-fraction);
            if(p.rain){for(unsigned end=0;end<2;++end){auto color=end?p.top:p.bottom;batch.vertices.push_back({px,py+(end?p.length:0),pz,color.r,color.g,color.b,color.a,0,0});}}
            else{Dbg_MsgAssert(p.texture,("Original snow texture is missing"));px+=std::sin(2*Mth::PI*((z_[id]+age_[id])&255)/256.f)*64;pz+=std::sin(2*Mth::PI*((x_[id]+age_[id])&255)/256.f)*64;
                quad(batch,Mth::Vector(px,py+p.size,pz,1),right*p.size,up*p.size,p.top);}
        }
    }
    if(p.rain&&!batch.vertices.empty()){
        // Real native PICA line conversion is shared with the whole original
        // legacy line particles, including endpoint colors and pixel width.
        EffectRenderScope effects(view,out);ParticleDrawScope draw(nullptr,batch.blend,batch.fixed_alpha);ParticleEmit::Textured(false);
        ParticleEmit::Begin(ParticlePrimitive::Lines,batch.vertices.size());for(const auto& v:batch.vertices){ParticleEmit::Position3f32(v.x,v.y,v.z);ParticleEmit::Color(Image::RGBA(v.r,v.g,v.b,v.a));}ParticleEmit::End();
    }else if(!batch.vertices.empty())out.push_back(std::move(batch));
    if(p.rain){WorldBatch splashes;splashes.texture=p.texture;splashes.blend=p.splash_blend;splashes.fixed_alpha=p.splash_fixed;splashes.depth_write=false;splashes.u_address=splashes.v_address=1;
        for(const auto& splash:splashes_)if(splash.remaining>0){Dbg_MsgAssert(p.texture,("Original splash texture is missing"));Dbg_Assert(p.splash_life>0);auto c=p.splash;
            float fraction=float(splash.remaining)/p.splash_life;c.r=uint8(c.r*fraction);c.g=uint8(c.g*fraction);c.b=uint8(c.b*fraction);c.a=uint8(c.a*fraction);quad(splashes,splash.position,right*p.splash_size,up*p.splash_size,c);}
        if(!splashes.vertices.empty())out.insert(out.begin()+first_effect,std::move(splashes));}
}
}
