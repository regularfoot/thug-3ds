#include <source_port/material_animation.h>
#include <algorithm>
namespace SourcePort {
namespace {
int64 periodic(int64 value,int64 period){auto n=value%period;return n<0?n+period:n;}
float wrap_uv(float value){float n=std::fmod(value+8.f,16.f);return (n<0?n+16.f:n)-8.f;}
}
bool EvaluateMaterial(const MaterialAsset& material,uint32 timer_ms,uint32 render_start_ms,MaterialFrame& out){
    MaterialFrame frame;frame.passes.resize(material.passes.size());float seconds=float(timer_ms)*0.001f;
    for(std::size_t i=0;i<material.passes.size();++i){const auto& pass=material.passes[i];auto& p=frame.passes[i];p.texture=pass.texture;
        // Same equation and radians as original DX9 sMaterial::figure_wibble_uv.
        if((pass.flags&1)&&!(pass.flags&(1<<14)))for(unsigned axis=0;axis<2;++axis){const auto* w=pass.uv_wibble.data();
            // Source sUVWibbleParams is interleaved U/V: velocity, frequency,
            // amplitude, phase. It is not two consecutive four-float blocks.
            float offset=seconds*w[axis]+w[4+axis]*std::sin(w[2+axis]*seconds+w[6+axis]);if(!std::isfinite(offset))return false;p.uv_offset[axis]=wrap_uv(offset);}
        if(pass.flags&(1<<11)){
            if(pass.animation.empty())return false;auto first=pass.animation.front().time,last=pass.animation.back().time;
            if(!std::is_sorted(pass.animation.begin(),pass.animation.end(),[](const auto& a,const auto& b){return a.time<b.time;}))return false;
            if(first==last){p.texture=pass.animation.front().checksum;continue;}
            int64 period=int64(last)-first,time=int64(first)+periodic(int64(timer_ms)+pass.anim_phase,period);
            // Source period field is intentionally discarded by the original.
            if(pass.anim_iterations>0&&(int64(timer_ms)-first)/period>=pass.anim_iterations)
                time=int64(first)+periodic(period*pass.anim_iterations+pass.anim_phase,period);
            auto next=std::upper_bound(pass.animation.begin(),pass.animation.end(),time,[](int64 t,const auto& key){return t<key.time;});
            if(next==pass.animation.begin())return false;p.texture=(next-1)->checksum;
        }
    }
    for(const auto& sequence:material.color_wibble){
        if(sequence.keys.empty()||!std::is_sorted(sequence.keys.begin(),sequence.keys.end(),[](const auto& a,const auto& b){return a.time<b.time;}))return false;
        std::array<uint8,4> rgba{};const auto* a=&sequence.keys.front();const auto* b=a;float t=0;
        int64 period=int64(sequence.keys.back().time)-a->time;
        if(period){int64 time=int64(a->time)+periodic(int64(render_start_ms)+sequence.phase,period);
            auto next=std::upper_bound(sequence.keys.begin(),sequence.keys.end(),time,[](int64 v,const auto& k){return v<k.time;});
            if(next==sequence.keys.begin()||next==sequence.keys.end())return false;a=&*(next-1);b=&*next;
            if(b->time<=a->time)return false;t=float(time-a->time)/float(b->time-a->time);}
        for(unsigned c=0;c<4;++c){float x=(a->rgba>>(c*8))&255,y=(b->rgba>>(c*8))&255;rgba[c]=uint8(std::clamp(int((1-t)*x+t*y),0,255));}
        frame.vertex_colors.push_back(rgba);
    }out=std::move(frame);return true;
}
}
