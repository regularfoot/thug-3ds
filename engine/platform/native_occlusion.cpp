#include <source_port/native_occlusion.h>
#include <source_port/thread_os.h>
#include <gfx/nx.h>
#include <cmath>

namespace NxXbox {
const Mth::Vector* NativeSelectedOccluder(unsigned index);
void NativeScoreOccluder(unsigned index);
}

namespace SourcePort {
void WorldOcclusion::Prepare(const Mth::Vector& camera,const Mth::Vector& right,unsigned viewport,float eye_shift){
    count_=meshes_=triangles_=0;
    camera_=camera;
    auto center=camera;
    NxXbox::BuildOccluders(&center,viewport);
    const auto shift=right*eye_shift;
    const Mth::Vector eyes[]={camera-shift,camera+shift};
    for(unsigned i=0;i<volumes_.size();++i){
        const auto* quad=NxXbox::NativeSelectedOccluder(i);
        if(!quad)continue;
        auto face=Mth::CrossProduct(quad[1]-quad[0],quad[3]-quad[0]);
        if(!(face.LengthSqr()>=0.01f))continue;
        face.Normalize();
        if(std::abs(Mth::DotProduct(face,quad[2]-quad[0]))>0.01f)continue;
        const auto middle=(quad[0]+quad[1]+quad[2]+quad[3])*.25f;
        auto& volume=volumes_[count_];
        volume.plane_count=0;volume.source=i;
        auto add=[&](const Mth::Vector& n,const Mth::Vector& p){
            volume.planes[volume.plane_count++]={n,Mth::Vector(std::abs(n[X]),std::abs(n[Y]),std::abs(n[Z]),0),
                Mth::DotProduct(camera-p,n)};
        };
        const float side=Mth::DotProduct(face,eyes[0]-quad[0]);
        const float other_side=Mth::DotProduct(face,eyes[1]-quad[0]);
        if(std::abs(side)<=.05f||std::abs(other_side)<=.05f||std::signbit(side)!=std::signbit(other_side))continue;
        add(side>0?face:-face,quad[0]);
        bool valid=true;
        for(unsigned eye=0;eye<(eye_shift>0?2u:1u)&&valid;++eye){
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
}
// Children cannot lie inside a shadow volume when their enclosing cell lies
// wholly outside any one of its planes. Keep ambiguous and boundary cases.
// The guard is wider for rejection than for accepting an occluded leaf.
bool WorldOcclusion::ClassifyCell(const Mth::Vector& center,const Mth::Vector& half,unsigned& mask)const{
    mask&=(1u<<count_)-1;
    const auto relative=center-camera_;
    for(unsigned bits=mask;bits;bits&=bits-1){
        const auto i=unsigned(__builtin_ctz(bits));
        const auto& volume=volumes_[i];
        bool inside=true;
        for(unsigned j=0;j<volume.plane_count;++j){
            const auto& plane=volume.planes[j];
            const float at=Mth::DotProduct(relative,plane.normal)+plane.offset;
            const float extent=Mth::DotProduct(half,plane.absolute);
            if(at-extent>.25f){mask&=~(1u<<i);inside=false;break;}
            if(!(at+extent<-.125f))inside=false;
        }
        if(inside){NxXbox::NativeScoreOccluder(volume.source);return true;}
    }
    return false;
}
bool WorldOcclusion::hidden(const Mth::Vector& center,const Mth::Vector* half_size,float radius,unsigned mask)const{
    mask&=(1u<<count_)-1;
    if(!mask)return false;
    const auto relative=center-camera_;
    for(;mask;mask&=mask-1){
        const unsigned i=__builtin_ctz(mask);
        const auto& volume=volumes_[i];
        unsigned plane=0;
        for(;plane<volume.plane_count;++plane){
            const auto& p=volume.planes[plane];
            const float extent=half_size?Mth::DotProduct(*half_size,p.absolute):radius;
            // Keep a guard band for bound and projection rounding.
            if(!(Mth::DotProduct(relative,p.normal)+p.offset+extent<-.125f))break;
        }
        if(plane==volume.plane_count){NxXbox::NativeScoreOccluder(volume.source);return true;}
    }
    return false;
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
    if(count==4)NxXbox::AddOcclusionPoly(vertices[0],vertices[1],vertices[2],vertices[3],checksum);
}
void CEngine::s_plat_enable_occlusion_poly(uint32 checksum,bool enable){Dbg_Assert(source_thread_is_controller());NxXbox::EnableOcclusionPoly(checksum,enable);}
void CEngine::s_plat_remove_all_occlusion_polys(){Dbg_Assert(source_thread_is_controller());NxXbox::RemoveAllOcclusionPolys();}
}
