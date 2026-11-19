// Original CNewParticle/CNewParticleManager remain whole. The native backend
// uses NGC p_nxnewparticle.cpp's active stream/LFSR/three-point trajectory math;
// the PC platform's plat_render is disabled, so it is not a usable backend.
#include <source_port/native_particles.h>
#include <source_port/thread_os.h>
#include <gfx/nxtexman.h>
#include <sys/timer.h>
#include <algorithm>
#include <cmath>
namespace {
SourcePort::EffectRenderScope* effect=nullptr;
uint32 next_random(uint32& seed){seed=(seed&1)?(seed>>1)^0x3500u:seed>>1;return seed;}
void coefficients(const Nx::CParticleParams& params,bool size,Mth::Vector& c0,Mth::Vector& c1,Mth::Vector& c2){
    Mth::Vector x[3];for(unsigned i=0;i<3;++i){x[i]=size?params.m_BoxDims[i]:params.m_BoxPos[i];x[i][3]=size?params.m_RadiusSpread[i]:params.m_Radius[i];}
    float t2=params.m_Lifetime,t1=t2*params.m_MidpointPct*.01f;c0=x[0];
    if(params.m_UseMidpoint&&t1>0&&t1<t2){c1=(t2*t2*(x[1]-x[0])-t1*t1*(x[2]-x[0]))/(t1*t2*(t2-t1));c2=(t1*(x[2]-x[0])-t2*(x[1]-x[0]))/(t1*t2*(t2-t1));}
    else{c1=(x[2]-x[0])/t2;c2.Set(0,0,0,0);}
}
Image::RGBA color_at(const Nx::CParticleParams& params,float age){
    float t=age/params.m_Lifetime;auto a=params.m_Color[0],b=params.m_Color[2];float mid=params.m_ColorMidpointPct*.01f;
    if(params.m_UseMidcolor&&mid>0&&mid<1){if(t>mid){a=params.m_Color[1];t=(t-mid)/(1-mid);}else{b=params.m_Color[1];t/=mid;}}
    t=std::clamp(t,0.f,1.f);auto c=[&](uint8 x,uint8 y){return uint8(std::clamp(int(x+(int(y)-int(x))*t),0,255));};return Image::RGBA(c(a.r,b.r),c(a.g,b.g),c(a.b,b.b),c(a.a,b.a));
}
}
namespace SourcePort {
// Retail VC.qb has 16 fountain objects. Leaves used by race and counter
// goals share this particle backend and must keep their normal rendering.
bool VancouverFountain(uint32 node,uint32 texture){
    switch(node){
        case 0x0f615606: // particles_p3_fish_03
        case 0x7f0ba289: // particles_p3_fish_06
        case 0x080c921f: // particles_p3_fish_07
        case 0xefb4bf18: // particles_p3_fish_09
        case 0x617d57d1: // particles_p3_fish_12
            return texture==0xc067da60; // vc_apm_droplet
        case 0x0b7adae5: // particles_mus_09
        case 0x6cd09719: // particles_mus_14
        case 0xf5d9c6a3: // particles_mus_17
        case 0xab7094ac: // particles_p3_fish_0217
        case 0x1e3952cc: // particles_p3_fish_0220
        case 0x87300376: // particles_p3_fish_0223
        case 0xf75af7f9: // particles_p3_fish_0226
        case 0x10e2dafe: // particles_p3_fish_0228
            return texture==0x1773cdd7; // vc_apm_ripple
        case 0xebaf33fb: // particles_mus_03
        case 0x75cba658: // particles_mus_04
        case 0x9bc5c774: // particles_mus_06
            return texture==0x192834c8; // water_spray
        default:return false;
    }
}
unsigned OriginalParticleBlend(uint32 crc){
    switch(crc){case 0x54628ed7:return 5;case 0x02e58c18:return 1;case 0xa7fd7d23:case 0xdea7e576:return 3;case 0x40f44b8a:return 7;case 0x68e77f40:return 9;
        case 0x18b98905:return 6;case 0xa86285a1:return 2;case 0x0d7a749a:case 0x0eea99ff:return 4;case 0x90b93703:return 8;case 0xb8aa03c9:return 10;
        case 0x515e298e:case 0x806fff30:return 0;default:Dbg_MsgAssert(false,("Unknown original particle blend checksum %08x",crc));return 0;}
}
EffectRenderScope::EffectRenderScope(const WorldView& view,std::vector<WorldBatch>& out):view_(&view),output_(&out),previous_(effect){Dbg_Assert(source_thread_is_controller());effect=this;}
EffectRenderScope::~EffectRenderScope(){Dbg_Assert(source_thread_is_controller()&&effect==this);effect=previous_;}
const WorldView& EffectRenderScope::View(){Dbg_Assert(source_thread_is_controller()&&effect);return *effect->view_;}
std::vector<WorldBatch>& EffectRenderScope::Output(){Dbg_Assert(source_thread_is_controller()&&effect);return *effect->output_;}
}
namespace Nx {
CNativeNewParticle::~CNativeNewParticle(){if(built_)plat_destroy();}
void CNativeNewParticle::plat_build(){
    Dbg_Assert(!built_&&m_params.m_Type==0xdedfc057&&std::isfinite(m_params.m_Lifetime)&&m_params.m_Lifetime>0&&m_params.m_MaxStreams>0&&m_params.m_MaxStreams<=1024);
    Dbg_Assert(std::isfinite(m_params.m_EmitRate)&&m_params.m_EmitRate>=0);built_=true;emitting_=false;hidden_=m_params.m_Hidden||m_params.m_Suspended;
    SourcePort::OriginalParticleBlend(m_params.m_BlendMode);
    if(m_params.m_Texture)Dbg_MsgAssert(dynamic_cast<CNativeTexture*>(CTexDictManager::sp_particle_tex_dict->GetTexture(m_params.m_Texture)),("Missing original particle texture %08x",m_params.m_Texture));
}
void CNativeNewParticle::plat_destroy(){Dbg_Assert(built_);decltype(streams_)().swap(streams_);emitting_=false;built_=false;}
void CNativeNewParticle::plat_hide(bool hidden){Dbg_Assert(built_);hidden_=hidden||m_params.m_Hidden||m_params.m_Suspended;}
void CNativeNewParticle::plat_update(){Advance(Tmr::FrameLength());}
void CNativeNewParticle::plat_render(){Gather(SourcePort::EffectRenderScope::View(),SourcePort::EffectRenderScope::Output());}
void CNativeNewParticle::Advance(float dt){
    Dbg_Assert(built_&&std::isfinite(dt)&&dt>=0&&std::isfinite(m_params.m_EmitRate)&&m_params.m_EmitRate>=0&&m_params.m_Lifetime>0);if(dt==0||hidden_)return;
    if(m_params.m_EmitRate&&(!emitting_||streams_.empty()||m_params.m_EmitRate!=streams_.back().rate)){
        if(streams_.size()<unsigned(m_params.m_MaxStreams)){Stream stream;stream.rate=m_params.m_EmitRate;stream.interval=1/stream.rate;stream.seed=std::rand()&16383;if(!stream.seed)stream.seed=1;streams_.push_back(stream);emitting_=true;}else emitting_=false;
    }else emitting_=m_params.m_EmitRate!=0;
    for(auto& stream:streams_)stream.age+=dt;
    if(emitting_&&!streams_.empty()){double n=double(streams_.back().age)*streams_.back().rate+1;Dbg_MsgAssert(n<65536,("Original particle stream capacity exceeded"));streams_.back().count=int(n);}
    while(!streams_.empty()&&streams_.front().age>m_params.m_Lifetime){auto& stream=streams_.front();
        int dead=int((stream.age-m_params.m_Lifetime)*stream.rate+1);int advance=std::min(dead,stream.count);stream.count-=dead;
        if(stream.count>0||(streams_.size()==1&&emitting_)){stream.age-=dead*stream.interval;for(int i=0;i<advance*4;++i)next_random(stream.seed);break;}
        streams_.pop_front();
    }
}
std::vector<SourcePort::ParticleSample> CNativeNewParticle::Samples()const{
    Dbg_Assert(built_);Mth::Vector s0,s1,s2,p0,p1,p2;coefficients(m_params,true,s0,s1,s2);coefficients(m_params,false,p0,p1,p2);
    // Original random vector components lie in [1,2); subtracting 1.5 of
    // the box polynomial centres the emitter/spread without invented paths.
    p0-=1.5f*s0;p1-=1.5f*s1;p2-=1.5f*s2;
    std::vector<SourcePort::ParticleSample> result;
    for(const auto& stream:streams_){uint32 seed=stream.seed;float age=stream.age;
        for(int i=0;i<stream.count;++i,age-=stream.interval){Mth::Vector random;for(unsigned c=0;c<4;++c)random[c]=1+next_random(seed)/16384.f;
            if(age<0||age>m_params.m_Lifetime)continue;
            auto spread=s0+age*s1+age*age*s2;spread.Scale(random);auto pos=p0+age*p1+age*age*p2+spread;
            float radius=pos[3];pos[3]=1;result.push_back({pos,radius,color_at(m_params,age)});
        }
    }return result;
}
void CNativeNewParticle::Gather(const SourcePort::WorldView& view,std::vector<SourcePort::WorldBatch>& out)const{
    if(!built_||hidden_||m_params.m_Hidden||m_params.m_Suspended)return;
    if(m_params.m_SuspendDistance>0&&(m_bsphere-view.position).Length()>m_params.m_SuspendDistance)return;
    if(!m_params.m_LocalCoord&&!SourcePort::SphereVisible(view,m_bsphere))return;
    const bool fountain=(view.hide_vancouver_ambient||view.ambient_counts)&&
        SourcePort::VancouverFountain(m_params.m_Name,m_params.m_Texture);
    if(fountain){
        if(view.ambient_counts)++view.ambient_counts->fountain_visible;
        if(view.hide_vancouver_ambient)return;
    }
    SourcePort::WorldBatch batch;batch.blend=SourcePort::OriginalParticleBlend(m_params.m_BlendMode);batch.fixed_alpha=m_params.m_FixedAlpha;batch.cutoff=m_params.m_AlphaCutoff;
    batch.depth_write=false;batch.draw_order=1500;batch.distance=(m_bsphere-view.position).LengthSqr();
    if(m_params.m_Texture){batch.texture=dynamic_cast<CNativeTexture*>(CTexDictManager::sp_particle_tex_dict->GetTexture(m_params.m_Texture));Dbg_Assert(batch.texture);}
    for(const auto& sample:Samples()){if(sample.radius<=0)continue;auto right=view.axes[0]*sample.radius,up=view.axes[1]*sample.radius;
        Mth::Vector p[4]={sample.position-right-up,sample.position-right+up,sample.position+right+up,sample.position+right-up};const float u[4]={0,0,1,1},v[4]={1,0,0,1};
        for(unsigned k:{0u,1u,2u,0u,2u,3u}){auto c=sample.color;batch.vertices.push_back({p[k][0],p[k][1],p[k][2],c.r,c.g,c.b,c.a,u[k],v[k]});}
    }
    if(view.ambient_counts){
        auto& count=fountain?view.ambient_counts->fountain_elements:view.ambient_counts->other_particle_elements;
        count+=unsigned(batch.vertices.size());
    }
    if(!batch.vertices.empty())out.push_back(std::move(batch));
}
CNewParticle* CNativeNewParticleManager::plat_create_particle(){return new CNativeNewParticle;}
}
