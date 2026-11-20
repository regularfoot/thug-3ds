#include <source_port/native_occlusion.h>
#include <source_port/thread_os.h>
#include <source_port/native_world.h>
#include <algorithm>
#include <source_port/memory.h>
#include <gfx/nx.h>
#include <cmath>

namespace NxXbox {
const Mth::Vector* NativeSelectedOccluder(unsigned index);
unsigned NativeOcclusionPolyCount();
const Mth::Vector* NativeUnusedOccluder(unsigned index);
void NativeScoreOccluder(unsigned index);
}

namespace SourcePort {
unsigned WorldOcclusion::Prepare(const Mth::Vector& camera,const Mth::Vector& right,unsigned viewport,float eye_shift,
                             const Mth::Vector* backward){
    count_=meshes_=triangles_=0;
    camera_=camera;
    auto center=camera;
    NxXbox::BuildOccluders(&center,viewport);
    const auto shift=right*eye_shift;
    const Mth::Vector eyes[]={camera-shift,camera+shift};
    if(bool(backward)!=dense_requested_){
        dense_requested_=backward!=nullptr;
        if(dense_requested_){
            NativeHeapScope storage;
            dense_.reset(new(std::nothrow) DenseVolumes);
        }else dense_.reset();
    }
    const unsigned eye_count=eye_shift>0?2u:1u;
    for(unsigned i=0;i<original_slots;++i)
        if(const auto* wall=NxXbox::NativeSelectedOccluder(i))add_volume(wall,i,eyes,eye_count);
    const unsigned original_count=count_;
    if(dense_)add_visible_walls(*backward,eyes,eye_count);
    return count_-original_count;
}

void WorldOcclusion::add_visible_walls(const Mth::Vector& backward,const Mth::Vector (&eyes)[2],unsigned eye_count){
    struct Candidate {const Mth::Vector* quad; float score;};
    std::array<Candidate,extra_slots> best;
    unsigned used=0;
    const unsigned total=NxXbox::NativeOcclusionPolyCount();
    for(unsigned i=0;i<total;++i){
        const auto* q=NxXbox::NativeUnusedOccluder(i);
        if(!q)continue;
        const auto delta=(q[0]+q[1]+q[2]+q[3])*.25f-camera_;
        const float front=-Mth::DotProduct(delta,backward);
        if(front<=0)continue;
        const auto area=Mth::CrossProduct(q[2]-q[0],q[3]-q[1]);
        const float distance2=delta.LengthSqr();
        // Projected area times alignment to the view. Only priority uses
        // this approximation. Both eyes still have to pass the exact test.
        const float score=std::abs(Mth::DotProduct(area,delta))*front/(distance2*distance2);
        if(!(score>0) || (used==best.size() && score<=best.back().score))continue;
        if(used<best.size())++used;
        unsigned at=used-1;
        while(at && score>best[at-1].score){best[at]=best[at-1];--at;}
        best[at]={q,score};
    }
    for(unsigned i=0;i<used;++i)add_volume(best[i].quad,original_slots,eyes,eye_count);
}

void WorldOcclusion::add_volume(const Mth::Vector* quad,unsigned source,const Mth::Vector (&eyes)[2],unsigned eye_count){
    auto face=Mth::CrossProduct(quad[1]-quad[0],quad[3]-quad[0]);
    if(!(face.LengthSqr()>=0.01f))return;
    face.Normalize();
    if(std::abs(Mth::DotProduct(face,quad[2]-quad[0]))>0.01f)return;
    const auto middle=(quad[0]+quad[1]+quad[2]+quad[3])*.25f;
    auto& volume=dense_?(*dense_)[count_]:original_[count_];
    volume.plane_count=0;volume.source=source;
    auto add=[&](const Mth::Vector& n,const Mth::Vector& p){
        volume.planes[volume.plane_count++]={n,Mth::Vector(std::abs(n[X]),std::abs(n[Y]),std::abs(n[Z]),0),
            Mth::DotProduct(camera_-p,n)};
    };
    const float side=Mth::DotProduct(face,eyes[0]-quad[0]);
    const float other_side=Mth::DotProduct(face,eyes[1]-quad[0]);
    if(std::abs(side)<=.05f||std::abs(other_side)<=.05f||std::signbit(side)!=std::signbit(other_side))return;
    add(side>0?face:-face,quad[0]);
    bool valid=true;
    for(unsigned eye=0;eye<eye_count&&valid;++eye){
        for(unsigned edge=0;edge<4;++edge){
            auto n=Mth::CrossProduct(quad[edge]-eyes[eye],quad[(edge+1)%4]-eyes[eye]);
            if(!(n.LengthSqr()>=.01f)){valid=false;break;}
            n.Normalize();
            if(Mth::DotProduct(n,middle-eyes[eye])>0)n=-n;
            // An invalid/concave exporter quad cannot safely cover a whole box.
            for(unsigned corner=0;corner<4;++corner)
                if(Mth::DotProduct(n,quad[corner]-quad[edge])>.01f)valid=false;
            add(n,quad[edge]);
        }
    }
    if(valid)++count_;
}
// Children cannot lie inside a shadow volume when their enclosing cell lies
// wholly outside any one of its planes. Keep ambiguous and boundary cases.
// The guard is wider for rejection than for accepting an occluded leaf.
bool WorldOcclusion::ClassifyCell(const Mth::Vector& center,const Mth::Vector& half,unsigned& mask)const{
    mask&=(1u<<count_)-1;
    const auto* volumes=dense_?dense_->data():original_.data();
    const auto relative=center-camera_;
    for(unsigned bits=mask;bits;bits&=bits-1){
        const auto i=unsigned(__builtin_ctz(bits));
        const auto& volume=volumes[i];
        bool inside=true;
        for(unsigned j=0;j<volume.plane_count;++j){
            const auto& plane=volume.planes[j];
            const float at=Mth::DotProduct(relative,plane.normal)+plane.offset;
            const float extent=Mth::DotProduct(half,plane.absolute);
            if(at-extent>.25f){mask&=~(1u<<i);inside=false;break;}
            if(!(at+extent<-.125f))inside=false;
        }
        if(inside){
            if(volume.source<original_slots)NxXbox::NativeScoreOccluder(volume.source);
            return true;
        }
    }
    return false;
}
bool WorldOcclusion::hidden(const Mth::Vector& center,const Mth::Vector* half_size,float radius,unsigned mask)const{
    mask&=(1u<<count_)-1;
    if(!mask)return false;
    const auto* volumes=dense_?dense_->data():original_.data();
    const auto relative=center-camera_;
    for(;mask;mask&=mask-1){
        const unsigned i=__builtin_ctz(mask);
        const auto& volume=volumes[i];
        unsigned plane=0;
        for(;plane<volume.plane_count;++plane){
            const auto& p=volume.planes[plane];
            const float extent=half_size?Mth::DotProduct(*half_size,p.absolute):radius;
            // Keep a guard band for bound and projection rounding.
            if(!(Mth::DotProduct(relative,p.normal)+p.offset+extent<-.125f))break;
        }
        if(plane==volume.plane_count){
            if(volume.source<original_slots)NxXbox::NativeScoreOccluder(volume.source);
            return true;
        }
    }
    return false;
}
// Only already gathered tiles can cover other draws. The gate, stairs and
// openings keep their own geometry and coverage in each eye.
unsigned WorldOcclusion::CullBehindSubwayTiles(const WorldView& view,std::vector<WorldBatch>& batches){
    count_=0;
    camera_=view.position;
    const auto shift=view.axes[X]*view.stereo_eye_shift;
    const Mth::Vector eyes[]={camera_-shift,camera_+shift};
    const unsigned eye_count=view.stereo_eye_shift>0?2:1;
    if(!(Mth::DotProduct(Mth::CrossProduct(view.axes[X],view.axes[Y]),view.axes[Z])>.99f))return 0;
    struct Cover {float score=0; std::array<Mth::Vector,3> points;};
    std::array<Cover,4> best{};
    for(const auto& b:batches){
        // Limit the proof to the specific original wall materials. Textures
        // on an unrelated/replaced mesh do not establish its coverage.
        if(b.material!=0xf0c4e94a&&b.material!=0xf0c4e936&&b.material!=0xf0c4e90e&&
           b.material!=0x10110010)continue;
        if(!b.retained||!b.opaque||!b.depth_write||b.blend||b.pass||b.sky||b.z_bias||
           !b.ignore_vertex_alpha||b.cutoff>1||b.u_address==2||b.v_address==2||!b.texture)continue;
        const auto tile=b.material==0x10110010?0xfdb0e178u:0xd95c9f00u;
        if(b.texture->Asset().checksum!=tile||b.texture->Revision()!=1||b.texture->IsRenderTarget())continue;
        const auto& vertices=b.VertexData();
        auto position=[&](size_t i){
            const auto& v=vertices[b.indices?(*b.indices)[i]:i];
            return Mth::Vector(v.x,v.y,v.z,1);
        };
        for(size_t i=0;i+2<b.ElementCount();i+=3){
            const auto a=position(i),c=position(i+1),d=position(i+2);
            const auto normal=Mth::CrossProduct(c-a,d-a);
            const auto center=(a+c+d)/3.f;
            const auto delta=center-camera_;
            const float depth=-Mth::DotProduct(delta,view.axes[Z]);
            if(!(depth>0&&depth<900))continue;
            const float eye_side=Mth::DotProduct(normal,eyes[0]-a);
            const float other_side=Mth::DotProduct(normal,eyes[1]-a);
            if(!(eye_side>0&&other_side>0))continue;
            bool clipped=false;
            for(const auto& v:{a,c,d}){
                const float z=Mth::DotProduct(camera_-v,view.axes[Z]);
                if(!(z>view.near_clip+4&&z<view.far_clip-4))clipped=true;
            }
            if(clipped)continue;
            const float score=std::min(eye_side,other_side)/(depth*depth*depth);
            if(!(score>.04f&&score>best.back().score))continue;
            auto at=best.size()-1;
            while(at&&score>best[at-1].score){best[at]=best[at-1];--at;}
            best[at]={score,{a,c,d}};
        }
    }
    for(const auto& item:best){
        if(!item.score)break;
        const auto& v=item.points;
        const auto center=(v[0]+v[1]+v[2])/3.f;
        const auto a=center+(v[0]-center)*.9f,b=center+(v[1]-center)*.9f,c=center+(v[2]-center)*.9f;
        // An inset quad wholly inside the submitted triangle. The existing
        // convex volume test intersects the coverage from both eye positions.
        const Mth::Vector quad[]={a+(c-a)*.1f,a+(b-a)*.1f,b,c};
        add_volume(quad,original_slots,eyes,eye_count);
    }
    if(!count_)return 0;
    const auto old=batches.size();
    batches.erase(std::remove_if(batches.begin(),batches.end(),[&](const auto& b){
        if(!b.retained||b.sky||b.camera_relative||b.z_bias||b.environment||b.pass||
           b.retained->box_half_size[X]<0)return false;
        const auto& mesh=*b.retained;
        const auto padded=mesh.box_half_size+Mth::Vector(3,3,3,0);
        return HidesBox(mesh.box_center,padded);
    }),batches.end());
    return unsigned(old-batches.size());
}
bool WorldOcclusion::HidesSphere(const Mth::Vector& center,float radius,unsigned mask)const{return hidden(center,nullptr,radius,mask);}
bool WorldOcclusion::HidesBox(const Mth::Vector& center,const Mth::Vector& half_size)const{
    return hidden(center,&half_size,0,~0u);
}
bool WorldOcclusion::HidesMesh(const Mth::Vector& center,const Mth::Vector& half_size,unsigned triangles,unsigned mask){
    if(!hidden(center,&half_size,0,mask))return false;
    ++meshes_;triangles_+=triangles;
    return true;
}
}
namespace Nx {
void CEngine::s_plat_add_occlusion_poly(uint32 count,Mth::Vector* vertices,uint32 checksum){
    Dbg_Assert(source_thread_is_controller()&&vertices);
    // Original CEngine accepts only quadrilateral occluders.
    // RU ggo_Occluder51 runs in front of the cannon courtyard façade
    // (Z -508.7 versus -490) and spans its windows and the rooms behind them.
    // It cannot prove visibility even when both eyes are on the same side.
    if(checksum==0xcd1bf265 && count==4 && std::abs(vertices[0][Z]+508.7f)<1.f)return;
    if(count==4)NxXbox::AddOcclusionPoly(vertices[0],vertices[1],vertices[2],vertices[3],checksum);
}
void CEngine::s_plat_enable_occlusion_poly(uint32 checksum,bool enable){Dbg_Assert(source_thread_is_controller());NxXbox::EnableOcclusionPoly(checksum,enable);}
void CEngine::s_plat_remove_all_occlusion_polys(){Dbg_Assert(source_thread_is_controller());NxXbox::RemoveAllOcclusionPolys();}
}
