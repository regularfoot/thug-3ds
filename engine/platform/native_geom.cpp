#include <source_port/stereo.h>
#include <source_port/native_world.h>
#include <source_port/native_occlusion.h>
#include <source_port/native_lighting.h>
#include <source_port/material_animation.h>
#include <source_port/application_os.h>
#include <source_port/launch_paths.h>
#include <source_port/input.h>
#include <source_port/clock_os.h>
#include <source_port/graphics.h>
#include <source_port/thread_os.h>
#include <source_port/memory.h>
#include <gfx/nx.h>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
namespace {
std::vector<Nx::CNativeGeom*> geoms;
struct WorldPartitionEntry {
    Nx::CNativeGeom* geom;
    Mth::CBBox bounds;
    Mth::Vector center;
    std::size_t source_order;
};
// Registry indices are stable until the partition is rebuilt. One bit per
// object gives both eyes the canonical source order, including moving actors.
std::vector<uint32> ordinary_bits,visible_bits;
struct WorldPartitionCell {
    Mth::Vector center,half;
    std::size_t first,last,left=0,right=0;
    bool range_limited=false;
};
std::vector<WorldPartitionEntry> partition_entries;
std::vector<WorldPartitionCell> partition_cells;
bool partition_dirty=true;
uint64 partition_stamp=0;
uint64 static_mesh_serial=0;
uint32 world_gather_stamp=0;
void controller(){Dbg_Assert(source_thread_is_controller());}
Mth::Vector transformed(const Mth::Vector& p,const Mth::Matrix& m){Mth::Vector out;for(unsigned c=0;c<4;++c)out[c]=p[0]*m[0][c]+p[1]*m[1][c]+p[2]*m[2][c]+p[3]*m[3][c];return out;}
Mth::Vector authored_position(const std::array<float, 3>& position){
    // A canonical identity transform ends each component with +0. Match
    // its treatment of negative zero as well as ordinary finite positions.
    const auto& p=position;
    return Mth::Vector(p[0]+0.f,p[1]+0.f,p[2]+0.f,1);
}
bool finite(const Mth::Vector& p){for(unsigned c=0;c<4;++c)if(!std::isfinite(p[c]))return false;return true;}
uint8 byte(float value){Dbg_Assert(!std::isnan(value));return uint8(std::clamp(value,0.f,255.f));}
uint32 packed(Image::RGBA c){return uint32(c.a)<<24|uint32(c.r)<<16|uint32(c.g)<<8|c.b;}
Image::RGBA unpacked(uint32 c){return Image::RGBA(c>>16,c>>8,c,c>>24);}
bool is_pool_mask(const SourcePort::MaterialPass& base,const SourcePort::MaterialPass& detail){
    // The PC shader uses the dark layer's alpha to blend in the concrete.
    // On the one-pass path use the actual diffuse texture and its own UVs.
    // Other previous-mask materials can be translucent decals or glass.
    return (base.alpha&0xffffff)==5&&(detail.alpha&0xffffff)==12&&
        ((base.texture==0xa54d6d80&&detail.texture==0x3b2c2b40)||
         (base.texture==0x352da6c0&&detail.texture==0xc9d691e0));
}
// Retail diffuse maps that mix solid surfaces with an opaque reflection.
// The colour mask controls the highlight. It is not framebuffer transparency.
bool solid_reflection_surface(const SourcePort::MaterialAsset& material){
    const auto& reflection=material.passes[0];
    const auto& diffuse=material.passes[1];
    if(material.sorted||material.alpha_cutoff>1||reflection.flags!=0x102c||
       (diffuse.flags!=0x24&&diffuse.flags!=0x1024)||(reflection.alpha&0xffffff)!=0||
       (diffuse.alpha&0xffffff)!=5)return false;
    switch(reflection.texture){
    case 0x6e6a3840:return diffuse.texture==0xd95c9f00; // NY station corridors
    case 0xef4d2b40:return diffuse.texture==0xfdb0e178; // NY stairwell tiles
    case 0x9753b1a0:return diffuse.texture==0x6a16f43c; // HI elevator doors and brushed metal panels
    case 0xa0489a20: // HI hotel exteriors, apartment and Wallows shutters
        switch(diffuse.texture){
        case 0x2d3a3a80:case 0xd76aec58:case 0x4f56929c:case 0x4fcb4f10:
        case 0xa8d78ce8:case 0xa1e2a038:case 0xdee076c4:case 0xae2ed2a4:
        case 0xeb5ac85c:case 0xf6e56e48:return true;
        }
        return false;
    case 0xbb4ef240:return diffuse.texture==0x9a61ab0c; // NY Chase floor, ledges and stone plinth
    case 0x480ecb10:return diffuse.texture==0x9409e7bc; // SE backstage wall and hallway
    case 0x6fa9c1c0:return diffuse.texture==0xe0274010; // FL roller shutter
    case 0x485d71a0:case 0x26499aa0:return diffuse.texture==0x127129dc; // VC plaster
    case 0x93112a90:return diffuse.texture==0x1fa92f8c||diffuse.texture==0xcfec3300; // SE cabinets
    case 0xbe9f1228:return diffuse.texture==0xa31f9298; // SE loudspeaker
    case 0x7d854fc8: // RU building fronts with masonry and painted windows
        switch(diffuse.texture){
        case 0x754199f4:case 0x83b3dfb0:case 0x4979ae9c:case 0xb9387980:
        case 0x561370bc:case 0x39abe0e4:case 0x6e094de8:case 0x4a0aaee8:
        case 0xcb1ea560:case 0x26362360:case 0xc7a118e8:case 0xa296abbc:
        case 0x40b98ca8:case 0x4dc63fc4:case 0x692fb8a4:case 0xcdfac3a4:
        case 0xd24439fc:return true;
        }
    }
    return false;
}
bool is_deck_sticker_slot(uint32 texture){
    return texture==0x55528610||texture==0x11ecfc38;
}
bool pass_match(const SourcePort::MaterialAsset& m,uint32 name,int pass,unsigned& adjusted){
    if(pass<0||pass>3)return false;adjusted=pass;
    if(m.name!=name&&m.checksum!=name){if(m.name<=name||m.name-name>0x7f)return false;unsigned diff=m.name-name,separate=diff&7,flags=diff>>3;
        if(separate){if(separate!=unsigned(pass+1))return false;adjusted=0;}
        else{if(!flags||!(flags&(1u<<pass)))return false;for(int p=0;p<pass;++p)if(!(flags&(1u<<p)))--adjusted;}}
    return adjusted<m.passes.size();
}
void face_billboard(const SourcePort::SectorAsset& source,const std::vector<uint16>& triangles,
                    const SourcePort::WorldView& view,const Mth::Matrix& root,
                    std::vector<Mth::Vector>& positions,std::vector<Mth::Vector>& normals){
    if(!(source.flags&0x00800000))return;
    Dbg_Assert((source.billboard_type==1||source.billboard_type==2)&&triangles.size()>=3);
    // XBX stores the pivot in the opposite Z convention to the position
    // stream. The original sector loader negates it before SetBillboardData.
    const Mth::Vector local_pivot(source.billboard[3],source.billboard[4],-source.billboard[5],1);
    Mth::Vector axis(source.billboard[6],source.billboard[7],source.billboard[8],0),up(0,1,0,0);
    bool axial=source.billboard_type==2;
    // Screen-aligned exporter records can carry NaN in the unused pivot axis.
    Dbg_Assert(finite(local_pivot)&&(!axial||finite(axis)));
    if(axial&&std::abs(Mth::DotProduct(axis,up))<=.99f)up=axis;
    Dbg_Assert(up.LengthSqr()>1e-12f);up.Normalize();
    // Original sMesh::SetBillboardData: encode pivot-relative vertices in the
    // first source triangle's normal/u/v basis. No exporter geometry is lost.
    auto normal=Mth::CrossProduct(positions[triangles[1]]-positions[triangles[0]],positions[triangles[2]]-positions[triangles[0]]);
    Dbg_Assert(normal.LengthSqr()>1e-12f);normal.Normalize();
    auto u=Mth::CrossProduct(normal,up);Dbg_Assert(u.LengthSqr()>1e-12f);u.Normalize();
    auto v=Mth::CrossProduct(u,normal);v.Normalize();
    const auto pivot=transformed(local_pivot,root);
    auto world_up=transformed(up,root);world_up[3]=0;world_up.Normalize();
    auto at=view.axes[Z];at[3]=0;at.Normalize();
    auto right=Mth::CrossProduct(at,axial?world_up:Mth::Vector(0,1,0,0));
    // The SDK's zero-vector normalization collapses a directly vertical view.
    // Use the real camera's other axis for this singular orientation.
    if(right.LengthSqr()<1e-12f)right=-view.axes[X];right[3]=0;right.Normalize();
    auto facing_up=axial?world_up:Mth::CrossProduct(right,at).Normalize();
    auto facing_at=axial?Mth::CrossProduct(facing_up,right).Normalize():at;
    float sx=root[X].Length(),sy=root[Y].Length(),sz=root[Z].Length();
    for(std::size_t i=0;i<positions.size();++i){
        auto delta=positions[i]-local_pivot;
        positions[i]=pivot+right*(Mth::DotProduct(delta,u)*sx)+facing_up*(Mth::DotProduct(delta,v)*sy)+facing_at*(Mth::DotProduct(delta,normal)*sz);
        positions[i][3]=1;normals[i]=facing_at;
    }
}
}
namespace SourcePort {
// The dictionary may be edited between frames (notably Create-a-Deck).
// Lookups normally live for one gather. Across frames, borrowed entries
// remain valid only for the generation of the texture dictionaries.
class WorldTextureLookup {
    struct Entry {const Nx::CTexDict* dict=nullptr; uint32 checksum=0; Nx::CNativeTexture* texture=nullptr;};
    static constexpr unsigned count=256;
    std::array<Entry,count> slots_{};
    // Only the optional inter-frame cache uses the larger table. Dictionary
    // edits and texture destruction invalidate positive and negative entries.
    struct Persistent {uint64 generation=0;std::array<Entry,2048> slots;};
    static std::unique_ptr<Persistent>& Store(){
        static std::unique_ptr<Persistent> store;
        return store;
    }
    Persistent* persistent_=nullptr;
public:
    void ReuseDictionary(){
        auto& store=Store();
        if(!store){
            NativeHeapScope storage;
            store=std::make_unique<Persistent>();
        }
        auto& cache=*store;
        if(cache.generation!=WorldTextureGeneration()){
            cache.slots.fill({});
            cache.generation=WorldTextureGeneration();
        }
        persistent_=&cache;
    }
    static void Release(){Store().reset();}
    unsigned hits=0,misses=0;
    Nx::CNativeTexture* Resolve(const Nx::CTexDict& dict,uint32 checksum){
        const auto hash=checksum^(checksum>>16)^(reinterpret_cast<std::uintptr_t>(&dict)>>4);
        auto& slot=persistent_?persistent_->slots[hash&(persistent_->slots.size()-1)]:slots_[hash&(count-1)];
        if(slot.dict==&dict&&slot.checksum==checksum){++hits;return slot.texture;}
        ++misses;
        auto* texture=dynamic_cast<Nx::CNativeTexture*>(dict.GetTexture(checksum));
        slot={&dict,checksum,texture};
        return texture;
    }
};
static Nx::CNativeTexture* world_texture(const WorldView& view,const Nx::CTexDict& dict,uint32 checksum){
    return view.texture_lookup?view.texture_lookup->Resolve(dict,checksum):
        dynamic_cast<Nx::CNativeTexture*>(dict.GetTexture(checksum));
}
void ReleaseNativeWorldRegistry(){
    controller();Dbg_Assert(geoms.empty());
    WorldTextureLookup::Release();
    decltype(geoms)().swap(geoms);
    decltype(partition_cells)().swap(partition_cells);
    decltype(partition_entries)().swap(partition_entries);
    decltype(ordinary_bits)().swap(ordinary_bits);
    decltype(visible_bits)().swap(visible_bits);
    partition_dirty=true;
    partition_stamp=0;
}
float ConservativeSphereScale(const Mth::Matrix& matrix){
    float frobenius=0,one=0,infinity=0;
    for(unsigned r=0;r<3;++r){float row=0,column=0;
        for(unsigned c=0;c<3;++c){float value=matrix[r][c];frobenius+=value*value;row+=std::abs(value);column+=std::abs(matrix[c][r]);}
        infinity=std::max(infinity,row);one=std::max(one,column);
    }
    // Both bounds enclose every direction, unlike maximum row length under
    // shear. The induced-norm bound is exact for the common identity/scale.
    // Pad rounding outward so a plane cannot reject a touching vertex.
    return std::sqrt(std::min(frobenius,one*infinity))*(1.f+8.f*std::numeric_limits<float>::epsilon());
}
void PrepareWorldFrustum(WorldView& view){
    if(view.backface_static)
        view.frustum_positive_winding=Mth::DotProduct(Mth::CrossProduct(view.axes[X],view.axes[Y]),view.axes[Z])>.99f;
    view.frustum_hfov=view.hfov;view.frustum_aspect=view.aspect;
    view.frustum_th=std::tan(view.hfov*float(Mth::PI)/360.f);view.frustum_tv=view.frustum_th/view.aspect;
    view.static_detail_horizontal=view.width?view.static_detail_pixels*view.frustum_th/view.width:0;
    view.static_detail_vertical=view.height?view.static_detail_pixels*view.frustum_tv/view.height:0;
    // Enclose the union of both eyes at full depth, including script queries
    // outside a render frame. In view space the absolute offset is
    // half_iod * abs(tan(hfov/2) - depth/focus).
    if (view.static_lod_pixels > 0) {
        const auto focal_pixels = .5f * std::max(view.width / view.frustum_th, view.height / view.frustum_tv);
        view.static_lod_depth_per_unit = 1.5f * focal_pixels / view.static_lod_pixels;
    }
    view.frustum_eye_pad=.5f*SOURCE_STEREO_MAX_IOD*view.frustum_th;
    view.frustum_th+=.5f*SOURCE_STEREO_MAX_IOD/SOURCE_STEREO_FOCUS;
    view.frustum_rx=std::sqrt(1+view.frustum_th*view.frustum_th);view.frustum_ry=std::sqrt(1+view.frustum_tv*view.frustum_tv);
    auto horizontal=view.axes[Z]*view.frustum_th;
    auto vertical=view.axes[Z]*view.frustum_tv;
    const Mth::Vector normals[]={view.axes[Z],-view.axes[Z],
        horizontal+view.axes[X],horizontal-view.axes[X],
        vertical+view.axes[Y],vertical-view.axes[Y]};
    for(std::size_t i=0;i<view.frustum_planes.size();++i){
        const auto& n=normals[i];
        view.frustum_planes[i].normal=n;
        view.frustum_planes[i].absolute.Set(std::abs(n[X]),std::abs(n[Y]),std::abs(n[Z]),0);
    }
}
static bool SphereInFrustum(const WorldView& view,const Mth::Vector& sphere){
    if(!finite(sphere)||sphere[3]<0)return false;
    const auto delta=sphere-view.position;
    const float distance=-Mth::DotProduct(delta,view.axes[Z]),radius=sphere[3];
    if(distance+radius<view.near_clip||distance-radius>view.far_clip)return false;
    const bool prepared=view.frustum_hfov==view.hfov&&view.frustum_aspect==view.aspect;
    const float tan_half=prepared?0.f:std::tan(view.hfov*float(Mth::PI)/360.f);
    const float th=prepared?view.frustum_th:tan_half+.5f*SOURCE_STEREO_MAX_IOD/SOURCE_STEREO_FOCUS;
    const float tv=prepared?view.frustum_tv:tan_half/view.aspect;
    const float eye_pad=prepared?view.frustum_eye_pad:.5f*SOURCE_STEREO_MAX_IOD*tan_half;
    float rx=prepared?view.frustum_rx:std::sqrt(1+th*th),ry=prepared?view.frustum_ry:std::sqrt(1+tv*tv);
    return std::abs(Mth::DotProduct(delta,view.axes[X]))<=distance*th+radius*rx+eye_pad&&
           std::abs(Mth::DotProduct(delta,view.axes[Y]))<=distance*tv+radius*ry;
}
static bool BoxInFrustum(const WorldView& view,const Mth::Vector& center,const Mth::Vector& half_size,unsigned planes){
    if(!planes)return true;
    const auto delta=center-view.position;
    const float limits[]={-view.near_clip,view.far_clip,view.frustum_eye_pad,view.frustum_eye_pad,0,0};
    for(;planes;planes&=planes-1){
        const auto i=__builtin_ctz(planes);
        const auto& plane=view.frustum_planes[i];
        const float distance=Mth::DotProduct(delta,plane.normal);
        const float radius=Mth::DotProduct(half_size,plane.absolute);
        // Leave a small margin for the GPU's projection and rounding.
        if(distance>limits[i]+radius+0.01f)return false;
    }
    return true;
}
static std::size_t BuildWorldPartitionCell(std::size_t first,std::size_t last){
    Mth::CBBox bounds;
    bounds.Reset();
    for(auto i=first;i<last;++i){
        bounds.AddPoint(partition_entries[i].bounds.GetMin());
        bounds.AddPoint(partition_entries[i].bounds.GetMax());
    }
    auto half=(bounds.GetMax()-bounds.GetMin())*.5f;
    const auto node=partition_cells.size();
    partition_cells.push_back({bounds.GetMin()+half,half,first,last});
    if(last-first<=8){
        partition_cells[node].range_limited=std::all_of(partition_entries.begin()+first,partition_entries.begin()+last,
            [](const auto& entry){return entry.geom->CanLimitWorldRange();});
        return node;
    }

    const unsigned axis=half[X]>=half[Z]?X:Z;
    const auto middle=first+(last-first)/2;
    std::nth_element(partition_entries.begin()+first,partition_entries.begin()+middle,partition_entries.begin()+last,
        [axis](const auto& a,const auto& b){return a.center[axis]<b.center[axis];});
    const auto left=BuildWorldPartitionCell(first,middle);
    const auto right=BuildWorldPartitionCell(middle,last);
    partition_cells[node].left=left;
    partition_cells[node].right=right;
    partition_cells[node].range_limited=partition_cells[left].range_limited && partition_cells[right].range_limited;
    return node;
}
static bool PrepareWorldPartition(){
    if(!partition_dirty)return !partition_cells.empty();
    SourcePort::RootHeapScope storage;
    partition_entries.clear();
    partition_cells.clear();
    ordinary_bits.assign((geoms.size()+31)/32,0);
    for(std::size_t i=0;i<geoms.size();++i){auto* geom=geoms[i];
        if(!geom->InStaticWorldPartition()){ordinary_bits[i/32]|=uint32(1)<<(i%32);continue;}
        Mth::CBBox bounds;
        Dbg_Assert(geom->StaticWorldBounds(bounds));
        partition_entries.push_back({geom,bounds,(bounds.GetMin()+bounds.GetMax())*.5f,i});
    }
    if(partition_entries.size()>=64)BuildWorldPartitionCell(0,partition_entries.size());
    partition_dirty=false;
    return !partition_cells.empty();
}
static bool ClassifyWorldBox(const WorldView& view,const Mth::Vector& center,const Mth::Vector& half,unsigned& planes){
    const auto delta=center-view.position;
    const float limits[]={-view.near_clip,view.far_clip,view.frustum_eye_pad,view.frustum_eye_pad,0,0};
    for(unsigned bits=planes;bits;bits&=bits-1){
        const unsigned i=__builtin_ctz(bits);
        const unsigned bit=1u<<i;
        const auto& plane=view.frustum_planes[i];
        const float distance=Mth::DotProduct(delta,plane.normal);
        const float radius=Mth::DotProduct(half,plane.absolute);
        if(distance>limits[i]+radius+.02f)return false;
        // Inherit a plane only with enough slack for both the cell and
        // the independently rounded mesh bounds in it.
        const float inside_margin=.125f+8.f*std::numeric_limits<float>::epsilon()*
            (std::abs(distance)+radius+std::abs(limits[i]));
        if(distance+radius<limits[i]-inside_margin)planes&=~bit;
    }
    return true;
}
static void MarkVisibleWorldCells(const WorldView& view,std::size_t node,unsigned planes,unsigned occluders=~0u){
    const auto& cell=partition_cells[node];
    if(view.static_world_radius>0 && cell.range_limited){
        const float x=std::max(0.f,std::abs(cell.center[X]-view.position[X])-cell.half[X]);
        const float z=std::max(0.f,std::abs(cell.center[Z]-view.position[Z])-cell.half[Z]);
        // A little slack covers center/extent rounding relative to sector bounds.
        const float radius=view.static_world_radius+std::abs(view.stereo_eye_shift)+1.f;
        if(x*x+z*z>radius*radius)return;
    }
    if(!ClassifyWorldBox(view,cell.center,cell.half,planes))return;
    // Reject a whole branch only if one original occluder covers the
    // complete AABB from both eyes. Other branches keep per-mesh testing.
    const bool test_occlusion=occluders && view.occlude_static_cells&&view.occlusion&&view.occlusion->HasVolumes();
    if(test_occlusion && (view.prune_occluders?view.occlusion->ClassifyCell(cell.center,cell.half,occluders):
                                              view.occlusion->HidesBox(cell.center,cell.half))){
        if(view.detail_stats)view.detail_stats->occluded_sectors+=cell.last-cell.first;
        return;
    }
    if((planes||(test_occlusion&&occluders))&&cell.left){
        MarkVisibleWorldCells(view,cell.left,planes,occluders);
        MarkVisibleWorldCells(view,cell.right,planes,occluders);
        return;
    }
    for(auto i=cell.first;i<cell.last;++i){
        const auto& entry=partition_entries[i];
        unsigned entry_planes=planes,entry_occluders=occluders;
        if(view.refine_static_leaves && (entry_planes || (test_occlusion&&entry_occluders))){
            const auto half=(entry.bounds.GetMax()-entry.bounds.GetMin())*.5f;
            if(entry_planes && !ClassifyWorldBox(view,entry.center,half,entry_planes))continue;
            if(view.prune_occluders && test_occlusion && entry_occluders && view.occlusion->ClassifyCell(entry.center,half,entry_occluders))continue;
        }
        entry.geom->MarkPartitionVisible(partition_stamp,entry_planes,entry_occluders);
        if(view.direct_partition_gather)visible_bits[entry.source_order/32]|=uint32(1)<<(entry.source_order%32);
    }
}
static bool OutsideStaticPropRadius(const WorldView& view,const Mth::Vector& center,const Mth::Vector& half_size){
    const float narrow=std::min(half_size[X],half_size[Z]);
    const float wide=std::max(half_size[X],half_size[Z]);
    if(view.small_props_only && (wide>92.f || half_size[Y]>56.f)) return false;
    // Keep backdrops, broad floor pieces and thin wall panels continuous.
    if(wide>360.f||half_size[Y]>144.f)return false;
    if(narrow>64.f&&half_size[Y]<narrow*.12f)return false;
    if(narrow<4.f&&half_size[Y]>48.f&&wide>72.f)return false;
    const float x=std::max(0.f,std::abs(center[X]-view.position[X])-half_size[X]);
    const float z=std::max(0.f,std::abs(center[Z]-view.position[Z])-half_size[Z]);
    return x*x+z*z>view.static_prop_radius*view.static_prop_radius;
}
static bool TinyDistantMesh(const WorldView& view,const Mth::Vector& center,const Mth::Vector& half_size){
    if (view.static_detail_pixels <= 0) return false;
    const float radius=half_size[X]+half_size[Y]+half_size[Z];
    // Most meshes cannot meet the pixel limit even at the far plane. Avoid
    // transforming their centers again after the frustum test.
    const float horizontal_limit=view.static_detail_horizontal;
    const float vertical_limit=view.static_detail_vertical;
    if(radius>=std::max(horizontal_limit,vertical_limit)*view.far_clip) return false;

    const auto delta=center-view.position;
    const float depth=-Mth::DotProduct(delta,view.axes[Z]);
    const float near=depth-radius;
    if(near<1800.f) return false;
    const float x=std::abs(Mth::DotProduct(delta,view.axes[X]));
    const float y=std::abs(Mth::DotProduct(delta,view.axes[Y]));
    // The L1 radius encloses the box on every camera axis. Bound projected
    // width including perspective from either eye. A stereo
    // focal offset adds the same constant at both ends and cancels.
    return radius*(1.f+(x+view.frustum_eye_pad)/depth)<=near*horizontal_limit &&
           radius*(1.f+y/depth)<=near*vertical_limit;
}
static StaticFacingBounds BuildStaticFacingBounds(const SectorAsset& source,const RenderTopology& topology,
                                                  const std::vector<uint16>& triangles,const Mth::Vector& center){
    StaticFacingBounds result;
    result.offset=-std::numeric_limits<float>::infinity();
    for(std::size_t i=0;i<triangles.size();i+=3){
        const auto a=authored_position(source.Position(topology.vertices[triangles[i]]));
        const auto b=authored_position(source.Position(topology.vertices[triangles[i+1]]));
        const auto c=authored_position(source.Position(topology.vertices[triangles[i+2]]));
        const auto ab=b-a,ac=c-a,bc=c-b;
        auto normal=Mth::CrossProduct(ab,ac);
        const float normal_squared=normal.LengthSqr();
        // Keep narrow/degenerate triangles on the GPU. Their projected
        // winding can be sensitive to the rasterizer's subpixel rounding.
        if(!(normal_squared>4.f*std::max({ab.LengthSqr(),ac.LengthSqr(),bc.LengthSqr()})))return {};
        normal*=1.f/std::sqrt(normal_squared);
        if(i==0){result.normal=normal;result.normal_error=0;}
        const auto error=normal-result.normal;
        for(unsigned axis=0;axis<3;++axis)
            result.normal_error=std::max(result.normal_error,std::abs(error[axis]));
        if(result.normal_error>.125f)return {}; // A mixed-facing mesh offers little opportunity.
        result.offset=std::max(result.offset,Mth::DotProduct(normal,center-a));
    }
    return result;
}
static bool FacesAwayFromBothEyes(const StaticFacingBounds& facing,const WorldView& view,
                                  const Mth::Vector& center,const Mth::Vector& half_size){
    if(facing.normal_error<0||!view.frustum_positive_winding)return false;
    const auto eye=view.position-center;
    const auto& near=view.frustum_planes[0];
    if(Mth::DotProduct(eye,near.normal)-Mth::DotProduct(half_size,near.absolute)<=view.near_clip+4.f)
        return false;
    const float reach=std::abs(eye[X])+std::abs(eye[Y])+std::abs(eye[Z]);
    const auto& right=view.axes[X];
    const float stereo_reach=view.stereo_eye_shift*(std::abs(right[X])+std::abs(right[Y])+std::abs(right[Z]));
    // This upper bound covers the signed plane distance of every triangle
    // from either off-axis camera. Leave additional slack at grazing angles.
    const float upper=Mth::DotProduct(facing.normal,eye)+facing.offset+
        (facing.normal_error+.025f)*reach+stereo_reach;
    return upper<-4.f;
}
bool SphereVisible(const WorldView& view,const Mth::Vector& sphere){
    if(!SphereInFrustum(view,sphere))return false;
    if(view.occlusion)return !view.occlusion->HidesSphere(sphere,sphere[3],view.occluder_mask);
    // Script visibility queries (including Film Goal) run before render.
    // Keep their original sphere test when there is no render-view volume.
    auto center=sphere;
    return !NxXbox::TestSphereAgainstOccluders(&center,sphere[3]);
}
bool GatherNativeWorld(const WorldView& view,uint32 time,std::vector<WorldBatch>& out,bool group_opaque){
    controller();
    const auto gather_started=view.detail_stats?source_clock_now():0;
    ++world_gather_stamp;
    auto& memory=Mem::Manager::sHandle();
    if (view.reclaim_cold_topology && memory.BottomUpHeap()->ParentRegion()->MemAvailable()<2*1024*1024) {
        for (auto* geom:geoms) geom->ReleaseColdRenderTopology(world_gather_stamp);
    }
    auto prepared=view;
    std::optional<WorldTextureLookup> texture_lookup;
    if(view.performance){
        texture_lookup.emplace();
        if(view.refine_static_leaves)texture_lookup->ReuseDictionary();
        prepared.texture_lookup=&*texture_lookup;
    }
    PrepareWorldFrustum(prepared);
    const bool use_partition=view.performance&&view.cull_static_meshes&&view.partition_static&&PrepareWorldPartition();
    if(use_partition){
        ++partition_stamp;
        NativeHeapScope storage;
        if(view.direct_partition_gather)visible_bits=ordinary_bits;
        MarkVisibleWorldCells(prepared,0,0x3f);
    }
    // Keep source order for equal sort keys and overlapping transparent draws.
    const auto gather=[&](auto* geom){
        if(view.prune_occluders)prepared.occluder_mask=use_partition?geom->PartitionOccluders(partition_stamp):~0u;
        geom->Gather(prepared,time,out,false,false,false,use_partition&&view.inherit_partition_planes?geom->PartitionPlanes(partition_stamp):0x3f);
    };
    if(use_partition&&view.direct_partition_gather){
        std::size_t gathered=0;
        for(std::size_t word=0;word<visible_bits.size();++word){
            for(uint32 bits=visible_bits[word];bits;bits&=bits-1){
                gather(geoms[word*32+__builtin_ctz(bits)]);
                ++gathered;
            }
        }
        if(view.detail_stats)view.detail_stats->partition_skipped+=geoms.size()-gathered;
    }else for(auto* geom:geoms){
        if(use_partition&&!geom->VisibleInPartition(partition_stamp)){
            if(view.detail_stats)++view.detail_stats->partition_skipped;
            continue;
        }
        gather(geom);
    }
    const auto sort_started=view.detail_stats?source_clock_now():0;
    const bool material_sorted=SortWorldBatches(out,group_opaque,view.detail_stats,view.sort_worker);
    if(view.detail_stats){
        if(texture_lookup){
            view.detail_stats->texture_hits+=texture_lookup->hits;
            view.detail_stats->texture_misses+=texture_lookup->misses;
        }
        view.detail_stats->gather_ticks+=sort_started-gather_started;
        view.detail_stats->sort_ticks+=source_clock_now()-sort_started;
    }
    return material_sorted;
}

WorldBasePasses FindWorldBasePasses(const MaterialAsset& material){
    const auto& passes=material.passes;
    unsigned base=0;
    while(base<passes.size()&&(passes[base].flags&8))++base;
    const bool pool=base+1<passes.size()&&is_pool_mask(passes[base],passes[base+1]);
    return {base+pool,base};
}
static bool WorldMaterialCullsBackfaces(const MaterialAsset& material,unsigned blend){
    return !material.no_backface&&(material.single_sided||blend==0||blend==12||blend==13);
}

void SetWorldMaterialState(WorldDrawState& state,const MaterialAsset& material,unsigned index,
                           unsigned diffuse,unsigned opacity,bool sky,bool performance,bool no_shadow){
    const auto& pass=material.passes[index];
    const bool solid_base=performance&&index==1&&diffuse==1&&opacity==1&&solid_reflection_surface(material);
    const auto& first=material.passes[solid_base?0:opacity];
    unsigned first_blend=unsigned(first.alpha)&0xffffff;
    // Original sMesh::Initialize clears previous-mask modes on the base pass.
    if(first_blend==12||first_blend==13)first_blend=0;
    const auto& alpha=index==diffuse?first:pass;
    state.material=material.checksum;
    state.pass=performance?0:index;
    state.environment=pass.flags&8;
    state.mesh_uv=true;
    state.opaque=first_blend==0;
    state.sky=sky;
    state.camera_relative=sky&&performance;
    state.draw_order=material.draw_order;
    state.z_bias=material.z_bias;
    state.blend=index==diffuse?first_blend:unsigned(pass.alpha)&0xffffff;
    state.fixed_alpha=unsigned(alpha.alpha>>32);
    state.cutoff=material.alpha_cutoff;
    state.u_address=pass.u_address;
    state.v_address=pass.v_address;
    state.filter=pass.filter;
    state.no_skater_shadow=no_shadow;
    state.ignore_vertex_alpha=alpha.flags&(1<<12);
    state.cull=WorldMaterialCullsBackfaces(material,first_blend);
    // The file's DX9 mesh flags do not include the runtime grass NO_ZWRITE.
    state.depth_write=!sky&&index==(performance?diffuse:0);
}
}
namespace Nx {
CNativeGeom::CNativeGeom(){controller();transform_.Ident();scale_.Set(1,1,1,1);sphere_.Set(0,0,0,0);model_cull_sphere_.Set(0,0,0,0);color_=Image::RGBA(128,128,128,128);SourcePort::RootHeapScope registry;geoms.push_back(this);partition_dirty=true;}
CNativeGeom::~CNativeGeom(){controller();LeaveStaticWorldPartition();auto at=std::find(geoms.begin(),geoms.end(),this);Dbg_Assert(at!=geoms.end());geoms.erase(at);partition_dirty=true;if(mp_orig_render_colors)Mem::Free(mp_orig_render_colors);mp_orig_render_colors=nullptr;}
void CNativeGeom::SetStaticWorldMaterials(std::shared_ptr<const SourcePort::StaticWorldMaterials> materials){
    if(partitioned_ && bool(static_materials_)!=bool(materials))partition_dirty=true;
    static_materials_=std::move(materials);
    city_prop_extent_=-1;
}
static bool distant_landmark(const Mth::CBBox& box){
    const auto span=box.GetMax()-box.GetMin();
    return span[Y]>=400.f && std::max(span[X],span[Z])>=2000.f;
}
bool CNativeGeom::CanLimitWorldRange() const{
    if(!partitioned_ || !static_materials_ || distant_landmark(box_))return false;
    // The sector distance test uses source coordinates only for stationary world geometry.
    static constexpr float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    return scale_[X]==1 && scale_[Y]==1 && scale_[Z]==1 &&
           std::memcmp(&transform_[0][0],identity,sizeof identity)==0;
}
void CNativeGeom::EnableStaticWorldPartition(){
    if(partitioned_||!asset_||sky_||dictionary_only_||model_||billboard_||!BoneMatrices().empty())return;
    for(auto sector:sectors_){
        const auto& source=asset_->sectors[sector];
        if(source.bone>=0||(source.flags&(16|0x00800000)))return;
    }
    partitioned_=true;
    partition_dirty=true;
}
void CNativeGeom::LeaveStaticWorldPartition(){
    if(!partitioned_)return;
    partitioned_=false;
    partition_dirty=true;
}
bool CNativeGeom::Assign(std::shared_ptr<SourcePort::SceneAsset> asset,std::vector<std::size_t> sectors,CTexDict* dict,bool sky,bool dictionary){
    controller();if(asset_||!asset)return false;for(auto i:sectors)if(i>=asset->sectors.size())return false;
    asset_=std::move(asset);sectors_=std::move(sectors);dictionary_=dict;sky_=sky;dictionary_only_=dictionary;
    billboard_=std::any_of(sectors_.begin(),sectors_.end(),[&](auto i){return asset_->sectors[i].flags&0x00800000;});
    bounds();return true;
}
std::size_t CNativeGeom::vertex_count(std::size_t sector) const {
    const auto at = edited_.find(sector);
    return at == edited_.end() ? asset_->sectors[sector].VertexCount() : at->second.size();
}
SourcePort::AssetVertex CNativeGeom::vertex(std::size_t sector, std::size_t index) const {
    const auto at = edited_.find(sector);
    return at == edited_.end() ? asset_->sectors[sector].Vertex(index) : at->second[index];
}
std::vector<SourcePort::AssetVertex>& CNativeGeom::edit(std::size_t sector) {
    auto [at, inserted] = edited_.try_emplace(sector);
    if (inserted) {
        const auto& src = asset_->sectors[sector];
        at->second.reserve(src.VertexCount());
        for (std::size_t i=0;i<src.VertexCount();++i) at->second.push_back(src.Vertex(i));
    }
    return at->second;
}
void CNativeGeom::bounds(){
    sphere_from_bounds_=true;
    city_prop_extent_=-1;
    model_cull_valid_=false;world_sphere_dirty_=true;box_.Reset();bool any=false;
    if(asset_)for(auto s:sectors_)for(std::size_t i=0;i<vertex_count(s);++i){const auto v=vertex(s,i);Mth::Vector p(v.position[0],v.position[1],v.position[2],1);Dbg_Assert(finite(p));box_.AddPoint(p);any=true;}
    if(!any){box_.Set(Mth::Vector(0,0,0,1),Mth::Vector(0,0,0,1));sphere_.Set(0,0,0,0);return;}
    sphere_=(box_.GetMin()+box_.GetMax())*.5f;float radius=0;
    for(auto s:sectors_)for(std::size_t i=0;i<vertex_count(s);++i){const auto v=vertex(s,i);auto delta=Mth::Vector(v.position[0],v.position[1],v.position[2],1)-sphere_;radius=std::max(radius,delta.LengthSqr());}sphere_[3]=std::sqrt(radius);
}
void CNativeGeom::copy_state(CNativeGeom& dest,bool instance)const{
    // A geom owns selected sectors, not the entire scene. Park floor/fence
    // copies must not duplicate every source piece/material for each tile.
    // Source streams are immutable; all writable streams and state below are
    // copied per geom, including non-instance clones used by the real editor.
    dest.asset_=asset_;dest.sectors_=sectors_;dest.cas_=cas_;dest.dictionary_=dictionary_;dest.model_=model_;dest.lights_=lights_;
    dest.transform_=transform_;dest.scale_=scale_;dest.box_=box_;dest.sphere_=sphere_;dest.sphere_from_bounds_=sphere_from_bounds_;dest.model_cull_sphere_=model_cull_sphere_;dest.model_cull_valid_=model_cull_valid_;dest.model_pose_=model_pose_;dest.direct_bones_=direct_bones_;dest.overrides_=overrides_;dest.edited_=edited_;dest.render_colors_=render_colors_;
    dest.color_=color_;dest.color_set_=color_set_;dest.active_=active_;dest.sky_=sky_;dest.billboard_=billboard_;dest.dictionary_only_=false;dest.explicit_uv_=explicit_uv_;dest.visibility_=visibility_;dest.hide_mask_=hide_mask_;
    dest.wibble_=wibble_;dest.custom_wibble_=custom_wibble_;dest.uv_offset_=uv_offset_;dest.m_multipleColorsEnabled=m_multipleColorsEnabled;dest.m_cloned=instance?vINSTANCE:vCOPY;dest.shadow_enabled_=shadow_enabled_;
}
CGeom* CNativeGeom::plat_clone(bool instance,CScene* scene){if(!asset_)return nullptr;auto* clone=new CNativeGeom;copy_state(*clone,instance);clone->scene_=scene;clone->model_=nullptr;return clone;}
CGeom* CNativeGeom::plat_clone(bool instance,CModel* model){if(!asset_||!model)return nullptr;auto* clone=new CNativeGeom;copy_state(*clone,instance);clone->scene_=nullptr;clone->model_=model;return clone;}
bool CNativeGeom::plat_load_geom_data(CMesh* mesh,CModel* model,bool multiple){
    auto* native=dynamic_cast<CNativeMesh*>(mesh);if(!native||!model)return false;std::vector<std::size_t> sectors(native->Asset()->sectors.size());std::iota(sectors.begin(),sectors.end(),0);
    if(!Assign(native->Asset(),std::move(sectors),mesh->GetTextureDictionary(),false,false))return false;cas_=native->CAS();model_=model;lights_=model->GetModelLights();m_multipleColorsEnabled=multiple;
    if(model->GetNumBones())plat_set_bone_matrix_data(model->GetBoneTransforms(),model->GetNumBones());return true;
}
void CNativeGeom::plat_finalize(){bounds();}
uint32 CNativeGeom::plat_get_checksum(){return asset_&&!sectors_.empty()?asset_->sectors[sectors_[0]].checksum:0;}
void CNativeGeom::plat_set_color(Image::RGBA c){bool set=!((c.r==128||c.r==129)&&c.g==128&&c.b==128);if(packed(color_)!=packed(c)||color_set_!=set){color_=c;color_set_=set;invalidate_render_cache();}}
Image::RGBA CNativeGeom::plat_get_color()const{return color_set_?color_:Image::RGBA(128,128,128,128);}
void CNativeGeom::plat_clear_color(){plat_set_color(Image::RGBA(128,128,128,128));}
bool CNativeGeom::material_pass(uint32 name,int pass,uint32& checksum,unsigned& adjusted)const{if(asset_)for(const auto& m:asset_->materials)if(pass_match(m,name,pass,adjusted)){checksum=m.checksum;return true;}return false;}
bool CNativeGeom::plat_set_material_color(uint32 name,int pass,Image::RGBA color){bool changed=false;if(asset_)for(const auto& m:asset_->materials){unsigned p;if(pass_match(m,name,pass,p)){auto& o=overrides_[{m.checksum,p}];if(!o.color_set||packed(o.color)!=packed(color))invalidate_render_cache();o.color=color;o.color_set=true;changed=true;}}return changed;}
Image::RGBA CNativeGeom::plat_get_material_color(uint32 name,int pass){uint32 key;unsigned p;if(!material_pass(name,pass,key,p))return Image::RGBA(0,0,0,0);
    auto at=overrides_.find({key,p});if(at!=overrides_.end()&&at->second.color_set)return at->second.color;
    for(const auto& m:asset_->materials)if(m.checksum==key)return Image::RGBA(byte(m.passes[p].color[0]*255),byte(m.passes[p].color[1]*255),byte(m.passes[p].color[2]*255),128);Dbg_Assert(false);return Image::RGBA(0,0,0,0);
}
void CNativeGeom::plat_set_world_position(const Mth::Vector& p){Dbg_Assert(finite(p));auto next=p;next[3]=1;if(std::memcmp(&next,&transform_[3],sizeof(next))){LeaveStaticWorldPartition();transform_[3]=next;world_sphere_dirty_=true;invalidate_render_cache(false);}}
void CNativeGeom::plat_set_orientation(const Mth::Matrix& m){auto next=transform_;for(unsigned r=0;r<3;++r){Dbg_Assert(finite(m[r]));next[r]=m[r];next[r][3]=0;}plat_set_transform(next);}
void CNativeGeom::plat_set_transform(const Mth::Matrix& m){for(unsigned r=0;r<4;++r)Dbg_Assert(finite(m[r]));if(std::memcmp(&transform_,&m,sizeof(m))){LeaveStaticWorldPartition();transform_=m;world_sphere_dirty_=true;invalidate_render_cache(false);}}
void CNativeGeom::plat_set_scale(const Mth::Vector& scale){Dbg_Assert(finite(scale));if(std::memcmp(&scale_,&scale,sizeof(scale))){LeaveStaticWorldPartition();scale_=scale;world_sphere_dirty_=true;invalidate_render_cache(false);}}
const Mth::CBBox& CNativeGeom::plat_get_bounding_box()const{
    if(model_)return box_;world_box_.Reset();auto matrix=transform_;for(unsigned r=0;r<3;++r)matrix[r]*=scale_[r];
    for(unsigned corner=0;corner<8;++corner){Mth::Vector p;for(unsigned c=0;c<3;++c)p[c]=(corner&(1u<<c))?box_.GetMax()[c]:box_.GetMin()[c];p[3]=1;world_box_.AddPoint(transformed(p,matrix));}return world_box_;
}
bool CNativeGeom::StaticWorldBounds(Mth::CBBox& out)const{
    if(!asset_||model_||!BoneMatrices().empty()||billboard_)return false; // billboards can exceed the source box
    out=plat_get_bounding_box();return true;
}
void CNativeGeom::plat_rotate_y(Mth::ERot90 rotation){
    float angle=rotation==Mth::ROT_90?float(Mth::PI)*.5f:rotation==Mth::ROT_180?float(Mth::PI):rotation==Mth::ROT_270?float(Mth::PI)*1.5f:0;
    if(angle){LeaveStaticWorldPartition();world_sphere_dirty_=true;invalidate_render_cache(false);}float c=std::cos(angle),s=std::sin(angle);for(unsigned r=0;r<3;++r){float x=transform_[r][0],z=transform_[r][2];transform_[r][0]=x*c+z*s;transform_[r][2]=-x*s+z*c;}
}
bool CNativeGeom::plat_render(Mth::Matrix* root,Mth::Matrix* bones,int count){if(!asset_||!root||count<0)return false;plat_set_transform(*root);if(bones)plat_set_bone_matrix_data(bones,count);return true;}
void CNativeGeom::plat_set_bone_matrix_data(Mth::Matrix* bones,int count){
    Dbg_Assert(count>=0&&count<=256&&(!count||bones));
    if(!count){
        if(!BoneMatrices().empty())invalidate_render_cache(false);
        model_pose_.reset();direct_bones_.clear();return;
    }
    if(model_&&source_application_os_is_new_3ds()&&SourcePort::UseOptimizedFramePath(SourcePort::PerformanceRouteStep())){
        if(auto* model=dynamic_cast<CNativeModel*>(model_)){
            auto pose=model->CapturePose(bones,count);
            if(pose==model_pose_)return;
            model_pose_=std::move(pose);direct_bones_.clear();
            LeaveStaticWorldPartition();invalidate_render_cache(false);return;
        }
    }
    if(!model_pose_&&direct_bones_.size()==std::size_t(count)&&
       std::memcmp(direct_bones_.data(),bones,count*sizeof(*bones))==0)return;
    for(int i=0;i<count;++i)for(unsigned r=0;r<4;++r)Dbg_Assert(finite(bones[i][r]));
    direct_bones_.assign(bones,bones+count);model_pose_.reset();
    LeaveStaticWorldPartition();invalidate_render_cache(false);
}
bool CNativeGeom::plat_enable_shadow(bool on){shadow_enabled_=on;return true;}
int CNativeGeom::plat_get_num_render_verts(){std::size_t total=0;if(asset_)for(auto s:sectors_)total+=vertex_count(s);Dbg_Assert(total<=INT_MAX);return total;}
int CNativeGeom::plat_get_num_render_base_polys(){std::size_t total=0;if(asset_)for(auto s:sectors_)for(std::size_t m=0;m<asset_->sectors[s].meshes.size();++m){std::vector<uint16> triangles;Dbg_Assert(SourcePort::MeshTriangles(*asset_,s,m,0,&cas_,hide_mask_,triangles));total+=triangles.size()/3;}Dbg_Assert(total<=INT_MAX);return total;}
int CNativeGeom::plat_get_num_render_polys(){std::size_t total=0;if(asset_)for(auto s:sectors_)for(std::size_t mi=0;mi<asset_->sectors[s].meshes.size();++mi){const auto& mesh=asset_->sectors[s].meshes[mi];std::vector<uint16> triangles;Dbg_Assert(SourcePort::MeshTriangles(*asset_,s,mi,0,&cas_,hide_mask_,triangles));for(const auto& material:asset_->materials)if(material.checksum==mesh.material)total+=triangles.size()/3*material.passes.size();}Dbg_Assert(total<=INT_MAX);return total;}
void CNativeGeom::plat_get_render_verts(Mth::Vector* out){Dbg_Assert(out||!plat_get_num_render_verts());auto matrix=transform_;for(unsigned r=0;r<3;++r)matrix[r]*=scale_[r];if(asset_)for(auto s:sectors_)for(std::size_t i=0;i<vertex_count(s);++i){const auto v=vertex(s,i);auto p=Mth::Vector(v.position[0],v.position[1],v.position[2],1);*out++=model_?p:transformed(p,matrix);}}
void CNativeGeom::plat_get_render_colors(Image::RGBA* out){
    Dbg_Assert(out||!plat_get_num_render_verts());if(asset_)for(auto s:sectors_){auto at=render_colors_.find(s);
        if(at!=render_colors_.end())for(auto c:at->second)*out++=unpacked(c);
        else for(std::size_t i=0;i<vertex_count(s);++i)*out++=unpacked(vertex(s,i).color);
    }
}
void CNativeGeom::plat_set_render_verts(Mth::Vector* in){LeaveStaticWorldPartition();invalidate_render_cache();Dbg_Assert(in||!plat_get_num_render_verts());auto inverse=transform_;if(!model_){for(unsigned r=0;r<3;++r)inverse[r]*=scale_[r];Dbg_Assert(std::abs(inverse.Determinant())>1e-12f);inverse.Invert();}
    if(asset_)for(auto s:sectors_)for(auto& v:edit(s)){Dbg_Assert(finite(*in));auto p=model_?*in:transformed(*in,inverse);v.position={p[0],p[1],p[2]};++in;}bounds();}
void CNativeGeom::plat_set_render_colors(Image::RGBA* in){
    invalidate_render_cache();Dbg_Assert(in||!plat_get_num_render_verts());if(asset_)for(auto s:sectors_){auto& colors=render_colors_[s];colors.resize(vertex_count(s));for(auto& c:colors)c=packed(*in++);}
}
void CNativeGeom::plat_set_uv_wibble_params(float uv,float ua,float uf,float up,float vv,float va,float vf,float vp){invalidate_render_cache();wibble_={uv,vv,uf,vf,ua,va,up,vp};custom_wibble_=true;}
bool CNativeGeom::plat_set_uv_wibble_offsets(uint32 name,int pass,float u,float v){uint32 key;unsigned p;if(!material_pass(name,pass,key,p))return false;Mth::Matrix mat;mat.Ident();mat[3].Set(u,v,0,1);return plat_set_uv_matrix(name,pass,mat);}
bool CNativeGeom::plat_set_uv_matrix(uint32 name,int pass,const Mth::Matrix& matrix){for(unsigned r=0;r<4;++r)Dbg_Assert(finite(matrix[r]));bool changed=false;
    if(asset_)for(const auto& m:asset_->materials){unsigned p;if(pass_match(m,name,pass,p)){auto& o=overrides_[{m.checksum,p}];if(!o.uv_set||std::memcmp(&o.uv,&matrix,sizeof(matrix)))invalidate_render_cache();o.uv=matrix;o.uv_set=true;changed=true;}}return changed;}
bool CNativeGeom::plat_allocate_uv_matrix_params(uint32 name,int pass){uint32 key;unsigned p;if(!material_pass(name,pass,key,p))return false;overrides_.try_emplace({key,p});return true;}
std::vector<CNativeGeom::RenderMeshCache>& CNativeGeom::render_sector(std::size_t slot)const{
    if(render_meshes_.empty()){
        SourcePort::RootHeapScope storage;
        render_meshes_.resize(sectors_.size());
    }
    auto& meshes=render_meshes_[slot];
    if(meshes.empty()){
        SourcePort::RootHeapScope storage;
        meshes.resize(asset_->sectors[sectors_[slot]].meshes.size());
    }
    return meshes;
}
const CNativeGeom::RenderMeshCache& CNativeGeom::render_mesh(RenderMeshCache& cache,std::size_t sector,std::size_t mesh,unsigned lod,bool generated,bool gameplay_skin)const{
    if(cache.valid&&cache.mask==hide_mask_&&cache.lod==lod)return cache;
    // Derived immutable topology survives the temporary frame/model context.
    SourcePort::RootHeapScope storage;
    const auto& source=asset_->sectors[sector];
    if (generated)
        Dbg_Assert(SourcePort::BuildDistantRenderTopology(source,source.meshes[mesh],cache.topology));
    else
        Dbg_Assert(SourcePort::BuildRenderTopology(*asset_,sector,mesh,lod,&cas_,hide_mask_,cache.topology));
    if(gameplay_skin)SourcePort::SimplifyGameplaySkin(source,cache.topology);
    const auto checksum=source.meshes[mesh].material;
    if(static_materials_)cache.material=static_materials_->MaterialIndex(checksum);
    else {
        cache.material=0;
        while(cache.material<asset_->materials.size()&&asset_->materials[cache.material].checksum!=checksum)
            ++cache.material;
    }
    Dbg_Assert(cache.material<asset_->materials.size());
    const auto passes=SourcePort::FindWorldBasePasses(asset_->materials[cache.material]);
    // Create-a-Deck uses two accessory slots on the base graphic itself.
    // Retail exports these textures with the environment flag also set.
    const auto& material=asset_->materials[cache.material];
    cache.overlay_passes=0;
    for(unsigned i=1;i<material.passes.size()&&i<4;++i){
        unsigned mapped;
        if(pass_match(material,0x55c293b6,i,mapped)&&mapped==i&&
           is_deck_sticker_slot(material.passes[i].texture))
            cache.overlay_passes|=1u<<i;
    }
    // The call button is a decal on a second sheet of brushed metal.
    // Its own UV set aligns the lettering with the elevator controls.
    if(material.passes.size()==3 && material.passes[0].texture==0x9753b1a0 &&
       solid_reflection_surface(material) && material.passes[2].texture==0x28fefff8 &&
       material.passes[2].flags==0x24 && (material.passes[2].alpha&0xffffff)==5)
        cache.overlay_passes|=1u<<2;
    cache.base_pass=passes.diffuse;cache.blend_pass=passes.opacity;
    cache.indices=std::make_shared<const std::vector<uint16>>(std::move(cache.topology.triangles));
    cache.mask=hide_mask_;cache.lod=lod;cache.valid=true;cache.facing_ready=false;cache.attributes_revision=cache.bounds_revision=0;cache.retained.reset();
    cache.skin.reset();
    cache.unsupported_palette=0;cache.ped_pose.reset();
    return cache;
}
CNativeGeom::SkinMesh::~SkinMesh(){if(gpu)Dbg_Assert(source_graphics_mesh_delete(gpu)==0);}
bool CNativeGeom::SkinMesh::UpdatePose(const std::vector<Mth::Matrix>& matrices)const{
    std::array<float,SOURCE_SKIN_BONES*12> rows{};
    for(std::size_t bone=0;bone<bones.size();++bone){
        if(bones[bone]>=matrices.size())return false;
        const auto& matrix=matrices[bones[bone]];
        for(unsigned row=0;row<3;++row)
            for(unsigned column=0;column<4;++column)
                rows[bone*12+row*4+column]=matrix[column][row];
    }
    return source_graphics_skin_pose(gpu,rows.data(),bones.size())==0;
}
void CNativeGeom::ReleaseColdRenderTopology(uint32 stamp) {
    if (!static_materials_ || render_meshes_.empty() || stamp-last_world_gather_<=2) return;
    decltype(render_meshes_)().swap(render_meshes_);
}
float CNativeGeom::city_prop_extent()const{
    // A sector is the original scene object. Classify its complete geometry
    // before any mesh is dropped, including parts with different materials.
    if(sky_||billboard_||sectors_.size()!=1)return 0;
    const auto& sector=asset_->sectors[sectors_.front()];
    if(sector.bone>=0||(sector.flags&16)||sector.meshes.empty())return 0;
    const auto half=(box_.GetMax()-box_.GetMin())*.5f;
    const float wide=std::max(half[X],half[Z]),narrow=std::min(half[X],half[Z]);
    if(wide>360||half[Y]>144||half[Y]<std::max(3.f,.18f*wide)||
       narrow<std::max(3.f,.16f*wide)||half[Y]>2.f*wide)return 0;
    for(const auto& mesh:sector.meshes){
        const auto index=static_materials_->MaterialIndex(mesh.material);
        if(index>=asset_->materials.size())return 0;
        const auto& material=asset_->materials[index];
        if(material.grass||material.alpha_cutoff>1||!material.color_wibble.empty()||material.passes.empty())return 0;
        for(const auto& pass:material.passes)
            if((pass.alpha&0xffffff)||(pass.flags&(1|2|8|(1<<11))))return 0;
    }
    // A sloped ramp or diagonal wall can have a deep axis-aligned box.
    // Find a plane through three spread-out vertices and keep thin sheets.
    if(sector.VertexCount()<4)return 0;
    const auto origin=authored_position(sector.Position(0));
    Mth::Vector edge(0,0,0,0),normal(0,0,0,0);
    float edge_sq=0,normal_sq=0;
    for(std::size_t i=1;i<sector.VertexCount();++i){
        auto offset=authored_position(sector.Position(i))-origin;
        const auto length=offset.LengthSqr();
        if(length>edge_sq){edge=offset;edge_sq=length;}
    }
    for(std::size_t i=1;i<sector.VertexCount();++i){
        auto cross=Mth::CrossProduct(edge,authored_position(sector.Position(i))-origin);
        const auto length=cross.LengthSqr();
        if(length>normal_sq){normal=cross;normal_sq=length;}
    }
    float low=0,high=0;
    for(std::size_t i=1;i<sector.VertexCount();++i){
        const auto distance=Mth::DotProduct(normal,authored_position(sector.Position(i))-origin);
        low=std::min(low,distance);high=std::max(high,distance);
    }
    const float extent=std::max(wide,half[Y]);
    const float thickness=std::max(6.f,.18f*extent);
    return (high-low)*(high-low)>normal_sq*thickness*thickness?extent:0.f;
}
void CNativeGeom::Gather(const SourcePort::WorldView& view,uint32 time,std::vector<SourcePort::WorldBatch>& out,bool snapshot,bool raw_colors,bool projection_receiver,unsigned frustum_mask)const{
    const auto& bones_=BoneMatrices();
    if(dictionary_only_||!asset_||(!snapshot&&(!active_||view.viewport>=32||!(visibility_&(1u<<view.viewport)))))return;
    // Keep the sector registered for QB and collision, including after a script reactivates it.
    if(sectors_.size()==1 && asset_->sectors[sectors_.front()].scenery_effect)return;
    if(world_sphere_dirty_){
        render_transform_=transform_;
        for(unsigned r=0;r<3;++r)render_transform_[r]*=scale_[r];
        static constexpr float identity[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
        render_transform_identity_=std::memcmp(&render_transform_[0][0],identity,sizeof(identity))==0;
        world_sphere_scale_=SourcePort::ConservativeSphereScale(render_transform_);
        world_sphere_=transformed(Mth::Vector(sphere_[0],sphere_[1],sphere_[2],1),render_transform_);
        world_sphere_[3]=sphere_[3]*world_sphere_scale_;
        world_sphere_dirty_=false;
    }
    const auto& model_matrix=render_transform_;
    const auto& center=world_sphere_;
    const bool local_player=!snapshot&&view.local_model&&model_==view.local_model;
    if(!local_player&&!snapshot&&!sky_&&!billboard_&&(bones_.empty()||model_cull_valid_)){
        auto bound=center;if(!bones_.empty()){
            bound=transformed(Mth::Vector(model_cull_sphere_[0],model_cull_sphere_[1],model_cull_sphere_[2],1),model_matrix);
            // Keep original per-mesh distance/transparent order separate.
            bound[3]=model_cull_sphere_[3]*world_sphere_scale_;
            // A model sphere encloses the bind pose, not necessarily the
            // current bone positions. It is not proof that a wall hides
            // the entire actor. Keep the inexpensive frustum check for
            // distant actors, but leave near poses to GPU clipping/depth.
            constexpr float near_actor_margin=512.f;
            const float near_actor_range=bound[3]+near_actor_margin;
            if((bound-view.position).LengthSqr()>near_actor_range*near_actor_range&&
               !SourcePort::SphereInFrustum(view,bound))return;
        }else if(view.refine_static_leaves && partitioned_ && !frustum_mask && sphere_from_bounds_ && view.occlusion){
            // The cell contains the authored sphere center and is inside all
            // frustum planes. A replacement sphere still takes the full test.
            if(view.occluder_mask && view.occlusion->HidesSphere(bound,bound[3],view.occluder_mask))return;
        }else if(!SourcePort::SphereVisible(view,bound))return;
    }
    const bool performance=view.performance&&!snapshot;
    // Reject distant level sectors before expanding their triangle strips or
    // uploading textures. Test their nearest edge so long ramps remain visible.
    if (performance && view.static_world_radius > 0 && static_materials_ && !sky_ &&
        !model_ && !projection_receiver && render_transform_identity_ && bones_.empty() && !(view.preserve_arena_landmarks&&distant_landmark(box_))) {
        const auto& low=box_.GetMin();
        const auto& high=box_.GetMax();
        const float dx=std::max({0.f,low[X]-view.position[X],view.position[X]-high[X]});
        const float dz=std::max({0.f,low[Z]-view.position[Z],view.position[Z]-high[Z]});
        const float radius=view.static_world_radius+std::abs(view.stereo_eye_shift);
        if(dx*dx+dz*dz>radius*radius) {
            if(view.detail_stats)++view.detail_stats->distant_prop_sectors;
            return;
        }
    }
    if(performance&&view.cull_static_meshes&&view.city_prop_radius>0&&!dictionary_only_&&static_materials_&&render_transform_identity_&&
       !model_&&bones_.empty()&&!raw_colors&&!projection_receiver&&m_cloned==vORIGINAL&&
       edited_.empty()&&render_colors_.empty()&&overrides_.empty()&&!hide_mask_&&!custom_wibble_&&!explicit_uv_){
        if(city_prop_extent_<0)city_prop_extent_=city_prop_extent();
        if(city_prop_extent_>0){
            const auto& low=box_.GetMin();
            const auto& high=box_.GetMax();
            const float dx=std::max({0.f,low[X]-view.position[X],view.position[X]-high[X]});
            const float dz=std::max({0.f,low[Z]-view.position[Z],view.position[Z]-high[Z]});
            const float radius=std::max(view.city_prop_radius,16.f*city_prop_extent_)+std::abs(view.stereo_eye_shift);
            if(dx*dx+dz*dz>radius*radius){
                if(view.detail_stats)++view.detail_stats->distant_prop_sectors;
                return;
            }
        }
    }
    if (performance && view.reclaim_cold_topology) last_world_gather_=world_gather_stamp;
    // GatherNativeWorld prepares the view once. Direct geometry callers,
    // including tools, may supply an unprepared view.
    const SourcePort::WorldView* bounds_view=&view;
    std::optional<SourcePort::WorldView> prepared;
    if(performance&&view.cull_static_meshes&&!sky_&&!projection_receiver&&
        (view.frustum_hfov!=view.hfov||view.frustum_aspect!=view.aspect)){
        prepared.emplace(view);
        SourcePort::PrepareWorldFrustum(*prepared);
        bounds_view=&*prepared;
    }
    std::optional<SourcePort::LightingFrame> lighting;
    if(!performance)lighting.emplace(SourcePort::NativeLighting(lights_,transform_[3],model_!=nullptr));
    std::shared_ptr<const SourcePort::LightingFrame> shard_lights;
    const float distance_squared=(center-view.position).LengthSqr();
    bool far_ped=false;
    if(view.distant_ped_poses&&performance&&view.local_model&&model_&&!local_player&&asset_->pedestrian_skin&&!bones_.empty()&&!projection_receiver&&!raw_colors){
        // Modular pedestrian outfits do not all get an explicit model sphere.
        // Use a common full-height bound about the actor root in that case.
        const auto bound=model_cull_valid_?transformed(Mth::Vector(model_cull_sphere_[X],model_cull_sphere_[Y],model_cull_sphere_[Z],1),model_matrix):model_matrix[W];
        const float radius=(model_cull_valid_?model_cull_sphere_[W]:96.f)*world_sphere_scale_;
        if(view.detail_stats)++view.detail_stats->ped_visible;
        const float depth=-Mth::DotProduct(bound-view.position,view.axes[Z])-radius;
        // 36 pixels of projected diameter at native resolution. Near actors always use the live pose.
        far_ped=radius>0&&depth>800&&radius*view.width<36.f*depth*std::tan(view.hfov*float(Mth::PI)/360.f);
        if(far_ped&&view.detail_stats)++view.detail_stats->ped_far;
    }
    for(std::size_t slot=0;slot<sectors_.size();++slot){const auto s=sectors_[slot];const auto& original=asset_->sectors[s];
        auto colors=render_colors_.find(s);const auto* diffuse=colors==render_colors_.end()?nullptr:&colors->second;
        // Only edited sectors need a copy of the original streams.
        std::optional<SourcePort::SectorAsset> edited;const SourcePort::SectorAsset* source=&original;
        if(auto at=edited_.find(s);at!=edited_.end()){edited.emplace(original);edited->vertices=at->second;source=&*edited;}
        const bool cull_mesh=performance&&!local_player&&view.cull_static_meshes&&bones_.empty()&&source->bone<0&&
            !(source->flags&(16|0x00800000))&&!sky_&&!projection_receiver;
        const bool original_scenery=cull_mesh&&static_materials_&&render_transform_identity_&&!model_&&
            !raw_colors&&source==&original&&m_cloned==vORIGINAL;
        const bool use_authored_positions=view.direct_static_positions&&cull_mesh&&static_materials_&&
            render_transform_identity_&&!model_&&!raw_colors&&source==&original;
        const bool limit_props=original_scenery&&view.static_prop_radius>0&&(view.small_props_only||asset_->sectors.size()>=1100);
        auto* meshes=performance&&!source->meshes.empty()?render_sector(slot).data():nullptr;
        SourcePort::SharedSkinPositions* reuse=nullptr;
        if(meshes && view.reuse_pose_vertices && local_player && source==&original && (source->flags&16) && !bones_.empty()){
            auto& cache=meshes[0].shared_positions;
            if(!cache){
                SourcePort::RootHeapScope storage;
                cache=std::make_unique<SourcePort::SharedSkinPositions>(original);
            }
            if(cache->Active()){reuse=cache.get();reuse->Begin();}
        }
        for(std::size_t mi=0;mi<source->meshes.size();++mi){const auto& mesh=source->meshes[mi];std::vector<uint16> original_triangles;
            const RenderMeshCache* cache=nullptr;const SourcePort::MaterialAsset* material=nullptr;
            if(performance){unsigned lod=!local_player&&distance_squared>1500.f*1500.f?mesh.lods.size()-1:0;cache=&render_mesh(meshes[mi],s,mi,lod);material=&asset_->materials[cache->material];}
            else{Dbg_Assert(SourcePort::MeshTriangles(*asset_,s,mi,0,&cas_,hide_mask_,original_triangles));
                for(const auto& m:asset_->materials)if(m.checksum==mesh.material){material=&m;break;}}
            if(local_player&&(view.city_stereo_fast || view.old_player_lod)&&(source->flags&16)&&!bones_.empty()&&cache&&source==&original){
                auto& reduced=meshes[mi].distant;
                if(!reduced){SourcePort::RootHeapScope storage;reduced=std::make_unique<RenderMeshCache>();}
                cache=&render_mesh(*reduced,s,mi,0,false,true);
            }
            const auto& full_triangles=cache?*cache->indices:original_triangles;
            if(full_triangles.empty())continue;
            Dbg_Assert(material);
            const auto* full_cache=cache;
            // A sector may span a whole facade/street. Cull each actual static
            // mesh against bounds derived from its referenced LOD vertices,
            // before posing/expanding them. Never apply bind-pose bounds to a
            // skeleton, billboard, snapshot or Shatter/projection source.
            if(cull_mesh){
                if(cache->bounds_revision!=render_revision_){
                    auto first=cache->topology.vertices.front();
                    auto world_position=[&](uint16 index){
                        if(use_authored_positions)return authored_position(source->Position(index));
                        const auto& p=source->Position(index);
                        return transformed(Mth::Vector(p[0],p[1],p[2],1),model_matrix);
                    };
                    auto low=world_position(first);
                    auto high=low;
                    for(auto index:cache->topology.vertices){
                        auto p=world_position(index);
                        for(unsigned c=0;c<3;++c){
                            low[c]=std::min(low[c],p[c]);
                            high[c]=std::max(high[c],p[c]);
                        }
                    }
                    cache->box_center=(low+high)*.5f;
                    cache->box_half_size=(high-low)*.5f;
                    cache->bounds_revision=render_revision_;
                }
                if(!SourcePort::BoxInFrustum(*bounds_view,cache->box_center,cache->box_half_size,frustum_mask))continue;
                const auto& half=cache->box_half_size;
                const auto opacity=cache->blend_pass;
                const auto* pass=opacity<material->passes.size()?&material->passes[opacity]:nullptr;
                // Preserve the source silhouette of blended foliage and
                // tall thin scenery (e.g. a palm trunk in a separate mesh).
                const bool silhouette=original_scenery&&
                    ((pass&&((pass->alpha&0xffffff)==5||material->alpha_cutoff>1))||
                     (half[Y]>72.f&&half[Y]>2.f*std::max(half[X],half[Z])));
                if(limit_props&&!silhouette&&pass&&!(pass->alpha&0xffffff)&&
                   !(pass->flags&(1|2|(1<<11)))&&!material->grass&&
                   SourcePort::OutsideStaticPropRadius(view,cache->box_center,half)){
                    if(view.detail_stats){
                        ++view.detail_stats->range_meshes;
                        view.detail_stats->range_triangles+=full_triangles.size()/3;
                    }
                    continue;
                }
                if(!model_&&!silhouette&&(!view.small_props_only||(half[Y]<56.f&&std::max(half[X],half[Z])<92.f))&&SourcePort::TinyDistantMesh(*bounds_view,cache->box_center,half)){
                    if(view.detail_stats){
                        ++view.detail_stats->meshes;
                        view.detail_stats->triangles+=full_triangles.size()/3;
                    }
                    continue;
                }
                if(view.occluder_mask&&view.occlusion&&view.occlusion->HidesMesh(cache->box_center,cache->box_half_size,full_triangles.size()/3,view.occluder_mask))continue;
                if(!silhouette&&view.static_lod_pixels>0&&!mesh.distant_triangles.empty()&&source==&original&&
                    m_cloned==vORIGINAL&&!model_&&!hide_mask_&&!diffuse&&!raw_colors&&
                    !custom_wibble_&&!explicit_uv_&&overrides_.empty()){
                    const auto& forward=view.axes[Z];
                    const auto delta=cache->box_center-view.position;
                    const auto& half=cache->box_half_size;
                    const float near=-Mth::DotProduct(delta,forward)-
                        (std::abs(forward[X])*half[X]+std::abs(forward[Y])*half[Y]+std::abs(forward[Z])*half[Z]);
                    // Test the nearest point of the full mesh. The projected
                    // bake error is a selection heuristic; stereo shares the LOD.
                    const float error=std::max(1.5f,mesh.distant_error)*world_sphere_scale_;
                    const float distance=std::max(view.static_lod_min_distance,bounds_view->static_lod_depth_per_unit*error);
                    if(near>=distance){
                        auto& distant=meshes[mi].distant;
                        if(!distant){
                            SourcePort::RootHeapScope storage;
                            distant=std::make_unique<RenderMeshCache>();
                        }
                        cache=&render_mesh(*distant,s,mi,0,true);
                        if(view.detail_stats){
                            ++view.detail_stats->lod_meshes;
                            view.detail_stats->lod_triangles_saved+=(full_triangles.size()-cache->indices->size())/3;
                        }
                    }
                }
            }
            const auto& triangles=cache?*cache->indices:original_triangles;
            const unsigned blend_pass=cache?cache->blend_pass:0;
            if(performance&&blend_pass==material->passes.size())continue; // reflection-only layer contributes nothing without reflections
            const unsigned base_pass=cache&&!raw_colors&&!projection_receiver?cache->base_pass:blend_pass;
            const auto overlay_passes=cache&&!raw_colors&&!projection_receiver?cache->overlay_passes:0;
            bool billboard=source->flags&0x00800000;
            bool need_normals=!performance||billboard;
            bool immutable=performance&&!overlay_passes&&view.retain_static&&!model_&&bones_.empty()&&!billboard&&!raw_colors&&!projection_receiver&&!custom_wibble_
                &&(material->frozen_scenery||material->color_wibble.empty());
            if(immutable)immutable=!(material->passes[base_pass].flags&(1|2|(1<<11)));
            if(original_scenery&&immutable&&view.backface_static&&bounds_view->frustum_positive_winding&&
               SourcePort::WorldMaterialCullsBackfaces(*material,material->passes[blend_pass].alpha&0xffffff)){
                const auto& bounds_center=full_cache->box_center;
                if(!cache->facing_ready){
                    cache->facing=SourcePort::BuildStaticFacingBounds(original,cache->topology,*cache->indices,bounds_center);
                    cache->facing_ready=true;
                }
                if(SourcePort::FacesAwayFromBothEyes(cache->facing,*bounds_view,bounds_center,full_cache->box_half_size)){
                    if(view.detail_stats){
                        ++view.detail_stats->backface_meshes;
                        view.detail_stats->backface_triangles+=triangles.size()/3;
                    }
                    continue;
                }
            }
            uint32 distant_color=0;
            if(view.old_player_lod && original_scenery && immutable && !material->sorted &&
               material->alpha_cutoff<=1 && material->passes.size()==1 && !material->grass &&
               !(material->passes.front().alpha&0xffffff) && overrides_.empty() && cache->bounds_revision==render_revision_){
                const auto& half=cache->box_half_size;
                const auto& pass=material->passes.front();
                const bool small=std::max({half[X],half[Y],half[Z]})<80.f;
                // Use the same choice for both eyes. Keep close props textured
                // and allow a margin when crossing the distance threshold.
                auto last=cache->retained.lock();
                const float limit=last && last->distant_color?750.f:950.f;
                if(dictionary_ && pass.texture && (cache!=full_cache ||
                    (small && (cache->box_center-view.position).LengthSqr()>limit*limit))){
                    if(auto* tex=SourcePort::world_texture(view,*dictionary_,pass.texture))
                        distant_color=tex->DistantColor();
                }
            }
            if(immutable&&cache->revision==render_revision_){if(auto saved=cache->retained.lock();saved && saved->distant_color==distant_color){
                if(view.detail_stats)++view.detail_stats->retained_hits;
                const auto group=out.size();
                auto prepare_batch=[&](SourcePort::WorldBatch& batch){
                    static_cast<SourcePort::WorldDrawState&>(batch)=*saved;
                    // A material survives a dictionary edit independently of its sampler.
                    batch.texture=nullptr;
                    const auto texture=material->passes[base_pass].texture;
                    if(texture && !saved->texture && view.detail_stats)++view.detail_stats->flat_meshes;
                    if(texture && saved->texture){
                        Dbg_Assert(dictionary_);
                        batch.texture=SourcePort::world_texture(view,*dictionary_,texture);
                        Dbg_Assert(batch.texture);
                    }
                    batch.indices=saved->indices;batch.retained=std::move(saved);
                    batch.distance=distance_squared;batch.group=group;
                    if(view.group_static_materials&&static_materials_&&batch.opaque)static_materials_->Bind(batch,cache->material);
                };
                if(view.refine_static_leaves)prepare_batch(out.emplace_back());
                else{
                    SourcePort::WorldBatch batch;
                    prepare_batch(batch);
                    out.push_back(std::move(batch));
                }
                continue;}}

            std::vector<Mth::Vector> positions,normals;
            auto pose_on_cpu=[&]{
                const bool measure_pose=view.profile_pose&&view.detail_stats&&!bones_.empty();
                const auto pose_started=measure_pose?source_clock_now():0;
                const bool posed=cache?SourcePort::PoseRenderTopology(*source,cache->topology,bones_.empty()?nullptr:bones_.data(),bones_.size(),positions,need_normals?&normals:nullptr)
                    :SourcePort::PoseSector(*source,bones_.empty()?nullptr:bones_.data(),bones_.size(),positions,&normals,&mesh);
                if(measure_pose)view.detail_stats->pose_ticks+=source_clock_now()-pose_started;
                Dbg_MsgAssert(posed,("Original geometry has no valid skeleton pose %08x",source->checksum));
                if(billboard){
                    face_billboard(*source,triangles,view,model_matrix,positions,normals);
                    if(performance&&view.cull_static_meshes&&!sky_&&!projection_receiver){
                        // The authored box describes the unrotated quad.
                        // Test the actual posed geometry against both eyes.
                        auto low=positions[triangles.front()];
                        auto high=low;
                        for(auto index:triangles){
                            for(unsigned axis=0;axis<3;++axis){
                                low[axis]=std::min(low[axis],positions[index][axis]);
                                high[axis]=std::max(high[axis],positions[index][axis]);
                            }
                        }
                        if(!SourcePort::BoxInFrustum(*bounds_view,(low+high)*.5f,(high-low)*.5f,0x3f))return false;
                    }
                }
                else if(performance){for(auto& p:positions)p=transformed(p,model_matrix);
                    if(need_normals)for(auto& n:normals){n=transformed(n,model_matrix);n[3]=0;}}
                return true;
            };
            // Position-only skin uses the original CAS topology and material.
            // Effects that need posed normals still use the CPU.
            const bool skin_pilot=asset_->moscow_guard_skin||(local_player&&view.test_player_skin)||
                (view.city_stereo_fast&&asset_->pedestrian_skin&&model_);
            bool use_skin=view.gpu_skin&&!overlay_passes&&performance&&skin_pilot&&
                cache&&source==&original&&!billboard&&!need_normals&&
                !raw_colors&&!projection_receiver&&view.index_skinned&&!bones_.empty()&&
                (source->flags&16)&&!custom_wibble_&&!explicit_uv_&&material->color_wibble.empty()&&
                !(material->passes[base_pass].flags&(1|2|8|(1<<11)));
            if(use_skin&&cache->unsupported_palette==bones_.size())use_skin=false;
            if(use_skin&&!use_authored_positions&&cache->skin&&cache->skin->attributes_revision!=attributes_revision_)
                cache->skin.reset();
            bool cpu_pose_ready=false;
            SourcePort::MaterialAsset custom;const auto* current=material;
            if(custom_wibble_){custom=*material;for(auto& p:custom.passes){p.flags|=1;p.uv_wibble=wibble_;}current=&custom;}
            bool animated=!performance||!current->color_wibble.empty()||custom_wibble_;
            if(performance)animated|=bool(current->passes[base_pass].flags&(1|2|(1<<11)));
            if(current->frozen_scenery)animated=false;
            SourcePort::MaterialFrame animation;if(animated)Dbg_Assert(SourcePort::EvaluateMaterial(*current,time,time,animation));
            std::size_t group=out.size();
            for(unsigned pi=performance?base_pass:0;pi<material->passes.size();++pi){const auto& pass=material->passes[pi];SourcePort::WorldBatch batch;
                if(projection_receiver&&pi)break;
                const bool overlay=performance&&pi!=base_pass;
                if(overlay){
                    if(!overlay_passes)break;
                    if(!(overlay_passes&(1u<<pi))||!dictionary_)continue;
                    const auto* slot=SourcePort::world_texture(view,*dictionary_,pass.texture);
                    if(!slot||(is_deck_sticker_slot(pass.texture)&&!slot->Asset().image))continue;
                }
                SourcePort::SetWorldMaterialState(batch,*material,pi,base_pass,blend_pass,sky_,performance,mesh.flags&2);
                if(overlay){batch.pass=pi;batch.opaque=false;batch.environment=false;}
                batch.group=group;batch.distance=distance_squared;
                const bool shard_lighting=snapshot&&!raw_colors&&model_&&(source->flags&4);
                if(shard_lighting){
                    if(!shard_lights)shard_lights=std::make_shared<const SourcePort::LightingFrame>(*lighting);
                    batch.shard_lights=shard_lights;
                }
                const bool index_static=immutable&&!sky_&&source_graphics_stream_bytes(cache->topology.vertices.size(),triangles.size())<triangles.size()*sizeof(SourceRenderVertex);
                const bool indexed=index_static||(performance&&!(local_player&&view.local_arrays)&&view.index_skinned&&(model_||!bones_.empty())&&!billboard&&!raw_colors&&!projection_receiver);
                if(indexed)batch.indices=cache->indices;
                if(use_skin&&cache->skin){
                    if(cache->skin->UpdatePose(bones_)){
                        batch.skin_mesh=cache->skin->gpu;
                        batch.skin_transform=&model_matrix;
                        batch.skin_vertices=cache->skin->uploaded_vertices;
                        batch.indices=cache->indices;
                        if(view.detail_stats)++view.detail_stats->skin_draws;
                    }else{
                        use_skin=false;
                    }
                }
                const bool expand_pose=local_player&&view.local_arrays&&!indexed&&cache&&!billboard;
                const auto entries=batch.skin_mesh?0:(indexed||expand_pose?cache->topology.vertices.size():triangles.size());
                bool retain_cpu=false;
                if(immutable){
                    const auto available=Mem::Manager::sHandle().BottomUpHeap()->ParentRegion()->MemAvailable();
                    constexpr std::size_t reserve=4*1024*1024;
                    // New 3DS has room in the original root arena but little
                    // spare libc heap during frontend reloads. Retained CPU
                    // payloads use root blocks only while a 4 MiB gameplay
                    // reserve remains. Low-memory and Old 3DS keep the old
                    // non-retaining/native path. No named model heap owns it.
                    if(view.mesh_retention&&available>0&&std::size_t(available)>reserve&&
                       entries<=(std::size_t(available)-reserve)/sizeof(SourceRenderVertex)){
                        SourcePort::RootHeapScope storage;
                        batch.vertices.reserve(entries);
                        retain_cpu=SourcePort::IsEngineAddress(batch.vertices.data());
                    }else{
                        SourcePort::NativeHeapScope storage;
                        batch.vertices.reserve(entries);
                    }
                }else batch.vertices.reserve(entries);
                if(shard_lighting)batch.shard_lighting.reserve(triangles.size());
                auto texture=animated?animation.passes[pi].texture:pass.texture;if(texture&&!projection_receiver){Dbg_Assert(dictionary_);batch.texture=SourcePort::world_texture(view,*dictionary_,texture);Dbg_MsgAssert(batch.texture,("Original material %08x missing texture %08x",material->checksum,texture));}
                if(distant_color){batch.texture=nullptr;if(view.detail_stats)++view.detail_stats->flat_meshes;}
                auto override=overrides_.find({material->checksum,pi});const Override* change=override==overrides_.end()?nullptr:&override->second;
                const bool attributes=performance&&!overlay&&(model_||!bones_.empty())&&!billboard&&!animated&&!(pass.flags&8)&&!raw_colors&&!projection_receiver;
                const bool reuse_attributes=attributes&&cache->attributes_revision==attributes_revision_&&cache->attributes.size()==cache->topology.vertices.size();
                if(attributes&&!reuse_attributes){SourcePort::RootHeapScope storage;cache->attributes.resize(cache->topology.vertices.size());}
                const bool packed_pose=reuse_attributes&&!use_skin&&(source->flags&16)&&(indexed||expand_pose);
                if(packed_pose){
                    const bool timed=view.profile_pose&&view.detail_stats;
                    const auto start=timed?source_clock_now():0;
                    if(far_ped&&source==&original&&!cache->ped_pose)
                        cache->ped_pose=SourcePort::DistantPedPose::Create(cache->topology.vertices.size());
                    if((!far_ped||source!=&original)&&cache->ped_pose)cache->ped_pose.reset();
                    if(cache->ped_pose){
                        bool sampled=false;
                        Dbg_Assert(cache->ped_pose->Draw(*source,cache->topology,bones_,model_matrix,cache->attributes,time,batch.vertices,sampled));
                        if(view.detail_stats){if(sampled)++view.detail_stats->ped_samples;else ++view.detail_stats->ped_replays;}
                    }else{
                        const auto before=reuse?reuse->hits:0;
                        Dbg_Assert(SourcePort::PoseSkinnedDraw(*source,cache->topology,bones_,model_matrix,cache->attributes,batch.vertices,reuse));
                        if(reuse&&view.detail_stats)view.detail_stats->shared_positions+=reuse->hits-before;
                    }
                    if(timed)view.detail_stats->pose_ticks+=source_clock_now()-start;
                }else if(!use_skin&&!use_authored_positions&&!cpu_pose_ready){
                    if(!pose_on_cpu())break;
                    cpu_pose_ready=true;
                }
                for(std::size_t entry=0;entry<(packed_pose?0:entries);++entry){auto compact=indexed||expand_pose?entry:triangles[entry];auto vi=cache?cache->topology.vertices[compact]:compact;
                    auto p=(use_authored_positions||use_skin)?authored_position(source->Position(vi)):positions[compact];
                    if(!billboard&&!performance)p=transformed(p,model_matrix);
                    if(reuse_attributes){const auto& a=cache->attributes[compact];batch.vertices.push_back({p[0],p[1],p[2],a.r,a.g,a.b,a.a,a.u,a.v});continue;}
                    Mth::Vector n(0,1,0,0);
                    if(need_normals)n=(billboard||performance)?normals[compact]:transformed(normals[compact],model_matrix);n[3]=0;
                    if(sky_&&!performance)for(unsigned c=0;c<3;++c)p[c]+=view.position[c];
                    float u=0,v=0;
                    if(batch.texture){if((pass.flags&8)&&!performance){float len=n.Length();if(len>0){float nx=0,ny=0;for(unsigned c=0;c<3;++c){nx+=n[c]/len*view.axes[0][c];ny+=n[c]/len*view.axes[1][c];}u=.5f+nx*pass.env_tiling[0]*.5f;v=.5f-ny*pass.env_tiling[1]*.5f;}}
                        // Some retail sectors (for example School II glass)
                        // share a textured material but export no coordinate
                        // stream. The original mesh loader accepts this
                        // format. Supply a constant coordinate on PICA.
                        else if(source->uv_sets){Dbg_MsgAssert(pi<source->uv_sets,("Original material %08x missing UV set %u",material->checksum,pi));const auto& uv=source->UV(std::size_t(vi)*source->uv_sets+pi);u=uv[0];v=uv[1];
                            if(!std::isfinite(u)||!std::isfinite(v)){
                                // Retail XBX uses these paired NaN markers in
                                // submitted texture streams. PICA needs finite
                                // UVs; use the texel origin for those pairs.
                                uint32 ubits,vbits;
                                std::memcpy(&ubits,&u,sizeof(u));
                                std::memcpy(&vbits,&v,sizeof(v));
                                const bool sentinel=ubits==vbits&&(ubits&0x7fffffff)>0x7f800000;
                                Dbg_MsgAssert(sentinel,("Non-finite UV sector %08x material %08x pass %u vertex %u: %08x/%08x",
                                    source->checksum,material->checksum,pi,unsigned(vi),ubits,vbits));
                                u=v=0;
                            }}}
                    if(change&&change->uv_set){auto uv=transformed(Mth::Vector(u,v,0,1),change->uv);u=uv[0];v=uv[1];}
                    else{auto offset=explicit_uv_?uv_offset_:(animated?animation.passes[pi].uv_offset:std::array<float,2>{0,0});u+=offset[0];v+=offset[1];}
                    Image::RGBA vc=unpacked(diffuse?(*diffuse)[vi]:source->Color(vi));auto w=source->ColorWibble(vi);if(w && current->frozen_scenery && w<=current->color_wibble.size()){vc=unpacked(current->color_wibble[w-1].keys.front().rgba);}
                    else if(w){Dbg_Assert(w<=animation.vertex_colors.size());auto a=animation.vertex_colors[w-1];vc=Image::RGBA(a[0],a[1],a[2],a[3]);}
                    auto rgb=pass.color;if(color_set_)rgb={color_.r/255.f,color_.g/255.f,color_.b/255.f};if(change&&change->color_set)rgb={change->color.r/255.f,change->color.g/255.f,change->color.b/255.f};
                    if(distant_color){rgb[0]*=((distant_color>>16)&255)/255.f;rgb[1]*=((distant_color>>8)&255)/255.f;rgb[2]*=(distant_color&255)/255.f;}
                    auto light=(!performance&&model_&&(source->flags&4))?SourcePort::LightNormal(*lighting,n):std::array<float,3>{1,1,1};
                    if(raw_colors){rgb={1,1,1};light={1,1,1};}
                    if(shard_lighting)batch.shard_lighting.push_back({n,{vc.r*rgb[0],vc.g*rgb[1],vc.b*rgb[2]}});
                    batch.vertices.push_back({p[0],p[1],p[2],byte(vc.r*rgb[0]*light[0]),byte(vc.g*rgb[1]*light[1]),byte(vc.b*rgb[2]*light[2]),batch.ignore_vertex_alpha?uint8(255):vc.a,u,v});
                    if(attributes){const auto& v=batch.vertices.back();cache->attributes[compact]={v.r,v.g,v.b,v.a,v.u,v.v};}
                }
                if(attributes)cache->attributes_revision=attributes_revision_;
                if(use_skin&&!batch.skin_mesh){
                    SourcePort::SkinTopology skin;
                    if(SourcePort::BuildSkinTopology(*source,cache->topology,bones_.size(),skin)){
                        SourceGpuMesh* gpu=nullptr;
                        std::size_t uploaded=batch.vertices.size();
                        if(local_player&&view.local_arrays&&triangles.size()<=65535){
                            // Moscow's local skater keeps the proven flat triangle order.
                            // Expand bind positions AND influences once, before animation.
                            // Both eyes reuse this immutable span with current bone matrices.
                            SourcePort::NativeHeapScope scratch;
                            std::vector<SourceRenderVertex> vertices;
                            std::vector<SourceSkinInfluence> influences;
                            std::vector<uint16> sequential;
                            uploaded=triangles.size();vertices.reserve(uploaded);
                            influences.reserve(uploaded);sequential.reserve(uploaded);
                            for(auto index:triangles){
                                vertices.push_back(batch.vertices[index]);
                                influences.push_back(skin.influences[index]);
                                sequential.push_back(uint16(sequential.size()));
                            }
                            gpu=source_graphics_skin_mesh_create(vertices.data(),influences.data(),uploaded,
                                                                 sequential.data(),uploaded);
                        }else if(!local_player||!view.local_arrays){
                            gpu=source_graphics_skin_mesh_create(batch.vertices.data(),skin.influences.data(),
                                uploaded,triangles.data(),triangles.size());
                        }
                        if(gpu){
                            SourcePort::RootHeapScope storage;
                            cache->skin=std::make_unique<SkinMesh>();
                            cache->skin->bones.assign(skin.bones.begin(),skin.bones.end());
                            cache->skin->gpu=gpu;
                            cache->skin->uploaded_vertices=uploaded;
                            cache->skin->attributes_revision=attributes_revision_;
                            if(cache->skin->UpdatePose(bones_)){
                                batch.skin_mesh=gpu;
                                batch.skin_transform=&model_matrix;
                                batch.skin_vertices=uploaded;
                                batch.indices=cache->indices;
                                batch.vertices.clear();
                                if(view.detail_stats)++view.detail_stats->skin_draws;
                            }else cache->skin.reset();
                        }
                    }else cache->unsupported_palette=bones_.size();
                    if(!batch.skin_mesh){
                        Dbg_Assert(pose_on_cpu());
                        for(std::size_t v=0;v<entries;++v){
                            batch.vertices[v].x=positions[v][0];
                            batch.vertices[v].y=positions[v][1];
                            batch.vertices[v].z=positions[v][2];
                        }
                    }
                }
                if(expand_pose&&!batch.skin_mesh){
                    batch.indices=cache->indices;
                    batch.expand_on_upload=true;
                }
                if(immutable){SourcePort::RootHeapScope storage;
                    if(!render_owner_)render_owner_=std::make_shared<const uint8>(0);
                    auto saved=std::make_shared<SourcePort::StaticWorldMesh>();
                    static_cast<SourcePort::WorldDrawState&>(*saved)=static_cast<const SourcePort::WorldDrawState&>(batch);
                    saved->distant_color=distant_color;
                    saved->vertices=std::move(batch.vertices);saved->indices=batch.indices;saved->owner=render_owner_;saved->serial=++static_mesh_serial;
                    if(view.prepare_source_strips && original_scenery && cache==full_cache && index_static &&
                       batch.opaque && batch.depth_write && !batch.pass && !batch.environment && !batch.z_bias &&
                       !hide_mask_ && material->passes.size()==1 && !material->grass && triangles.size()>=192){
                        const auto& strip=mesh.lods[cache->lod];
                        if(strip.size()<=SOURCE_DRAW_SLICE_ELEMENTS && strip.size()<triangles.size())
                            saved->source_strip=SourcePort::CompactSourceStrip(strip,cache->topology.vertices,triangles);
                    }
                    if(cull_mesh){
                        saved->box_center=full_cache->box_center;
                        saved->box_half_size=full_cache->box_half_size;
                    }
                    cache->retained=saved;cache->revision=render_revision_;batch.retained=std::move(saved);
                    if(view.detail_stats)++view.detail_stats->retained_builds;
                    if(retain_cpu)view.mesh_retention->Remember(batch.retained);
                    if(view.group_static_materials&&static_materials_&&batch.opaque)static_materials_->Bind(batch,cache->material);
                }
                if(local_player&&view.detail_stats){
                    ++view.detail_stats->local_parts;
                    view.detail_stats->local_array_parts+=!batch.GpuIndexed();
                    view.detail_stats->local_triangles+=triangles.size()/3;
                }
                out.push_back(std::move(batch));
            }
        }
    }
}
CGeom* CEngine::s_plat_init_geom(){return new CNativeGeom;}
bool CEngine::s_plat_uninit_geom(CGeom* geom){if(!geom)return false;delete geom;return true;}
}
