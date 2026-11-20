#pragma once
#include <source_port/render_assets.h>
#include <source_port/graphics_os.h>
#include <gfx/nxmesh.h>
namespace SourcePort {
struct CASRecord { uint32 mask=0;uint16 mesh=0;std::array<uint16,3> indices{}; };
struct CASAsset {uint32 removal_mask=0;std::vector<CASRecord> records;};
bool ReadCAS(std::shared_ptr<const ResourceBytes>,CASAsset&,ResourceError* error=nullptr);
// CEngine's legacy in-memory mesh API omits the CAS byte length. The actual
// CFileLibrary producer supplies it for the duration of its synchronous load;
// no changes to SCutsceneModelDataInfo's layout and no guessed readable span.
class EmbeddedCASResource {
public:
    EmbeddedCASResource(const void*,std::size_t);~EmbeddedCASResource();
    EmbeddedCASResource(const EmbeddedCASResource&)=delete;EmbeddedCASResource& operator=(const EmbeddedCASResource&)=delete;
private:
    const void* pointer_;std::size_t size_;EmbeddedCASResource* previous_;
    friend bool ReadEmbeddedCAS(const void*,CASAsset&);
};
bool ReadEmbeddedCAS(const void*,CASAsset&);
// Preserve triangle-strip winding and exporter degenerates. Removal masks
// refer to source mesh load order and source strip indices, before batching.
bool MeshTriangles(const SceneAsset&,std::size_t sector,std::size_t mesh,unsigned lod,
    const CASAsset*,uint32 hide_mask,std::vector<uint16>& triangles);
bool PoseSector(const SectorAsset&,const Mth::Matrix* bones,int num_bones,
    std::vector<Mth::Vector>& positions,std::vector<Mth::Vector>* normals=nullptr,const MeshAsset* mesh=nullptr);
// Immutable derived topology: source indices are retained for attributes and
// CAS, compact triangle indices address only vertices actually drawn.
struct RenderTopology {
    std::vector<uint16> vertices,triangles;
    unsigned weights=1;
};
bool BuildRenderTopology(const SceneAsset&,std::size_t sector,std::size_t mesh,unsigned lod,
    const CASAsset*,uint32 hide_mask,RenderTopology&);
std::vector<uint16> CompactSourceStrip(const std::vector<uint16>& strip, const std::vector<uint16>& vertices,
                                      const std::vector<uint16>& triangles);
void SimplifyGameplaySkin(const SectorAsset&,RenderTopology&);
bool BuildDistantRenderTopology(const SectorAsset&,const MeshAsset&,RenderTopology&);
bool PoseRenderTopology(const SectorAsset&,const RenderTopology&,const Mth::Matrix* bones,int num_bones,
    std::vector<Mth::Vector>& positions,std::vector<Mth::Vector>* normals=nullptr);
struct DrawVertexAttributes { uint8 r,g,b,a; float u,v; };
// Reuse only world positions with identical source skin inputs within one
// sector gather. UV, colour and the order of each material's vertices stay local.
class SharedSkinPositions {
public:
    explicit SharedSkinPositions(const SectorAsset&);
    void Begin();
    bool Active() const {return !slots_.empty();}
    unsigned hits=0;
    std::array<float,3>* Lookup(unsigned index,unsigned weights,bool& ready);
private:
    struct Slot { std::array<float,3> point{}; unsigned generation=0,weights=0; };
    std::vector<uint16> mapping_;
    std::vector<Slot> slots_;
    unsigned generation_=0;
};
bool PoseSkinnedDraw(const SectorAsset&,const RenderTopology&,const std::vector<Mth::Matrix>& bones,
    const Mth::Matrix& world,const std::vector<DrawVertexAttributes>&,std::vector<SourceRenderVertex>&,SharedSkinPositions* reuse=nullptr);
// Visual samples for a small, distant pedestrian. Bone animation and world transforms stay live.
class DistantPedPose {
public:
    static std::unique_ptr<DistantPedPose> Create(std::size_t vertices);
    ~DistantPedPose();
    DistantPedPose(const DistantPedPose&)=delete;
    bool Draw(const SectorAsset&,const RenderTopology&,const std::vector<Mth::Matrix>& bones,
        const Mth::Matrix& world,const std::vector<DrawVertexAttributes>&,uint32 now_ms,
        std::vector<SourceRenderVertex>& out,bool& sampled);
private:
    using Point=std::array<float,3>;
    explicit DistantPedPose(std::size_t n):capacity_(n){}
    std::vector<Point> current_,previous_;
    std::size_t capacity_,bone_count_=0;
    uint32 sampled_at_=0,last_draw_=0,span_=0;
};
struct SkinTopology {
    std::vector<SourceSkinInfluence> influences;
    std::vector<uint16> bones;
};
bool BuildSkinTopology(const SectorAsset&,const RenderTopology&,unsigned bone_count,SkinTopology&);
}
namespace Nx {
class CNativeMesh final:public CMesh {
public:
    CNativeMesh();
    bool Assign(std::shared_ptr<SourcePort::SceneAsset>,SourcePort::CASAsset,CTexDict*,bool shadow_volume);
    const std::shared_ptr<SourcePort::SceneAsset>& Asset()const{return asset_;}
    const SourcePort::CASAsset& CAS()const{return cas_;}
    bool ShadowVolumeRequested()const{return shadow_volume_;}
private:
    std::shared_ptr<SourcePort::SceneAsset> asset_;
    SourcePort::CASAsset cas_;bool shadow_volume_=false;
};
}
