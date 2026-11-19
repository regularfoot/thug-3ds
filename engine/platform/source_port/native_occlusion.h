#pragma once
#include <core/allmath.h>
// The complete original PC occluder selector, five-plane tests and scores.
// Only its D3D vector/optional object translation boundary is adapted.
#include <gfx/dx9/nx/occlude.h>
#include <array>

namespace SourcePort {
class WorldOcclusion {
public:
    void Prepare(const Mth::Vector& camera,const Mth::Vector& right,unsigned viewport,float eye_shift);
    bool HidesSphere(const Mth::Vector& center,float radius,unsigned mask=~0u)const;
    bool ClassifyCell(const Mth::Vector& center,const Mth::Vector& half_size,unsigned& mask)const;
    bool HasVolumes()const{return count_!=0;}
    bool HidesBox(const Mth::Vector& center,const Mth::Vector& half_size)const;
    bool HidesMesh(const Mth::Vector& center,const Mth::Vector& half_size,unsigned triangles,unsigned mask=~0u);
    unsigned HiddenMeshes()const{return meshes_;}
    unsigned HiddenTriangles()const{return triangles_;}
private:
    struct Plane {Mth::Vector normal,absolute;float offset;};
    struct Volume {
        std::array<Plane,10> planes;
        unsigned plane_count=0,source=0;
    };
    std::array<Volume,8> volumes_;
    Mth::Vector camera_{0,0,0,0};
    unsigned count_=0,meshes_=0,triangles_=0;
    bool hidden(const Mth::Vector& center,const Mth::Vector* half_size,float radius,unsigned mask)const;
};
}
