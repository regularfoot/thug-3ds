#include <source_port/legacy_particle_draw.h>
#include <source_port/thread_os.h>
#include <algorithm>
#include <cmath>

namespace {
SourcePort::ParticleDrawScope* draw;
void controller() { Dbg_Assert(source_thread_is_controller()); }
}
namespace SourcePort {
ParticleDrawScope::ParticleDrawScope(Nx::CNativeTexture* texture, unsigned blend, unsigned fixed) {
    controller(); Dbg_Assert(!draw && blend <= 17 && fixed <= 255);
    batch_.texture = texture; batch_.blend = blend; batch_.fixed_alpha = fixed;
    batch_.depth_write = false; batch_.cutoff = 1;
    batch_.u_address = batch_.v_address = 1;
    draw = this;
}
ParticleDrawScope::~ParticleDrawScope() {
    controller(); Dbg_Assert(draw == this && !begun_ && !pending_);
    if (!batch_.vertices.empty()) EffectRenderScope::Output().push_back(std::move(batch_));
    draw = nullptr;
}
void ParticleDrawScope::finish_vertex() {
    if (!pending_) return;
    Dbg_Assert(color_ && (uv_ == textured_) && vertices_.size() <= expected_);
    pending_ = false;
}
void ParticleEmit::Textured(bool enabled) {
    controller(); Dbg_Assert(draw && !draw->begun_);
    Dbg_Assert(!enabled || draw->batch_.texture);
    draw->textured_ = enabled;
    if (!enabled) draw->batch_.texture = nullptr;
}
void ParticleEmit::Begin(ParticlePrimitive primitive, int count) {
    controller(); Dbg_Assert(draw && !draw->begun_ && count >= 0 && count <= 4 * 1024 * 1024);
    if (primitive == ParticlePrimitive::Quads) Dbg_Assert(count % 4 == 0);
    if (primitive == ParticlePrimitive::Lines) Dbg_Assert(count % 2 == 0);
    if (primitive == ParticlePrimitive::Strip || primitive == ParticlePrimitive::Fan) Dbg_Assert(count >= 3);
    draw->primitive_ = primitive; draw->expected_ = count;
    draw->vertices_.clear(); draw->vertices_.reserve(count); draw->begun_ = true;
}
void ParticleEmit::Position3f32(float x, float y, float z) {
    controller(); Dbg_Assert(draw && draw->begun_);
    draw->finish_vertex(); Dbg_Assert(draw->vertices_.size() < draw->expected_);
    Dbg_Assert(std::isfinite(x) && std::isfinite(y) && std::isfinite(z));
    draw->vertices_.push_back({x,y,z,0,0,0,0,0,0});
    draw->pending_ = true; draw->color_ = draw->uv_ = false;
}
void ParticleEmit::Color(Image::RGBA color) {
    controller(); Dbg_Assert(draw && draw->pending_ && !draw->color_);
    auto& v = draw->vertices_.back(); v.r=color.r; v.g=color.g; v.b=color.b; v.a=color.a;
    draw->color_ = true;
}
void ParticleEmit::TexCoord2f32(float u, float v) {
    controller(); Dbg_Assert(draw && draw->pending_ && draw->textured_ && !draw->uv_);
    Dbg_Assert(std::isfinite(u) && std::isfinite(v));
    draw->vertices_.back().u=u; draw->vertices_.back().v=v; draw->uv_=true;
}
void ParticleEmit::End() {
    controller(); Dbg_Assert(draw && draw->begun_); draw->finish_vertex();
    Dbg_Assert(draw->vertices_.size() == draw->expected_);
    auto& input=draw->vertices_; auto& output=draw->batch_.vertices;
    auto triangle=[&](std::size_t a,std::size_t b,std::size_t c) {
        output.push_back(input[a]); output.push_back(input[b]); output.push_back(input[c]);
    };
    switch(draw->primitive_) {
    case ParticlePrimitive::Quads:
        for(std::size_t i=0;i<input.size();i+=4) { triangle(i,i+1,i+2); triangle(i,i+2,i+3); }
        break;
    case ParticlePrimitive::Strip:
        for(std::size_t i=2;i<input.size();++i) {
            if(i&1) triangle(i-1,i-2,i); else triangle(i-2,i-1,i);
        }
        break;
    case ParticlePrimitive::Fan:
        for(std::size_t i=2;i<input.size();++i) triangle(0,i-1,i);
        break;
    case ParticlePrimitive::Lines: {
        // PICA has no native line primitive. Expand the actual source endpoints
        // into one-pixel quads; per-end depth preserves constant screen width.
        const auto& view=EffectRenderScope::View();
        float tangent=std::tan(view.hfov*float(Mth::PI)/360);
        for(std::size_t i=0;i<input.size();i+=2) {
            Mth::Vector p[2]; float depth[2],sx[2],sy[2];
            for(unsigned e=0;e<2;++e) {
                const auto& v=input[i+e]; p[e].Set(v.x,v.y,v.z,1); auto d=p[e]-view.position;
                depth[e]=-Mth::DotProduct(d,view.axes[Z]);
                float safe=std::max(std::abs(depth[e]),view.near_clip);
                sx[e]=Mth::DotProduct(d,view.axes[X])/safe;
                sy[e]=Mth::DotProduct(d,view.axes[Y])/safe;
            }
            float dx=(sx[1]-sx[0])*view.width,dy=(sy[1]-sy[0])*view.height*view.aspect;
            float length=std::hypot(dx,dy); float nx=length? -dy/length:1,ny=length?dx/length:0;
            SourceRenderVertex corners[4];
            for(unsigned e=0;e<2;++e) {
                float d=std::max(std::abs(depth[e]),view.near_clip);
                auto offset=view.axes[X]*(nx*tangent*d/view.width)+view.axes[Y]*(ny*tangent*d/(view.aspect*view.height));
                for(unsigned side=0;side<2;++side) {
                    auto point=p[e]+offset*(side? -1.f:1.f); auto& v=corners[e*2+side]; v=input[i+e]; v.x=point[X];v.y=point[Y];v.z=point[Z];
                }
            }
            for(unsigned index:{0u,1u,2u,2u,1u,3u}) output.push_back(corners[index]);
        }
        break;
    }
    }
    draw->begun_=false;
}
}
