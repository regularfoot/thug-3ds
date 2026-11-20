#pragma once
#include <core/allmath.h>
// The complete original PC occluder selector, five-plane tests and scores.
// Only its D3D vector/optional object translation boundary is adapted.
#include <gfx/dx9/nx/occlude.h>
#include <array>
#include <memory>
#include <vector>

namespace SourcePort {
struct WorldView;
struct WorldBatch;
class WorldOcclusion {
public:
    // Returns the number of additional valid stereo occluders.
    unsigned Prepare(const Mth::Vector& camera,const Mth::Vector& right,unsigned viewport,float eye_shift,
                 const Mth::Vector* backward=nullptr);
    unsigned CullBehindSubwayTiles(const WorldView&,std::vector<WorldBatch>&);
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
    static constexpr unsigned original_slots=8, extra_slots=6;
    static_assert(original_slots+extra_slots<32);
    void add_volume(const Mth::Vector* quad,unsigned source,const Mth::Vector (&eyes)[2],unsigned eye_count);
    void add_visible_walls(const Mth::Vector& backward,const Mth::Vector (&eyes)[2],unsigned eye_count);
    std::array<Volume,original_slots> original_;
    using DenseVolumes=std::array<Volume,original_slots+extra_slots>;
    std::unique_ptr<DenseVolumes> dense_;
    bool dense_requested_=false;
    Mth::Vector camera_{0,0,0,0};
    unsigned count_=0,meshes_=0,triangles_=0;
    bool hidden(const Mth::Vector& center,const Mth::Vector* half_size,float radius,unsigned mask)const;
};
}
