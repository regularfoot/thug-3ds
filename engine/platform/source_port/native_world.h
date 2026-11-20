#pragma once
#include <source_port/mesh_assets.h>
#include <source_port/thread_os.h>
#include <source_port/native_texture.h>
#include <source_port/native_lighting.h>
#include <gfx/nxgeom.h>
#include <gfx/nxscene.h>
#include <gfx/nxsector.h>
#include <gfx/nxmodel.h>
#include <map>
#include <deque>
namespace SourcePort {
class WorldOcclusion;
class WorldMeshRetention;
class WorldTextureLookup;
struct WorldDetailStats {unsigned worker_items=0;unsigned shared_positions=0,ped_visible=0,ped_far=0,ped_samples=0,ped_replays=0,distant_prop_sectors=0,texture_hits=0,texture_misses=0,local_parts=0,local_triangles=0,local_array_parts=0,meshes=0,triangles=0,lod_meshes=0,lod_triangles_saved=0,partition_skipped=0,material_meshes=0,material_groups=0,range_meshes=0,range_triangles=0,backface_meshes=0,backface_triangles=0,occluded_sectors=0,skin_draws=0,retained_hits=0,retained_builds=0;uint64 pose_ticks=0,gather_ticks=0,sort_ticks=0,coalesce_ticks=0,group_ticks=0;};
struct AmbientEffectCounts {
    unsigned fountain_visible=0,fountain_elements=0,weather_elements=0,legacy_elements=0,other_particle_elements=0;
};
struct WorldView {
    SourceFrameWorker* sort_worker=nullptr;
    bool wide_static_groups=false;
    bool refine_static_leaves=false, keep_turn_buffers=false;
    std::vector<uint64>* upload_trace=nullptr;
    bool prepare_source_strips=false, use_source_strips=false;
    bool prune_occluders=false, city_stereo_fast=false;
    unsigned occluder_mask=~0u;
    bool defer_cold_runs=false, group_opaque_disjoint=false, reuse_pose_vertices=false;
    bool hide_vancouver_ambient=false,reuse_static_prefix=false,vancouver_early=false,distant_ped_poses=false;
    AmbientEffectCounts* ambient_counts=nullptr;
    Mth::Matrix axes;Mth::Vector position;
    float hfov=90,aspect=4.f/3,near_clip=1,far_clip=12000;
    unsigned viewport=0,x=0,y=0,width=400,height=240;
    bool performance=false,retain_static=true,cull_static_meshes=true,index_skinned=true;
    bool partition_static=true,inherit_partition_planes=true,group_static_materials=true,direct_static_positions=true;
    bool direct_partition_gather=false;
    WorldOcclusion* occlusion=nullptr;
    WorldMeshRetention* mesh_retention=nullptr;
    WorldTextureLookup* texture_lookup=nullptr;
    const Nx::CModel* local_model=nullptr; // original local skater only, frame lifetime
    bool local_arrays=false; // independent submission path for Moscow
    bool profile_pose=false;
    bool gpu_skin=false;
    bool test_player_skin=false;
    float static_detail_pixels=0,static_prop_radius=0,city_prop_radius=0;
    bool small_props_only=false,reclaim_cold_topology=false;
    float static_lod_pixels=0,static_lod_depth_per_unit=0,static_lod_min_distance=2000.f;
    float static_detail_horizontal=0,static_detail_vertical=0;
    float stereo_eye_shift=0;
    bool backface_static=false,occlude_static_cells=false,frustum_positive_winding=false;
    WorldDetailStats* detail_stats=nullptr;
    // Prepared per view, never global: viewports/FOV changes cannot share stale planes.
    float frustum_hfov=0,frustum_aspect=0,frustum_th=0,frustum_tv=0,frustum_rx=0,frustum_ry=0;
    float frustum_eye_pad=0;
    struct FrustumPlane {
        Mth::Vector normal{0,0,0,0},absolute{0,0,0,0};
    };
    std::array<FrustumPlane,6> frustum_planes;
};
struct WorldDrawState {
    Nx::CNativeTexture* texture=nullptr;
    uint32 material=0,blend=0;unsigned fixed_alpha=128,cutoff=0,u_address=0,v_address=0,filter=0;
    bool mesh_uv=false,environment=false,camera_relative=false,cull=false,depth_write=true,sky=false,ignore_vertex_alpha=false,opaque=true,no_skater_shadow=false,fog_disabled=false;
    float draw_order=0,distance=0,z_bias=0;unsigned pass=0;std::size_t group=0;
};
struct StaticWorldMesh:WorldDrawState {
    std::vector<SourceRenderVertex> vertices;
    std::shared_ptr<const std::vector<uint16>> indices;
    std::vector<uint16> source_strip;
    std::weak_ptr<const uint8> owner;
    uint64 serial=0;
    // Only immutable, frustum-tested geometry can be moved across draws.
    Mth::Vector box_center{0,0,0,0};
    Mth::Vector box_half_size{-1,-1,-1,0};
};
// CPU copies survive a change in the GPU's combined material runs. GPU
// buffers, index ranges and draw order remain owned by NativeWorldBatch.
// FIFO retention is bounded independently of both GPU and original heaps.
class WorldMeshRetention {
public:
    explicit WorldMeshRetention(std::size_t budget):budget_(budget){}
    bool Remember(std::shared_ptr<const StaticWorldMesh>);
    void Clear(){entries_.clear();bytes_=0;}
    std::size_t Bytes()const{return bytes_;}
    std::size_t Meshes()const{return entries_.size();}
private:
    struct Entry {std::shared_ptr<const StaticWorldMesh> mesh;std::size_t bytes;};
    std::deque<Entry> entries_;
    std::size_t budget_,bytes_=0;
};
class StaticWorldMaterials;
struct WorldBatch:WorldDrawState {
    struct ShardLighting {Mth::Vector normal;std::array<float,3> unlit;};
    std::vector<ShardLighting> shard_lighting;
    // Only a lit Shatter snapshot owns light data; passes of the same geom
    // share its immutable light frame. Normal geometry never copies it.
    std::shared_ptr<const LightingFrame> shard_lights;
    std::vector<SourceRenderVertex> vertices;
    std::shared_ptr<const StaticWorldMesh> retained;
    std::shared_ptr<const std::vector<uint16>> indices;
    // CPU topology used to expand the Moscow player directly into a flat GPU stream.
    bool expand_on_upload=false;
    bool GpuIndexed()const{return indices&&!expand_on_upload;}
    // Frame-only. The owning scene and geoms outlive gather and submit.
    SourceGpuMesh* skin_mesh=nullptr;
    const Mth::Matrix* skin_transform=nullptr;
    std::size_t skin_vertices=0;
    const StaticWorldMaterials* static_materials=nullptr;
    uint32 material_group=0;
    std::size_t ElementCount()const{return indices?indices->size():VertexData().size();}
    const std::vector<SourceRenderVertex>& VertexData()const{return retained?retained->vertices:vertices;}
    std::size_t VertexCount()const{return expand_on_upload?indices->size():(skin_mesh?skin_vertices:VertexData().size());}
};
struct WorldBasePasses {unsigned diffuse,opacity;};
struct StaticFacingBounds {
    Mth::Vector normal{0,0,0,0};
    float offset=0,normal_error=-1;
};
WorldBasePasses FindWorldBasePasses(const MaterialAsset&);
void SetWorldMaterialState(WorldDrawState&,const MaterialAsset&,unsigned pass,unsigned diffuse,unsigned opacity,bool sky,bool performance,bool no_shadow);
class StaticWorldMaterials {
public:
    StaticWorldMaterials(const SceneAsset&,const Nx::CTexDict*);
    void Bind(WorldBatch&,std::size_t material)const;
    std::size_t GroupCount()const{return groups_;}
    std::size_t MaterialIndex(uint32 checksum)const;
private:
    struct Entry {std::uintptr_t texture=0;uint32 group=0;};
    std::vector<std::array<Entry,2>> materials_;
    std::vector<std::pair<uint32,uint32>> material_indices_;
    uint32 groups_=0;
};
struct WorldResidencyStats {std::size_t bytes=0,meshes=0,hits=0,uploads=0,evictions=0,streamed=0,draws=0,stream_vertices=0,stream_uploads=0,indexed_meshes=0,static_indexed_runs=0,stream_indices=0,budget=0,prefix_hits=0,strip_runs=0,strip_saved_indices=0;};
constexpr std::size_t StaticWorldVertexBudget=4*1024*1024;
std::size_t CityStaticWorldBudget(std::size_t resident_bytes,std::size_t linear_free,std::size_t arena_free,bool keep_turn_buffers=false);
struct WorldDrawRun {
    const WorldBatch* first=nullptr;
    std::size_t count=0;
    // Borrows a contiguous span of the frame's sorted batches.
    std::size_t retained_count=0,element_count=0;
    bool indexed=false;
    std::size_t transient_count=1;
    bool source_strip=false;
    std::size_t IndexCount()const{return indexed?element_count:0;}
    std::size_t BatchCount()const{return retained_count?retained_count:transient_count;}
};
bool MatchesStaticRun(const std::vector<std::shared_ptr<const StaticWorldMesh>>& resident,
    bool resident_indexed,const WorldDrawRun& run,bool allow_prefix);
std::vector<WorldDrawRun> BuildWorldDrawRuns(const std::vector<WorldBatch>&,bool merge_materials=true,bool ordered_streams=false);
void AppendWorldDrawRun(const WorldDrawRun&,float u_scale,float v0,float v1,std::vector<SourceRenderVertex>&);
bool AppendWorldStreamRun(const WorldDrawRun&,float u_scale,float v0,float v1,std::vector<SourceRenderVertex>&,std::vector<uint16>&,std::vector<SourceIndexRange>&,SourceDrawPacket&);
struct WorldStreamWriter {
    SourceRenderVertex* vertices;
    uint16* indices;
    SourceIndexRange* ranges;
    std::size_t vertex_capacity, index_capacity, range_capacity;
    std::size_t vertex_count=0, index_count=0, range_count=0;

    bool Append(const WorldDrawRun&,float u_scale,float v0,float v1,SourceDrawPacket&);
    bool Full() const {
        return vertex_count==vertex_capacity&&index_count==index_capacity&&range_count==range_capacity;
    }
};
void PrepareWorldFrustum(WorldView&);
// Upper bound on the 3x3 transform's largest singular value, including shear.
// Identity world sectors retain their real radius instead of sqrt(3) inflation.
float ConservativeSphereScale(const Mth::Matrix&);
bool SphereVisible(const WorldView&,const Mth::Vector&);
bool GatherNativeWorld(const WorldView&,uint32,std::vector<WorldBatch>&,bool group_opaque=false);
bool SortWorldBatches(std::vector<WorldBatch>&,bool group_opaque,WorldDetailStats* detail=nullptr,SourceFrameWorker* worker=nullptr);
void CoalesceOpaqueWorldBatches(std::vector<WorldBatch>&,std::size_t sorted_prefix=0);
unsigned GroupStaticMaterialWindows(const WorldView&,std::vector<WorldBatch>&);
unsigned GroupScreenDisjointWorldBatches(const WorldView&,std::vector<WorldBatch>&,bool merge_materials=true);
void PrepareWorldMaterial(const WorldDrawState&,SourceDrawPacket&);
void ReleaseNativeWorldRegistry();
// Batch indices change with visibility. A slot must not retain an unrelated
// historical maximum indefinitely; keep only bounded spare capacity.
bool RetainWorldVertexBuffer(std::size_t capacity,std::size_t needed);
class NativeWorldBatch {
public:
    NativeWorldBatch()=default;~NativeWorldBatch();
    NativeWorldBatch(const NativeWorldBatch&)=delete;NativeWorldBatch& operator=(const NativeWorldBatch&)=delete;
    bool Prepare(const WorldView&,const std::vector<WorldBatch>&,std::vector<SourceDrawPacket>&);
    void Release();
    WorldResidencyStats ResidencyStats()const{return stats_;}
private:
    struct Buffer {SourceGpuMesh* gpu=nullptr;std::size_t capacity=0,index_capacity=0;};
    Buffer streaming_;
    struct Resident {std::vector<std::shared_ptr<const StaticWorldMesh>> meshes;SourceGpuMesh* gpu=nullptr;std::size_t capacity=0,index_capacity=0;float u_scale=1,v0=0,v1=1;uint64 last_seen=0;bool source_strip=false;};
    using Residency = std::multimap<uint64,Resident>;
    Residency resident_;
    // A serial hints at the previous exact run. The multimap remains the
    // owner and handles collisions, alternate lengths and prefix matches.
    std::vector<Residency::iterator> recent_runs_;
    Residency::iterator next_owner_check_=resident_.end();
    struct Candidate {
        Residency::iterator resident;
        uint64 key=0;
        float u=1,v0=0,v1=1;
        bool keep=false,deferred=false;
    };
    struct RunObservation {uint64 serial=0,tail=0;unsigned frame=0;};
    std::vector<RunObservation> observed_;
    std::vector<Candidate> choices_;
    std::vector<Residency::iterator> eviction_candidates_;
    std::vector<WorldDrawRun> runs_;
    std::vector<SourceDrawPacket> packets_;
    uint64 frame_=0;WorldResidencyStats stats_;
    void evict(Residency::iterator);
};
}
namespace Nx {
class CNativeGeom final:public CGeom {
public:
    CNativeGeom();~CNativeGeom()override;
    bool Assign(std::shared_ptr<SourcePort::SceneAsset>,std::vector<std::size_t>,CTexDict*,bool sky,bool dictionary);
    // Projection receivers use only base triangles; projector UV/material
    // replace the surface's own sampler streams (including unused sentinels).
    void Gather(const SourcePort::WorldView&,uint32,std::vector<SourcePort::WorldBatch>&,bool snapshot=false,bool raw_colors=false,bool projection_receiver=false,unsigned frustum_mask=0x3f)const;
    void SetScene(CScene* scene){scene_=scene;}
    void SetStaticWorldMaterials(std::shared_ptr<const SourcePort::StaticWorldMaterials> materials){static_materials_=std::move(materials);city_prop_extent_=-1;}
    void EnableStaticWorldPartition();
    bool InStaticWorldPartition()const{return partitioned_;}
    void MarkPartitionVisible(uint64 stamp,unsigned planes,unsigned occluders){partition_stamp_=stamp;partition_planes_=planes;partition_occluders_=occluders;}
    bool VisibleInPartition(uint64 stamp)const{return !partitioned_||partition_stamp_==stamp;}
    unsigned PartitionOccluders(uint64 stamp)const{return partitioned_&&partition_stamp_==stamp?partition_occluders_:~0u;}
    unsigned PartitionPlanes(uint64 stamp)const{return partitioned_&&partition_stamp_==stamp?partition_planes_:0x3f;}
    void SetModelCullingSphere(const Mth::Vector& sphere){model_cull_sphere_=sphere;model_cull_valid_=true;}
    void SetTextureDictionary(CTexDict* dictionary){if(dictionary_!=dictionary){dictionary_=dictionary;invalidate_render_cache();}}
    bool CastsShadow()const{return shadow_enabled_;}
    std::size_t SourceSectorCount()const{return sectors_.size();}
    bool StaticWorldBounds(Mth::CBBox&)const;
    void ReleaseColdRenderTopology(uint32 stamp);
private:
    struct Override {bool color_set=false,uv_set=false;Image::RGBA color;Mth::Matrix uv;Override(){uv.Ident();}};
    std::shared_ptr<SourcePort::SceneAsset> asset_;
    std::shared_ptr<const SourcePort::StaticWorldMaterials> static_materials_;
    std::vector<std::size_t> sectors_;SourcePort::CASAsset cas_;
    CTexDict* dictionary_=nullptr;CScene* scene_=nullptr;CModel* model_=nullptr;CModelLights* lights_=nullptr;
    Mth::Matrix transform_;Mth::Vector scale_;Mth::CBBox box_;mutable Mth::CBBox world_box_;Mth::Vector sphere_;
    Mth::Vector model_cull_sphere_;bool model_cull_valid_=false;
    mutable Mth::Matrix render_transform_;
    mutable Mth::Vector world_sphere_;
    mutable float world_sphere_scale_=1;
    mutable bool world_sphere_dirty_=true;
    mutable bool render_transform_identity_=false;
    float city_prop_extent()const;
    mutable float city_prop_extent_=-1;
    bool billboard_=false;
    bool partitioned_=false;
    uint64 partition_stamp_=0;
    unsigned partition_planes_=0x3f,partition_occluders_=~0u;
    void LeaveStaticWorldPartition();
    std::vector<Mth::Matrix> direct_bones_;
    std::shared_ptr<const std::vector<Mth::Matrix>> model_pose_;
    const std::vector<Mth::Matrix>& BoneMatrices()const{return model_pose_?*model_pose_:direct_bones_;}
    std::map<std::pair<uint32,unsigned>,Override> overrides_;
    std::map<std::size_t,std::vector<SourcePort::AssetVertex>> edited_;
    // Original VC lighting edits only the diffuse stream, not positions,
    // normals, weights or UVs. Keep those immutable source streams shared.
    std::map<std::size_t,std::vector<uint32>> render_colors_;
    struct SkinMesh {
        std::vector<uint16> bones;
        SourceGpuMesh* gpu=nullptr;
        std::size_t uploaded_vertices=0;
        uint64 attributes_revision=0;
        ~SkinMesh();
        bool UpdatePose(const std::vector<Mth::Matrix>&) const;
    };
    struct RenderMeshCache {SourcePort::RenderTopology topology;uint32 mask=0;unsigned lod=0;std::size_t material=0;unsigned base_pass=0,blend_pass=0;uint8 overlay_passes=0;bool valid=false;
        mutable std::vector<SourcePort::DrawVertexAttributes> attributes;mutable uint64 attributes_revision=0;
        mutable std::shared_ptr<const std::vector<uint16>> indices;
        mutable Mth::Vector box_center,box_half_size;
        mutable uint64 bounds_revision=0;
        mutable SourcePort::StaticFacingBounds facing;
        mutable bool facing_ready=false;
        mutable std::weak_ptr<const SourcePort::StaticWorldMesh> retained;mutable uint64 revision=0;
        mutable unsigned unsupported_palette=0;
        std::unique_ptr<RenderMeshCache> distant;
        mutable std::unique_ptr<SkinMesh> skin;
        std::unique_ptr<SourcePort::SharedSkinPositions> shared_positions; // first mesh owns its sector's table
        mutable std::unique_ptr<SourcePort::DistantPedPose> ped_pose;
    };
    // Indexed by the geom's own sector slot, not the shared asset's global index.
    mutable std::vector<std::vector<RenderMeshCache>> render_meshes_;
    mutable uint32 last_world_gather_=0;
    std::vector<RenderMeshCache>& render_sector(std::size_t slot)const;
    const RenderMeshCache& render_mesh(RenderMeshCache&,std::size_t sector,std::size_t mesh,unsigned lod,bool generated=false,bool gameplay_skin=false)const;
    uint64 render_revision_=1,attributes_revision_=1;mutable std::shared_ptr<const uint8> render_owner_;
    void invalidate_render_cache(bool attributes=true){++render_revision_;if(attributes)++attributes_revision_;render_owner_.reset();}
    Image::RGBA color_;bool color_set_=false,active_=true,sky_=false,dictionary_only_=false,explicit_uv_=false,shadow_enabled_=false;
    uint32 visibility_=0xffffffff,hide_mask_=0;
    std::array<float,8> wibble_{};bool custom_wibble_=false;std::array<float,2> uv_offset_{};
    void bounds();void copy_state(CNativeGeom&,bool)const;
    std::vector<SourcePort::AssetVertex>& edit(std::size_t);
    std::size_t vertex_count(std::size_t) const;
    SourcePort::AssetVertex vertex(std::size_t, std::size_t) const;
    bool material_pass(uint32,int,uint32&,unsigned&)const;
    CGeom* plat_clone(bool,CScene*)override;CGeom* plat_clone(bool,CModel*)override;
    bool plat_load_geom_data(CMesh*,CModel*,bool)override;void plat_finalize()override;
    uint32 plat_get_checksum()override;
    void plat_set_color(Image::RGBA)override;Image::RGBA plat_get_color()const override;void plat_clear_color()override;
    bool plat_set_material_color(uint32,int,Image::RGBA)override;Image::RGBA plat_get_material_color(uint32,int)override;
    void plat_set_visibility(uint32 mask)override{visibility_=mask;}uint32 plat_get_visibility()const override{return visibility_;}
    void plat_set_active(bool on)override{active_=on;}bool plat_is_active()const override{return active_;}
    const Mth::CBBox& plat_get_bounding_box()const override;
    bool sphere_from_bounds_=false;
    void plat_set_bounding_sphere(const Mth::Vector& s)override{sphere_from_bounds_=false;sphere_=s;model_cull_valid_=false;world_sphere_dirty_=true;}
    const Mth::Vector plat_get_bounding_sphere()const override{return sphere_;}
    void plat_set_world_position(const Mth::Vector&)override;const Mth::Vector& plat_get_world_position()const override{return transform_[3];}
    void plat_set_orientation(const Mth::Matrix&)override;const Mth::Matrix& plat_get_orientation()const override{return transform_;}
    void plat_rotate_y(Mth::ERot90)override;
    void plat_set_transform(const Mth::Matrix&)override;const Mth::Matrix& plat_get_transform()const override{return transform_;}
    void plat_set_scale(const Mth::Vector&)override;Mth::Vector plat_get_scale()const override{return scale_;}
    void plat_set_model_lights(CModelLights* lights)override{lights_=lights;}
    bool plat_render(Mth::Matrix*,Mth::Matrix*,int)override;void plat_set_bone_matrix_data(Mth::Matrix*,int)override;
    bool plat_hide_polys(uint32 mask)override{if(hide_mask_!=mask){hide_mask_=mask;invalidate_render_cache();}return asset_!=nullptr;}
    bool plat_enable_shadow(bool)override;
    int plat_get_num_render_polys()override;int plat_get_num_render_base_polys()override;int plat_get_num_render_verts()override;
    void plat_get_render_verts(Mth::Vector*)override;void plat_get_render_colors(Image::RGBA*)override;
    void plat_set_render_verts(Mth::Vector*)override;void plat_set_render_colors(Image::RGBA*)override;
    void plat_set_uv_wibble_params(float,float,float,float,float,float,float,float)override;
    void plat_use_explicit_uv_wibble(bool on)override{if(explicit_uv_!=on){explicit_uv_=on;invalidate_render_cache();}}
    void plat_set_uv_wibble_offsets(float u,float v)override{if(!explicit_uv_||uv_offset_!=std::array<float,2>{u,v}){uv_offset_={u,v};explicit_uv_=true;invalidate_render_cache();}}
    bool plat_set_uv_wibble_offsets(uint32,int,float,float)override;
    bool plat_set_uv_matrix(uint32,int,const Mth::Matrix&)override;bool plat_allocate_uv_matrix_params(uint32,int)override;
};
class CNativeSector final:public CSector {
public:CNativeSector();
private:bool shatter_=false;
    void plat_set_shatter(bool)override;bool plat_get_shatter()const override{return shatter_;}
    CSector* plat_clone(bool,CScene*)override;
};
class CNativeScene final:public CScene {
public:
    explicit CNativeScene(int bits=10);~CNativeScene()override;
    bool Assign(std::shared_ptr<SourcePort::SceneAsset>,CTexDict*,bool sky,bool dictionary,const char* scene_name = nullptr);
    bool Add(std::shared_ptr<SourcePort::SceneAsset>);
private:
    std::shared_ptr<SourcePort::SceneAsset> asset_;std::vector<std::shared_ptr<SourcePort::SceneAsset>> additions_;
    bool partition_world_=false;
    Image::RGBA majority_;void bounds();
    void plat_post_load()override;void plat_post_add()override;
    bool plat_load_textures(const char*)override;bool plat_load_collision(const char*)override;bool plat_add_collision(const char*)override;
    bool plat_unload_add_scene()override;CSector* plat_create_sector()override;
    void plat_set_majority_color(Image::RGBA c)override{majority_=c;}Image::RGBA plat_get_majority_color()const override{return majority_;}
};
class CNativeModel final:public CModel {
public:
    ~CNativeModel()override;
    std::shared_ptr<const std::vector<Mth::Matrix>> CapturePose(const Mth::Matrix*,unsigned);
private:
    std::shared_ptr<std::vector<Mth::Matrix>> current_pose_,previous_pose_;
    int skeleton_size_=0;
    bool plat_set_render_mode(ERenderMode)override;
    bool plat_init_skeleton(int)override;
    Mth::Vector plat_get_bounding_sphere()override;
    void plat_set_bounding_sphere(const Mth::Vector&)override;
};
}
